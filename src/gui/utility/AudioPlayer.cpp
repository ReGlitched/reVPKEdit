#include "AudioPlayer.h"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <QObject>

namespace {

ma_decoder g_decoder{};
ma_device g_device{};

std::atomic<bool> g_initialized{false};
std::atomic<std::int64_t> g_seekFrame{-1};
std::atomic<std::size_t> g_positionFrames{0};
std::atomic<std::size_t> g_lengthFrames{0}; 
std::atomic<std::uint32_t> g_sampleRate{0};
std::atomic<std::uint32_t> g_channels{0};

std::vector<std::byte> g_audioBytes;

std::mutex g_mutex;

void deinitUnlocked() {
	ma_device_stop(&g_device); 
	ma_device_uninit(&g_device);
	ma_decoder_uninit(&g_decoder);

	g_initialized.store(false, std::memory_order_release);
	g_seekFrame.store(-1, std::memory_order_release);
	g_positionFrames.store(0, std::memory_order_release);
	g_lengthFrames.store(0, std::memory_order_release);
	g_sampleRate.store(0, std::memory_order_release);
	g_channels.store(0, std::memory_order_release);
	g_audioBytes.clear();
}

void dataCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frameCount) {
	auto* decoder = static_cast<ma_decoder*>(device->pUserData);
	if (!decoder || !output) {
		return;
	}

	const std::int64_t seekTo = g_seekFrame.exchange(-1, std::memory_order_acq_rel);
	if (seekTo >= 0) {
		ma_decoder_seek_to_pcm_frame(decoder, static_cast<ma_uint64>(seekTo));
		const std::size_t len = g_lengthFrames.load(std::memory_order_acquire);
		const std::size_t pos = (len > 0) ? std::min<std::size_t>(static_cast<std::size_t>(seekTo), len) : static_cast<std::size_t>(seekTo);
		g_positionFrames.store(pos, std::memory_order_release);
	}

	ma_uint64 framesRead = 0;
	ma_decoder_read_pcm_frames(decoder, output, frameCount, &framesRead);

	if (framesRead < frameCount) {
		const ma_uint32 bpf = ma_get_bytes_per_frame(device->playback.format, device->playback.channels);
		std::byte* outBytes = static_cast<std::byte*>(output);
		std::memset(outBytes + static_cast<size_t>(framesRead) * bpf, 0, static_cast<size_t>(frameCount - framesRead) * bpf);
	}

	const std::size_t len = g_lengthFrames.load(std::memory_order_acquire);
	if (len > 0) {
		const std::size_t cur = g_positionFrames.load(std::memory_order_acquire);
		const std::size_t next = std::min<std::size_t>(len, cur + static_cast<std::size_t>(framesRead));
		g_positionFrames.store(next, std::memory_order_release);
	} else {
		g_positionFrames.fetch_add(static_cast<std::size_t>(framesRead), std::memory_order_acq_rel);
	}
}

static std::size_t computeLengthFramesByScan() {
	ma_decoder tmp{};
	if (ma_decoder_init_memory(g_audioBytes.data(), g_audioBytes.size(), nullptr, &tmp) != MA_SUCCESS) {
		return 0;
	}

	constexpr ma_uint64 kChunkFrames = 4096;
	std::vector<std::byte> scratch;
	scratch.resize(static_cast<size_t>(kChunkFrames) * ma_get_bytes_per_frame(tmp.outputFormat, tmp.outputChannels));

	ma_uint64 total = 0;
	while (true) {
		ma_uint64 rd = 0;
		ma_decoder_read_pcm_frames(&tmp, scratch.data(), kChunkFrames, &rd);
		if (rd == 0) {
			break;
		}
		total += rd;
	}

	ma_decoder_uninit(&tmp);
	return static_cast<std::size_t>(total);
}

} // namespace

bool AudioPlayer::initialized() {
	return g_initialized.load(std::memory_order_acquire);
}

QString AudioPlayer::initAudio(const void* data, std::size_t len) {
	if (!data || len == 0) {
		return QObject::tr("No audio data.");
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	if (initialized()) {
		deinitUnlocked();
	}

	g_audioBytes.resize(len);
	std::memcpy(g_audioBytes.data(), data, len);

	if (ma_decoder_init_memory(g_audioBytes.data(), g_audioBytes.size(), nullptr, &g_decoder) != MA_SUCCESS) {
		g_audioBytes.clear();
		return QObject::tr("Failed to initialize decoder.");
	}

	ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
	deviceConfig.playback.format = g_decoder.outputFormat;
	deviceConfig.playback.channels = g_decoder.outputChannels;
	deviceConfig.sampleRate = g_decoder.outputSampleRate;
	deviceConfig.dataCallback = dataCallback;
	deviceConfig.pUserData = &g_decoder;

	if (ma_device_init(nullptr, &deviceConfig, &g_device) != MA_SUCCESS) {
		ma_decoder_uninit(&g_decoder);
		g_audioBytes.clear();
		return QObject::tr("Failed to open playback device.");
	}

	g_sampleRate.store(g_decoder.outputSampleRate, std::memory_order_release);
	g_channels.store(g_decoder.outputChannels, std::memory_order_release);

	ma_uint64 pcmFrameCount = 0;
	std::size_t cachedLen = 0;
	if (ma_decoder_get_length_in_pcm_frames(&g_decoder, &pcmFrameCount) == MA_SUCCESS) {
		cachedLen = static_cast<std::size_t>(pcmFrameCount);
	} else {
		cachedLen = computeLengthFramesByScan();
	}
	g_lengthFrames.store(cachedLen, std::memory_order_release);

	g_initialized.store(true, std::memory_order_release);
	g_positionFrames.store(0, std::memory_order_release);
	g_seekFrame.store(-1, std::memory_order_release);

	return "";
}

void AudioPlayer::pause() {
	if (!initialized()) {
		return;
	}
	if (ma_device_stop(&g_device) != MA_SUCCESS) {
		deinitAudio();
	}
}

void AudioPlayer::play() {
	if (!initialized()) {
		return;
	}
	if (ma_device_start(&g_device) != MA_SUCCESS) {
		deinitAudio();
	}
}

void AudioPlayer::setVolume(float volume) {
	if (!initialized()) {
		return;
	}
	if (ma_device_set_master_volume(&g_device, volume) != MA_SUCCESS) {
		deinitAudio();
	}
}

std::size_t AudioPlayer::getPositionInFrames() {
	if (!initialized()) {
		return 0;
	}
	return g_positionFrames.load(std::memory_order_acquire);
}

double AudioPlayer::getPositionInSeconds() {
	const auto sr = getSampleRate();
	if (sr == 0) {
		return 0.0;
	}
	return static_cast<double>(getPositionInFrames()) / static_cast<double>(sr);
}

std::size_t AudioPlayer::getLengthInFrames() {
	if (!initialized()) {
		return 0;
	}
	return g_lengthFrames.load(std::memory_order_acquire);
}

double AudioPlayer::getLengthInSeconds() {
	const auto sr = getSampleRate();
	if (sr == 0) {
		return 0.0;
	}
	return static_cast<double>(getLengthInFrames()) / static_cast<double>(sr);
}

std::uint32_t AudioPlayer::getSampleRate() {
	if (!initialized()) {
		return 0;
	}
	return g_sampleRate.load(std::memory_order_acquire);
}

std::uint32_t AudioPlayer::getChannelCount() {
	if (!initialized()) {
		return 0;
	}
	return g_channels.load(std::memory_order_acquire);
}

void AudioPlayer::seekToFrame(std::int64_t frame) {
	if (!initialized()) {
		return;
	}
	if (frame < 0) {
		frame = 0;
	}
	const std::size_t len = getLengthInFrames();
	if (len > 0 && static_cast<std::size_t>(frame) > len) {
		frame = static_cast<std::int64_t>(len);
	}

	g_seekFrame.store(frame, std::memory_order_release);
	g_positionFrames.store(static_cast<std::size_t>(frame), std::memory_order_release);
}

void AudioPlayer::deinitAudio() {
	if (!initialized()) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	deinitUnlocked();
}

