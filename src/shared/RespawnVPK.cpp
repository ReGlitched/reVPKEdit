#include "RespawnVPK.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_set>

#include <FileStream.h>
#include <sourcepp/FS.h>
#include <sourcepp/String.h>
#include <sourcepp/crypto/CRC32.h>

#include "RespawnVPKManifest.h"

#ifdef VPKEDIT_HAVE_LZHAM
#include <lzham_bridge.h>
#endif

using namespace vpkpp;
using namespace sourcepp;

namespace {

constexpr std::uint32_t RESPAWN_VPK_SIGNATURE = 0x55AA1234u;
constexpr std::uint16_t RESPAWN_VPK_MAJOR_VERSION = 2;
constexpr std::uint16_t RESPAWN_VPK_MINOR_VERSION = 3;

constexpr std::uint16_t RESPAWN_CHUNK_END_MARKER = 0xFFFFu;
constexpr std::uint16_t RESPAWN_CHUNK_CONT_MARKER = 0x0000u;

constexpr std::size_t RESPAWN_VPK_HEADER_LEN = 16;

constexpr std::uint32_t LOAD_VISIBLE     = 1u << 0;
constexpr std::uint32_t LOAD_CACHE       = 1u << 8;
constexpr std::uint32_t LOAD_ACACHE_UNK0 = 1u << 10;

constexpr std::uint16_t TEXTURE_DEFAULT = 1u << 3;

constexpr std::size_t DEFAULT_MAX_PART_SIZE = 1024 * 1024;
constexpr std::size_t DEFAULT_COMPRESSION_THRESHOLD = 4096;

struct CamEntry {
	std::uint32_t magic = 3302889984u;
	std::uint32_t originalSize = 0;
	std::uint32_t compressedSize = 0;
	std::uint32_t sampleRate = 0;
	std::uint8_t channels = 0;
	std::uint32_t sampleCount = 0;
	std::uint32_t headerSize = 44;
	std::uint64_t vpkContentOffset = 0;
	std::string path;
};

struct WriteBuffer {
	std::vector<std::byte> buf;

	explicit WriteBuffer(std::size_t reserve = 0) {
		buf.reserve(reserve);
	}

	void writeBytes(std::span<const std::byte> b) {
		if (b.empty()) return;
		buf.insert(buf.end(), b.begin(), b.end());
	}

	void writeU8(std::uint8_t v) {
		buf.push_back(static_cast<std::byte>(v));
	}

	void writeU16(std::uint16_t v) {
		writeU8(static_cast<std::uint8_t>(v & 0xFF));
		writeU8(static_cast<std::uint8_t>((v >> 8) & 0xFF));
	}

	void writeU24(std::uint32_t v) {
		writeU8(static_cast<std::uint8_t>(v & 0xFF));
		writeU8(static_cast<std::uint8_t>((v >> 8) & 0xFF));
		writeU8(static_cast<std::uint8_t>((v >> 16) & 0xFF));
	}

	void writeU32(std::uint32_t v) {
		writeU8(static_cast<std::uint8_t>(v & 0xFF));
		writeU8(static_cast<std::uint8_t>((v >> 8) & 0xFF));
		writeU8(static_cast<std::uint8_t>((v >> 16) & 0xFF));
		writeU8(static_cast<std::uint8_t>((v >> 24) & 0xFF));
	}

	void writeU64(std::uint64_t v) {
		writeU32(static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
		writeU32(static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
	}

	void writeCString(std::string_view s) {
		writeBytes(std::span<const std::byte>{reinterpret_cast<const std::byte*>(s.data()), s.size()});
		writeU8(0);
	}
};

static std::optional<CamEntry> tryMakeCamEntry(const std::vector<std::byte>& wavFile, const std::string& path) {
	if (wavFile.size() < 44) {
		return std::nullopt;
	}
	const auto* b = reinterpret_cast<const std::uint8_t*>(wavFile.data());
	if (!(b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F')) {
		return std::nullopt;
	}
	if (!(b[8] == 'W' && b[9] == 'A' && b[10] == 'V' && b[11] == 'E')) {
		return std::nullopt;
	}

	auto readU16LE = [&](std::size_t off) -> std::uint16_t {
		return static_cast<std::uint16_t>(b[off] | (static_cast<std::uint16_t>(b[off + 1]) << 8));
	};
	auto readU32LE = [&](std::size_t off) -> std::uint32_t {
		return static_cast<std::uint32_t>(b[off])
			| (static_cast<std::uint32_t>(b[off + 1]) << 8)
			| (static_cast<std::uint32_t>(b[off + 2]) << 16)
			| (static_cast<std::uint32_t>(b[off + 3]) << 24);
	};

	const auto sampleRate = readU32LE(24);
	const auto channels = readU16LE(22);
	const auto blockAlign = readU16LE(32);
	const auto dataLength = readU32LE(40);
	if (channels == 0 || blockAlign == 0) {
		return std::nullopt;
	}
	const auto sampleCount = dataLength / blockAlign;

	CamEntry out;
	out.originalSize = static_cast<std::uint32_t>(wavFile.size());
	out.compressedSize = static_cast<std::uint32_t>(wavFile.size());
	out.sampleRate = sampleRate;
	out.channels = static_cast<std::uint8_t>(channels & 0xFF);
	out.sampleCount = sampleCount;
	out.headerSize = 44;
	out.path = path;
	return out;
}

static void stripWavHeaderInPlace(std::vector<std::byte>& file) {
	if (file.size() < 44) {
		return;
	}
	auto* b = reinterpret_cast<std::uint8_t*>(file.data());
	if (b[0] == 0xCB && b[1] == 0xCB && b[2] == 0xCB && b[3] == 0xCB) {
		return;
	}
	std::fill_n(b, 44, 0xCB);
}

} // namespace

std::unique_ptr<PackFile> RespawnVPK::open(const std::string& path, const EntryCallback& callback) {
	(void) callback;
	std::error_code ec;
	if (!std::filesystem::is_regular_file(path, ec)) {
		return nullptr;
	}

	std::ifstream f{path, std::ios::binary};
	if (!f) {
		return nullptr;
	}

	std::uint32_t treeLength = 0;
	if (!RespawnVPK::readAndValidateHeader(f, treeLength)) {
		return nullptr;
	}

	auto* vpk = new RespawnVPK{path};
	std::unique_ptr<PackFile> packFile{vpk};

	// Parse nested null-terminated strings: ext -> dir -> filename -> entry
	while (true) {
		std::string extension = RespawnVPK::readCString(f);
		if (!f || extension.empty()) {
			break;
		}

		while (true) {
			std::string directory = RespawnVPK::readCString(f);
			if (!f || directory.empty()) {
				break;
			}

			while (true) {
				std::string filename = RespawnVPK::readCString(f);
				if (!f || filename.empty()) {
					break;
				}

				std::string fullPath = filename;
				if (fullPath == " ") {
					fullPath.clear();
				}
				if (extension != " ") {
					fullPath += '.';
					fullPath += extension;
				}
				if (directory != " ") {
					if (!directory.empty()) {
						fullPath = directory + '/' + fullPath;
					}
				}

				// Entry:
				//   u32 crc
				//   u16 preloadBytes
				//   file parts:
				//     u16 archiveIndex (0xFFFF terminates the list)
				//     u16 loadFlags
				//     u32 textureFlags
				//     u64 entryOffset
				//     u64 entryLength
				//     u64 entryLengthUncompressed
				const auto crc32 = RespawnVPK::readU32(f);
				const auto preloadBytes = RespawnVPK::readU16(f);
				if (!f) {
					return nullptr;
				}

				MetaEntry meta;
				meta.crc32 = crc32;
				meta.preloadBytes = preloadBytes;

				while (true) {
					FilePart part;
					part.archiveIndex = RespawnVPK::readU16(f);
					if (!f) {
						return nullptr;
					}
					if (part.archiveIndex == RESPAWN_CHUNK_END_MARKER) {
						break;
					}

					part.loadFlags = RespawnVPK::readU16(f);
					part.textureFlags = RespawnVPK::readU32(f);
					part.entryOffset = RespawnVPK::readU64(f);
					part.entryLength = RespawnVPK::readU64(f);
					part.entryLengthUncompressed = RespawnVPK::readU64(f);
					if (!f) {
						return nullptr;
					}
					meta.parts.push_back(part);
				}

				// Preload bytes (if any) are stored inline in the directory VPK immediately after the chunk list.
				// If we don't skip them here, the directory tree parsing desyncs and the open fails (TF2 uses preloads heavily).
				if (meta.preloadBytes) {
					meta.preloadOffset = static_cast<std::uint64_t>(f.tellg());
					f.seekg(static_cast<std::streamoff>(meta.preloadBytes), std::ios::cur);
					if (!f) {
						return nullptr;
					}
				}

				Entry entry = createNewEntry();
				entry.crc32 = meta.crc32;

				std::uint64_t dataLen = 0;
				dataLen += meta.preloadBytes;
				for (const auto& part : meta.parts) {
					dataLen += part.entryLengthUncompressed;
				}
				entry.length = dataLen;

				if (!meta.parts.empty()) {
					entry.archiveIndex = meta.parts.front().archiveIndex;
				}

				auto cleanPath = vpk->cleanEntryPath(fullPath);
				vpk->metaEntries.emplace(cleanPath, std::move(meta));
				vpk->entries.insert(cleanPath, std::move(entry));
			}
		}
	}

	return packFile;
}

std::optional<std::vector<std::byte>> RespawnVPK::readEntry(const std::string& path_) const {
	this->lastError.clear();

	const auto cleanPath = this->cleanEntryPath(path_);

	if (const auto entry = this->findEntry(cleanPath, true)) {
		if (entry->unbaked) {
			return readUnbakedEntry(*entry);
		}
	}

	const auto metaIt = this->metaEntries.find(cleanPath);
	if (metaIt == this->metaEntries.end()) {
		this->lastError = "entry not found in Respawn VPK tree";
		return std::nullopt;
	}

	const auto& meta = metaIt->second;

	// Basic sanity limits to avoid crashing on malformed packed VPKs
	// These are intentionally conservative; assets should be well below this
	constexpr std::uint64_t MAX_ENTRY_UNCOMPRESSED = 1024ull * 1024ull * 1024ull;
	constexpr std::uint64_t MAX_PART_COMPRESSED = 512ull * 1024ull * 1024ull;
	constexpr std::uint64_t MAX_PART_UNCOMPRESSED = 512ull * 1024ull * 1024ull;

	std::vector<std::byte> out;
	{
		std::uint64_t total = 0;
		total += meta.preloadBytes;
		for (const auto& part : meta.parts) {
			if (part.entryLength > MAX_PART_COMPRESSED) {
				this->lastError = "archive part too large (compressed length)";
				return std::nullopt;
			}
			if (part.entryLengthUncompressed > MAX_PART_UNCOMPRESSED) {
				this->lastError = "archive part too large (uncompressed length)";
				return std::nullopt;
			}
			total += part.entryLengthUncompressed;
			if (total > MAX_ENTRY_UNCOMPRESSED) {
				this->lastError = "entry too large (uncompressed)";
				return std::nullopt;
			}
		}
		try {
			out.reserve(static_cast<std::size_t>(total));
		} catch (...) {
			this->lastError = "failed to allocate output buffer for entry";
			return std::nullopt;
		}
	}

	// Preload bytes are stored inline in the directory VPK.
	if (meta.preloadBytes) {
		const auto preload = RespawnVPK::readFileRange(std::string{this->fullFilePath}, meta.preloadOffset, meta.preloadBytes);
		if (!preload) {
			this->lastError = "failed to read preload bytes from directory VPK";
			return std::nullopt;
		}
		out.insert(out.end(), preload->begin(), preload->end());
	}

	for (const auto& part : meta.parts) {
		const auto archivePath = RespawnVPK::buildArchivePath(std::string{this->fullFilePath}, part.archiveIndex);
		const auto compressed = RespawnVPK::readFileRange(archivePath, part.entryOffset, static_cast<std::size_t>(part.entryLength));
		if (!compressed) {
			this->lastError = "failed to read archive part from: " + archivePath;
			return std::nullopt;
		}

		if (!part.isCompressed()) {
			out.insert(out.end(), compressed->begin(), compressed->end());
			continue;
		}

#ifdef VPKEDIT_HAVE_LZHAM
		const auto decompressed = RespawnVPK::lzhamDecompress(compressed->data(), compressed->size(), static_cast<std::size_t>(part.entryLengthUncompressed));
		if (!decompressed) {
			this->lastError = "failed to LZHAM decompress chunk (archiveIndex=" + std::to_string(part.archiveIndex) + ")";
			return std::nullopt;
		}
		out.insert(out.end(), decompressed->begin(), decompressed->end());
#else
		(void) part;
		this->lastError = "this entry is LZHAM compressed, but vpkedit was built without LZHAM support";
		return std::nullopt;
#endif
	}

	return out;
}

bool RespawnVPK::extractEntryToFile(const std::string& entryPath, const std::string& filepath, std::string* outError) const {
	this->lastError.clear();

	const auto cleanPath = this->cleanEntryPath(entryPath);
	if (const auto entry = this->findEntry(cleanPath, true)) {
		if (entry->unbaked) {
			const auto data = readUnbakedEntry(*entry);
			if (!data) {
				this->lastError = "failed to read unbaked entry data";
				if (outError) *outError = this->lastError;
				return false;
			}
			FileStream out{filepath, FileStream::OPT_TRUNCATE | FileStream::OPT_CREATE_IF_NONEXISTENT};
			if (!out) {
				this->lastError = "failed to open output path for write: " + filepath;
				if (outError) *outError = this->lastError;
				return false;
			}
			out.write(std::span<const std::byte>{data->data(), data->size()});
			return true;
		}
	}

	const auto metaIt = this->metaEntries.find(cleanPath);
	if (metaIt == this->metaEntries.end()) {
		this->lastError = "entry not found in Respawn VPK tree";
		if (outError) *outError = this->lastError;
		return false;
	}
	const auto& meta = metaIt->second;

	FileStream out{filepath, FileStream::OPT_TRUNCATE | FileStream::OPT_CREATE_IF_NONEXISTENT};
	if (!out) {
		this->lastError = "failed to open output path for write: " + filepath;
		if (outError) *outError = this->lastError;
		return false;
	}

	auto streamCopyRange = [&](const std::string& srcPath, std::uint64_t offset, std::uint64_t length) -> bool {
		std::ifstream f{srcPath, std::ios::binary};
		if (!f) {
			this->lastError = "failed to open archive file: " + srcPath;
			return false;
		}
		f.seekg(0, std::ios::end);
		const auto endPos = f.tellg();
		if (endPos < 0) {
			this->lastError = "failed to stat archive file: " + srcPath;
			return false;
		}
		const auto fileSize = static_cast<std::uint64_t>(endPos);
		if (offset > fileSize || length > (fileSize - offset)) {
			this->lastError = "archive part range out of bounds: " + srcPath;
			return false;
		}
		f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		if (!f) {
			this->lastError = "failed to seek archive file: " + srcPath;
			return false;
		}

		// do NOT use a large stack buffer here; this runs on a QT worker thread
		// A big stack allocation will hard-crash with stack overflow
		std::vector<char> buf;
		buf.resize(256 * 1024);
		std::uint64_t remaining = length;
		while (remaining) {
			const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buf.size())));
			f.read(buf.data(), static_cast<std::streamsize>(chunk));
			if (!f) {
				this->lastError = "failed to read archive bytes from: " + srcPath;
				return false;
			}
			out.write(std::span<const std::byte>{reinterpret_cast<const std::byte*>(buf.data()), chunk});
			remaining -= chunk;
		}
		return true;
	};

	// Preload bytes (if any) are stored inline in the directory VPK and must be written first.
	if (meta.preloadBytes) {
		if (!streamCopyRange(std::string{this->fullFilePath}, meta.preloadOffset, meta.preloadBytes)) {
			if (outError) *outError = this->lastError;
			return false;
		}
	}

	for (const auto& part : meta.parts) {
		const auto archivePath = RespawnVPK::buildArchivePath(std::string{this->fullFilePath}, part.archiveIndex);

		if (!part.isCompressed()) {
			if (!streamCopyRange(archivePath, part.entryOffset, part.entryLength)) {
				if (outError) *outError = this->lastError;
				return false;
			}
			continue;
		}

#ifdef VPKEDIT_HAVE_LZHAM
		// For compressed parts we still need a contiguous input/output buffer for LZHAM
		// This is usually fine because parts are typically small; this avoids allocating the full entry
		const auto compressed = RespawnVPK::readFileRange(archivePath, part.entryOffset, static_cast<std::size_t>(part.entryLength));
		if (!compressed) {
			this->lastError = "failed to read archive part from: " + archivePath;
			if (outError) *outError = this->lastError;
			return false;
		}

		const auto decompressed = RespawnVPK::lzhamDecompress(compressed->data(), compressed->size(), static_cast<std::size_t>(part.entryLengthUncompressed));
		if (!decompressed) {
			this->lastError = "failed to LZHAM decompress chunk (archiveIndex=" + std::to_string(part.archiveIndex) + ")";
			if (outError) *outError = this->lastError;
			return false;
		}

		out.write(std::span<const std::byte>{decompressed->data(), decompressed->size()});
#else
		this->lastError = "this entry is LZHAM compressed, but vpkedit was built without LZHAM support";
		if (outError) *outError = this->lastError;
		return false;
#endif
	}

	return true;
}

Attribute RespawnVPK::getSupportedEntryAttributes() const {
	using enum Attribute;
	return LENGTH | VPK_PRELOADED_DATA | ARCHIVE_INDEX | CRC32;
}

void RespawnVPK::addEntryInternal(vpkpp::Entry& entry, const std::string& path, std::vector<std::byte>& buffer, vpkpp::EntryOptions) {
	// Respawn VPK doesnt use the Valve VPK preload feature  ignore EntryOptions::vpk_preloadBytes
	entry.extraData.clear();
	entry.crc32 = crypto::computeCRC32(std::span<const std::byte>{buffer.data(), buffer.size()});
	entry.length = buffer.size();
	entry.compressedLength = 0;
	entry.offset = 0;
	// For display: treat new/unbaked entries as going into the patch archive (999)
	entry.archiveIndex = 999;
	entry.flags = 0;

	// Preserve per-entry flags if replacing an existing entry; otherwise choose defaults based on extension
	std::uint32_t loadFlags = static_cast<std::uint32_t>(LOAD_VISIBLE | LOAD_CACHE);
	std::uint32_t textureFlags = 0;

	if (const auto it = this->metaEntries.find(path); it != this->metaEntries.end()) {
		if (!it->second.parts.empty()) {
			loadFlags = it->second.parts.front().loadFlags;
			textureFlags = it->second.parts.front().textureFlags;
		}
	} else {
		std::string extLower;
		{
			auto ext = std::filesystem::path{path}.extension().string();
			if (!ext.empty() && ext[0] == '.') {
				ext.erase(0, 1);
			}
			for (auto& c : ext) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			extLower = std::move(ext);
		}
		if (extLower == "wav") {
			loadFlags = static_cast<std::uint32_t>(LOAD_VISIBLE | LOAD_CACHE);
		} else if (extLower == "acache") {
			loadFlags = static_cast<std::uint32_t>(LOAD_VISIBLE | LOAD_CACHE | LOAD_ACACHE_UNK0);
		} else {
			loadFlags = static_cast<std::uint32_t>(LOAD_VISIBLE | LOAD_CACHE);
		}
		if (extLower == "vtf") {
			textureFlags = TEXTURE_DEFAULT;
		}
	}

	this->unbakedFlags[path] = {loadFlags, textureFlags};
}

bool RespawnVPK::renameEntry(const std::string& oldPath_, const std::string& newPath_) {
	const auto oldPath = this->cleanEntryPath(oldPath_);
	const auto newPath = this->cleanEntryPath(newPath_);
	const bool ok = PackFile::renameEntry(oldPath, newPath);
	if (!ok) {
		return false;
	}
	if (const auto it = this->metaEntries.find(oldPath); it != this->metaEntries.end()) {
		auto meta = std::move(it->second);
		this->metaEntries.erase(it);
		this->metaEntries.emplace(newPath, std::move(meta));
	}
	if (const auto it = this->unbakedFlags.find(oldPath); it != this->unbakedFlags.end()) {
		auto flags = it->second;
		this->unbakedFlags.erase(it);
		this->unbakedFlags.emplace(newPath, flags);
	}
	return true;
}

bool RespawnVPK::renameDirectory(const std::string& oldDir_, const std::string& newDir_) {
	const auto oldDir = this->cleanEntryPath(oldDir_) + '/';
	const auto newDir = this->cleanEntryPath(newDir_) + '/';

	// Update pack entries first
	const bool ok = PackFile::renameDirectory(oldDir_, newDir_);
	if (!ok) {
		return false;
	}

	// Update meta entries
	std::vector<std::pair<std::string, MetaEntry>> moved;
	for (auto it = this->metaEntries.begin(); it != this->metaEntries.end(); ++it) {
		if (it->first.rfind(oldDir, 0) == 0) {
			moved.emplace_back(newDir + it->first.substr(oldDir.size()), std::move(it->second));
		}
	}
	// Erase after collecting to avoid iterator invalidation
	for (auto it = this->metaEntries.begin(); it != this->metaEntries.end(); ) {
		if (it->first.rfind(oldDir, 0) == 0) {
			it = this->metaEntries.erase(it);
		} else {
			++it;
		}
	}
	for (auto& [newPath, meta] : moved) {
		this->metaEntries.emplace(std::move(newPath), std::move(meta));
	}

	// Update unbaked flags for renamed paths
	std::vector<std::pair<std::string, std::pair<std::uint32_t, std::uint16_t>>> movedFlags;
	for (auto it = this->unbakedFlags.begin(); it != this->unbakedFlags.end(); ++it) {
		if (it->first.rfind(oldDir, 0) == 0) {
			movedFlags.emplace_back(newDir + it->first.substr(oldDir.size()), it->second);
		}
	}
	for (auto it = this->unbakedFlags.begin(); it != this->unbakedFlags.end(); ) {
		if (it->first.rfind(oldDir, 0) == 0) {
			it = this->unbakedFlags.erase(it);
		} else {
			++it;
		}
	}
	for (auto& [newPath, flags] : movedFlags) {
		this->unbakedFlags.emplace(std::move(newPath), flags);
	}
	return true;
}

bool RespawnVPK::removeEntry(const std::string& path_) {
	const auto path = this->cleanEntryPath(path_);
	this->metaEntries.erase(path);
	this->unbakedFlags.erase(path);
	return PackFile::removeEntry(path_);
}

std::size_t RespawnVPK::removeDirectory(const std::string& dirName_) {
	auto dirName = this->cleanEntryPath(dirName_);
	if (!dirName.empty()) {
		dirName += '/';
	}
	for (auto it = this->metaEntries.begin(); it != this->metaEntries.end(); ) {
		if (dirName.empty()) {
			it = this->metaEntries.erase(it);
			continue;
		}
		if (it->first.rfind(dirName, 0) == 0) {
			it = this->metaEntries.erase(it);
		} else {
			++it;
		}
	}
	for (auto it = this->unbakedFlags.begin(); it != this->unbakedFlags.end(); ) {
		if (dirName.empty()) {
			it = this->unbakedFlags.erase(it);
			continue;
		}
		if (it->first.rfind(dirName, 0) == 0) {
			it = this->unbakedFlags.erase(it);
		} else {
			++it;
		}
	}
	return PackFile::removeDirectory(dirName_);
}

static std::string toLower(std::string s) {
	for (auto& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

static std::string getExtensionLower(std::string_view path) {
	auto p = std::filesystem::path{path};
	auto ext = p.extension().string();
	if (!ext.empty() && ext[0] == '.') {
		ext.erase(0, 1);
	}
	return ::toLower(ext);
}

std::string RespawnVPK::makeArchivePathForWrite(const std::string& dirVpkPath, std::uint16_t archiveIndex) {
	std::string base = RespawnVPK::stripPakLangFilenamePrefix(dirVpkPath);

	{
		constexpr std::string_view suffix = "_dir.vpk";
		if (base.size() >= suffix.size()) {
			bool matches = true;
			for (std::size_t i = 0; i < suffix.size(); i++) {
				const auto a = static_cast<unsigned char>(base[base.size() - suffix.size() + i]);
				const auto b = static_cast<unsigned char>(suffix[i]);
				if (std::tolower(a) != std::tolower(b)) {
					matches = false;
					break;
				}
			}
			if (matches) {
				char buf[16];
				std::snprintf(buf, sizeof(buf), "_%03u.vpk", static_cast<unsigned>(archiveIndex));
				base.replace(base.size() - suffix.size(), suffix.size(), buf);
			}
		}
	}

	return base;
}

std::vector<std::byte> RespawnVPK::lzhamCompress(const std::byte* src, std::size_t srcLen) {
#ifdef VPKEDIT_HAVE_LZHAM
	std::vector<std::byte> out(std::max<std::size_t>(srcLen, 1));
	for (int tries = 0; tries < 6; tries++) {
		size_t outLen = out.size();
		const auto rc = lzham_bridge_compress(
			reinterpret_cast<const std::uint8_t*>(src), srcLen,
			reinterpret_cast<std::uint8_t*>(out.data()), &outLen);

		if (rc == 0) {
			out.resize(outLen);
			return out;
		}
		if (rc == 3) {
			const auto next = std::min<std::size_t>(std::max<std::size_t>(out.size() * 2, 1024), 128 * 1024 * 1024);
			if (next <= out.size()) {
				break;
			}
			out.resize(next);
			continue;
		}
		break;
	}
	return std::vector<std::byte>{src, src + srcLen};
#else
	return std::vector<std::byte>{src, src + srcLen};
#endif
}

bool RespawnVPK::bake(const std::string& outputDir_, vpkpp::BakeOptions, const EntryCallback& callback) {
	this->lastError.clear();

	// Respawn VPKs write updated *_dir.vpk and (optionally) a patch archive *_999.vpk with modified/new files
	const std::string outputDir = this->getBakeOutputDir(outputDir_);
	const std::string outDirVpkPath = outputDir + '/' + this->getFilename();

	// Load manifest (optional); used to determine flags and packing knobs per entry
	const auto manifestOpt = respawn_vpk::readManifestForDirVpkPath(std::filesystem::path{outDirVpkPath});
	const respawn_vpk::ManifestMap* manifest = manifestOpt ? &*manifestOpt : nullptr;

	// Collect entries: baked first, then unbaked overrides if same key
	struct Item {
		vpkpp::Entry* entry = nullptr;
		bool unbaked = false;
	};
	std::unordered_map<std::string, Item> items;
	items.reserve(this->entries.size() + this->unbakedEntries.size());
	{
		std::string key;
		for (auto it = this->entries.begin(); it != this->entries.end(); ++it) {
			it.key(key);
			items.emplace(key, Item{&it.value(), false});
		}
		for (auto it = this->unbakedEntries.begin(); it != this->unbakedEntries.end(); ++it) {
			it.key(key);
			items[key] = Item{&it.value(), true};
		}
	}

	struct TreeItem {
		std::string path;
		std::string ext;
		std::string dir;
		std::string fileStem;
		MetaEntry meta;
		bool inPatchArchive = false;
	};

	std::vector<TreeItem> treeItems;
	treeItems.reserve(items.size());

	constexpr std::uint16_t PATCH_ARCHIVE_INDEX = 999;

	std::vector<std::byte> patchArchive;
	std::vector<CamEntry> patchCams;

	// Deduplicate new patch data globally across this bake, matching revpk behavior at a coarse level
	// Key is (crc32 << 32) | size, values are absolute file offsets into the patch archive
	std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> patchDedup;
	patchDedup.reserve(1024);

	std::unordered_set<std::uint16_t> referencedArchives;
	referencedArchives.reserve(16);

	// If any baked entry already references the patch archive index, we must preserve the existing patch archive
	// (and append new data), otherwise we invalidate stored offsets for unchanged patch entries
	bool preserveExistingPatchArchive = false;
	for (const auto& [path, item] : items) {
		if (!item.entry || item.unbaked) {
			continue;
		}
		const auto metaIt = this->metaEntries.find(path);
		if (metaIt == this->metaEntries.end()) {
			continue;
		}
		if (metaIt->second.archiveIndex == PATCH_ARCHIVE_INDEX) {
			preserveExistingPatchArchive = true;
			break;
		}
	}

	const auto srcPatchArchivePath = RespawnVPK::buildArchivePath(std::string{this->fullFilePath}, PATCH_ARCHIVE_INDEX);
	const auto dstPatchArchivePath = RespawnVPK::makeArchivePathForWrite(outDirVpkPath, PATCH_ARCHIVE_INDEX);
	const auto srcPatchCamPath = srcPatchArchivePath + ".cam";
	const auto dstPatchCamPath = dstPatchArchivePath + ".cam";

	std::uint64_t patchOffset = 0;
	if (preserveExistingPatchArchive) {
		std::error_code ec;
		ec.clear();
		std::filesystem::create_directories(std::filesystem::path{dstPatchArchivePath}.parent_path(), ec);

		if (!outputDir.empty()) {
			ec.clear();
			if (std::filesystem::is_regular_file(srcPatchArchivePath, ec)) {
				ec.clear();
				std::filesystem::copy_file(srcPatchArchivePath, dstPatchArchivePath, std::filesystem::copy_options::overwrite_existing, ec);
			}
			ec.clear();
			if (std::filesystem::is_regular_file(srcPatchCamPath, ec)) {
				ec.clear();
				std::filesystem::copy_file(srcPatchCamPath, dstPatchCamPath, std::filesystem::copy_options::overwrite_existing, ec);
			}
		}

		ec.clear();
		if (std::filesystem::is_regular_file(dstPatchArchivePath, ec)) {
			patchOffset = static_cast<std::uint64_t>(std::filesystem::file_size(dstPatchArchivePath, ec));
		}
	}

	for (auto& [path, item] : items) {
		if (!item.entry) {
			continue;
		}

		TreeItem out;
		out.path = path;

		const auto fsPath = std::filesystem::path{path};
		const auto extLower = ::getExtensionLower(path);
		const auto filename = fsPath.filename().string();
		std::string dir = fsPath.parent_path().string();
		sourcepp::string::normalizeSlashes(dir, true, true);

		out.ext = extLower.empty() ? " " : extLower;
		out.dir = dir.empty() ? " " : dir;
		if (out.ext == " ") {
			out.fileStem = filename;
		} else {
			out.fileStem = fsPath.stem().string();
		}

		if (!item.unbaked) {
			const auto metaIt = this->metaEntries.find(path);
			if (metaIt == this->metaEntries.end()) {
				this->lastError = "missing Respawn metadata for baked entry: " + path;
				return false;
			}
			out.meta = metaIt->second;

			// If a manifest exists, it is authoritative for flags and preloadSize
			if (manifest) {
				const auto mkey = respawn_vpk::normalizeManifestPath(path);
				if (const auto it = manifest->find(mkey); it != manifest->end()) {
					out.meta.preloadBytes = it->second.preloadSize;
					for (auto& p : out.meta.parts) {
						p.loadFlags = it->second.loadFlags;
						p.textureFlags = it->second.textureFlags;
					}
				}
			}

			for (const auto& p : out.meta.parts) {
				referencedArchives.insert(p.archiveIndex);
			}
			treeItems.push_back(std::move(out));
			continue;
		}

		// Unbaked entry: encode into patch archive
		const auto data = readUnbakedEntry(*item.entry);
		if (!data) {
			this->lastError = "failed to read unbaked entry data: " + path;
			return false;
		}

		auto file = *data;

		// WAV handling: generate cam metadata and overwrite RIFF header
		if (out.ext == "wav") {
			if (auto cam = ::tryMakeCamEntry(file, out.path)) {
				patchCams.push_back(*cam);
			}
			::stripWavHeaderInPlace(file);
		}

		out.meta.crc32 = crypto::computeCRC32(std::span<const std::byte>{file.data(), file.size()});
		out.inPatchArchive = true;

		// Choose per-entry values (manifest > explicitly tracked flags > preserve > defaults)
		std::uint32_t loadFlags = static_cast<std::uint32_t>(LOAD_VISIBLE | LOAD_CACHE);
		std::uint32_t textureFlags = 0;
		std::uint16_t preloadSize = 0;
		bool useCompression = true;
		bool deDuplicate = true;
		bool manifestMatched = false;

		if (manifest) {
			const auto mkey = respawn_vpk::normalizeManifestPath(path);
			if (const auto it = manifest->find(mkey); it != manifest->end()) {
				loadFlags = it->second.loadFlags;
				textureFlags = it->second.textureFlags;
				preloadSize = it->second.preloadSize;
				useCompression = it->second.useCompression;
				deDuplicate = it->second.deDuplicate;
				manifestMatched = true;
			}
		}
		if (!manifestMatched) {
			if (const auto it = this->unbakedFlags.find(path); it != this->unbakedFlags.end()) {
				loadFlags = it->second.first;
				textureFlags = it->second.second;
			} else if (const auto metaIt = this->metaEntries.find(path); metaIt != this->metaEntries.end()) {
				if (!metaIt->second.parts.empty()) {
					loadFlags = metaIt->second.parts.front().loadFlags;
					textureFlags = metaIt->second.parts.front().textureFlags;
				}
			} else {
				loadFlags = static_cast<std::uint32_t>(LOAD_VISIBLE | LOAD_CACHE);
				if (out.ext == "acache") {
					loadFlags |= LOAD_ACACHE_UNK0;
				}
				if (out.ext == "vtf") {
					textureFlags = TEXTURE_DEFAULT;
				}
			}
		}

		out.meta.preloadBytes = preloadSize;

		// Split into parts (and optionally compress)
		std::size_t fileOff = 0;
		while (fileOff < file.size()) {
			const auto partLen = std::min<std::size_t>(DEFAULT_MAX_PART_SIZE, file.size() - fileOff);
			const auto partSpan = std::span<const std::byte>{file.data() + fileOff, partLen};

			std::vector<std::byte> partData(partSpan.begin(), partSpan.end());
			bool doCompress = partLen >= DEFAULT_COMPRESSION_THRESHOLD && out.ext != "wav" && out.ext != "vtf";
			if (manifestMatched) {
				// Manifest is authoritative. Still keep the usual exclusions
				doCompress = useCompression && out.ext != "wav" && out.ext != "vtf";
			}
			if (doCompress) {
				const auto compressed = RespawnVPK::lzhamCompress(partSpan.data(), partSpan.size());
				if (compressed.size() < partData.size()) {
					partData = compressed;
				}
			}

			FilePart p;
			p.archiveIndex = PATCH_ARCHIVE_INDEX;
			p.loadFlags = loadFlags;
			p.textureFlags = textureFlags;
			p.entryLength = static_cast<std::uint64_t>(partData.size());
			p.entryLengthUncompressed = static_cast<std::uint64_t>(partLen);

			// Deduplicate stored bytes if enabled
			if (deDuplicate && !partData.empty()) {
				const auto crc = crypto::computeCRC32(std::span<const std::byte>{partData.data(), partData.size()});
				const std::uint64_t h = (static_cast<std::uint64_t>(crc) << 32) | static_cast<std::uint64_t>(partData.size());
				bool reused = false;
				if (auto it = patchDedup.find(h); it != patchDedup.end()) {
					const auto baseOffset = patchOffset - static_cast<std::uint64_t>(patchArchive.size());
					for (const auto off : it->second) {
						if (off < baseOffset) {
							continue;
						}
						const auto rel = static_cast<std::size_t>(off - baseOffset);
						if (rel + partData.size() > patchArchive.size()) {
							continue;
						}
						if (std::memcmp(patchArchive.data() + rel, partData.data(), partData.size()) == 0) {
							p.entryOffset = off;
							reused = true;
							break;
						}
					}
				}
				if (!reused) {
					p.entryOffset = patchOffset;
					patchDedup[h].push_back(p.entryOffset);
					patchArchive.insert(patchArchive.end(), partData.begin(), partData.end());
					patchOffset += static_cast<std::uint64_t>(partData.size());
				}
			} else {
				p.entryOffset = patchOffset;
				patchArchive.insert(patchArchive.end(), partData.begin(), partData.end());
				patchOffset += static_cast<std::uint64_t>(partData.size());
			}

			out.meta.parts.push_back(p);
			fileOff += partLen;
		}

		referencedArchives.insert(PATCH_ARCHIVE_INDEX);
		treeItems.push_back(std::move(out));
	}

	// Copy required referenced archive vpks (and optional .cam) when baking to a different directory
	if (!outputDir.empty()) {
		for (const auto idx : referencedArchives) {
			if (idx == PATCH_ARCHIVE_INDEX && preserveExistingPatchArchive) {
				continue;
			}
			const auto src = RespawnVPK::buildArchivePath(std::string{this->fullFilePath}, idx);
			std::error_code ec;
			if (!std::filesystem::is_regular_file(src, ec)) {
				continue;
			}
			const auto dst = RespawnVPK::makeArchivePathForWrite(outDirVpkPath, idx);
			ec.clear();
			std::filesystem::create_directories(std::filesystem::path{dst}.parent_path(), ec);
			ec.clear();
			std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);

			const auto srcCam = src + ".cam";
			ec.clear();
			if (std::filesystem::is_regular_file(srcCam, ec)) {
				const auto dstCam = dst + ".cam";
				ec.clear();
				std::filesystem::copy_file(srcCam, dstCam, std::filesystem::copy_options::overwrite_existing, ec);
			}
		}
	}

	// Write patch archive (only if we actually have patch content)
	if (!patchArchive.empty()) {
		{
			const auto openMode = preserveExistingPatchArchive ? (std::ios::binary | std::ios::app) : (std::ios::binary | std::ios::trunc);
			std::ofstream f{dstPatchArchivePath, openMode};
			if (!f) {
				this->lastError = "failed to open patch archive for write: " + dstPatchArchivePath;
				return false;
			}
			f.write(reinterpret_cast<const char*>(patchArchive.data()), static_cast<std::streamsize>(patchArchive.size()));
			if (!f) {
				this->lastError = "failed to write patch archive: " + dstPatchArchivePath;
				return false;
			}
		}

		// Write patch .cam if needed (append to preserve offsets for preserved patch archives)
		if (!patchCams.empty()) {
			for (auto& c : patchCams) {
				const auto it = std::find_if(treeItems.begin(), treeItems.end(), [&](const TreeItem& t) { return t.path == c.path; });
				if (it != treeItems.end() && !it->meta.parts.empty()) {
					c.vpkContentOffset = it->meta.parts.front().entryOffset;
				}
			}

			WriteBuffer w(patchCams.size() * 32);
			for (const auto& t : treeItems) {
				if (t.ext != "wav" || !t.inPatchArchive) {
					continue;
				}
				const auto it = std::find_if(patchCams.begin(), patchCams.end(), [&](const CamEntry& c) { return c.path == t.path; });
				if (it == patchCams.end()) {
					continue;
				}
				w.writeU32(it->magic);
				w.writeU32(it->originalSize);
				w.writeU32(it->compressedSize);
				w.writeU24(it->sampleRate & 0x00FFFFFFu);
				w.writeU8(it->channels);
				w.writeU32(it->sampleCount);
				w.writeU32(it->headerSize);
				w.writeU64(it->vpkContentOffset);
			}

			const auto camOpenMode = preserveExistingPatchArchive ? (std::ios::binary | std::ios::app) : (std::ios::binary | std::ios::trunc);
			std::ofstream f{dstPatchCamPath, camOpenMode};
			if (f) {
				f.write(reinterpret_cast<const char*>(w.buf.data()), static_cast<std::streamsize>(w.buf.size()));
			}
		}
	}

	// Sort entries for deterministic tree layout
	std::sort(treeItems.begin(), treeItems.end(), [](const TreeItem& a, const TreeItem& b) {
		const auto ka = a.ext + '\0' + a.dir + '\0' + a.fileStem + '\0';
		const auto kb = b.ext + '\0' + b.dir + '\0' + b.fileStem + '\0';
		return ka < kb;
	});

	// Build directory tree buffer
	WriteBuffer treeBuf;
	treeBuf.buf.reserve(treeItems.size() * 64);
	std::string lastExt;
	std::string lastDir;

	for (const auto& e : treeItems) {
		if (!lastExt.empty() && e.ext != lastExt) {
			treeBuf.writeU16(0);
			lastDir.clear();
		} else if (!lastDir.empty() && e.dir != lastDir) {
			treeBuf.writeU8(0);
		}

		if (e.ext != lastExt) {
			treeBuf.writeCString(e.ext);
			lastExt = e.ext;
		}
		if (e.dir != lastDir) {
			treeBuf.writeCString(e.dir);
			lastDir = e.dir;
		}
		treeBuf.writeCString(e.fileStem);

		treeBuf.writeU32(e.meta.crc32);
		treeBuf.writeU16(e.meta.preloadBytes);

		for (const auto& p : e.meta.parts) {
			treeBuf.writeU16(p.archiveIndex);
			treeBuf.writeU16(static_cast<std::uint16_t>(p.loadFlags & 0xFFFFu));
			treeBuf.writeU32(p.textureFlags);
			treeBuf.writeU64(p.entryOffset);
			treeBuf.writeU64(p.entryLength);
			treeBuf.writeU64(p.entryLengthUncompressed);
		}
		treeBuf.writeU16(RESPAWN_CHUNK_END_MARKER);

		if (callback) {
			vpkpp::Entry ent = vpkpp::PackFile::createNewEntry();
			ent.crc32 = e.meta.crc32;
			std::uint64_t len = 0;
			for (const auto& p : e.meta.parts) {
				len += p.entryLengthUncompressed;
			}
			ent.length = len;
			if (!e.meta.parts.empty()) {
				ent.archiveIndex = e.meta.parts.front().archiveIndex;
			}
			callback(e.path, ent);
		}
	}
	treeBuf.writeU24(0);

	// Build header
	WriteBuffer headerBuf(RESPAWN_VPK_HEADER_LEN);
	headerBuf.writeU32(RESPAWN_VPK_SIGNATURE);
	headerBuf.writeU16(RESPAWN_VPK_MAJOR_VERSION);
	headerBuf.writeU16(RESPAWN_VPK_MINOR_VERSION);
	headerBuf.writeU32(static_cast<std::uint32_t>(treeBuf.buf.size()));
	headerBuf.writeU32(0); // signature size (unused)

	// Write dir VPK
	{
		std::ofstream f{outDirVpkPath, std::ios::binary | std::ios::trunc};
		if (!f) {
			this->lastError = "failed to open for write: " + outDirVpkPath;
			return false;
		}
		f.write(reinterpret_cast<const char*>(headerBuf.buf.data()), static_cast<std::streamsize>(headerBuf.buf.size()));
		f.write(reinterpret_cast<const char*>(treeBuf.buf.data()), static_cast<std::streamsize>(treeBuf.buf.size()));
		if (!f) {
			this->lastError = "failed to write: " + outDirVpkPath;
			return false;
		}
	}

	// Rebuild in-memory state to match output
	this->metaEntries.clear();
	this->entries.clear();
	this->unbakedEntries.clear();
	this->unbakedFlags.clear();

	for (auto& ti : treeItems) {
		std::string fullPath = ti.fileStem == " " ? std::string{} : ti.fileStem;
		if (ti.ext != " ") {
			fullPath += '.';
			fullPath += ti.ext;
		}
		if (ti.dir != " " && !ti.dir.empty()) {
			fullPath = ti.dir + '/' + fullPath;
		}
		fullPath = this->cleanEntryPath(fullPath);

		Entry entry = createNewEntry();
		entry.crc32 = ti.meta.crc32;

		std::uint64_t dataLen = 0;
		for (const auto& p : ti.meta.parts) {
			dataLen += p.entryLengthUncompressed;
		}
		entry.length = dataLen;
		if (!ti.meta.parts.empty()) {
			entry.archiveIndex = ti.meta.parts.front().archiveIndex;
		}

		this->metaEntries.emplace(fullPath, ti.meta);
		this->entries.emplace(fullPath, entry);
	}

	PackFile::setFullFilePath(outputDir);

	// Refresh (write) manifest next to the dir vpk, so future folder-based repacks can preserve flags
	{
		std::vector<respawn_vpk::ManifestWriteItem> mani;
		mani.reserve(treeItems.size());
		for (const auto& ti : treeItems) {
			respawn_vpk::ManifestWriteItem m;
			m.path = ti.path;
			m.values.preloadSize = ti.meta.preloadBytes;
			if (!ti.meta.parts.empty()) {
				m.values.loadFlags = ti.meta.parts.front().loadFlags;
				m.values.textureFlags = ti.meta.parts.front().textureFlags;
				m.values.useCompression = (ti.meta.parts.front().entryLength != ti.meta.parts.front().entryLengthUncompressed);
			}
			m.values.deDuplicate = true;
			mani.push_back(std::move(m));
		}
		std::string err;
		(void)respawn_vpk::writeManifestForDirVpkPath(std::filesystem::path{outDirVpkPath}, mani, &err);
	}

	return true;
}

bool RespawnVPK::isRespawnVPKDirPath(std::string_view path) {
	// Historically Respawn dir VPks ended with `_dir.vpk` (Apex/R5).
	// Titanfall 2 uses a split naming scheme where the directory can live in `_000.vpk`.
	//
	// This is only a lightweight heuristic; `open()` ultimately validates the header signature/version.
	auto endsWithCi = [](std::string_view s, std::string_view suffix) -> bool {
		if (s.size() < suffix.size()) return false;
		return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(),
			[](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			});
	};

	if (endsWithCi(path, "_dir.vpk")) {
		return true;
	}

	// Match `..._000.vpk` (case-insensitive).
	if (path.size() < 8) { // "_000.vpk"
		return false;
	}
	const auto tail = path.substr(path.size() - 8);
	if (!endsWithCi(tail, ".vpk")) {
		return false;
	}
	if (tail[0] != '_' ||
		tail[1] != '0' || tail[2] != '0' || tail[3] != '0') {
		return false;
	}
	return true;
}

bool RespawnVPK::readAndValidateHeader(std::ifstream& f, std::uint32_t& treeLength) {
	f.seekg(0, std::ios::beg);
	if (!f) {
		return false;
	}

	const auto sig = RespawnVPK::readU32(f);
	const auto major = RespawnVPK::readU16(f);
	const auto minor = RespawnVPK::readU16(f);
	treeLength = RespawnVPK::readU32(f);
	(void) RespawnVPK::readU32(f);
	if (!f) {
		return false;
	}

	if (sig != RESPAWN_VPK_SIGNATURE) {
		return false;
	}
	// Respawn's packedstore uses major=2 with minor observed as 0..3 depending on game/build.
	// We still validate the signature and require a non-zero directory tree length.
	if (major != RESPAWN_VPK_MAJOR_VERSION || minor > RESPAWN_VPK_MINOR_VERSION) {
		return false;
	}
	if (!treeLength) {
		return false;
	}

	return true;
}

std::string RespawnVPK::readCString(std::ifstream& f) {
	std::string out;
	char c = 0;
	while (f.get(c)) {
		if (c == '\0') {
			break;
		}
		out.push_back(c);
	}
	return out;
}

std::uint16_t RespawnVPK::readU16(std::ifstream& f) {
	std::array<unsigned char, 2> b{};
	f.read(reinterpret_cast<char*>(b.data()), b.size());
	if (!f) {
		return 0;
	}
	return static_cast<std::uint16_t>(b[0] | (static_cast<std::uint16_t>(b[1]) << 8));
}

std::uint32_t RespawnVPK::readU32(std::ifstream& f) {
	std::array<unsigned char, 4> b{};
	f.read(reinterpret_cast<char*>(b.data()), b.size());
	if (!f) {
		return 0;
	}
	return static_cast<std::uint32_t>(b[0])
		| (static_cast<std::uint32_t>(b[1]) << 8)
		| (static_cast<std::uint32_t>(b[2]) << 16)
		| (static_cast<std::uint32_t>(b[3]) << 24);
}

std::uint64_t RespawnVPK::readU64(std::ifstream& f) {
	std::array<unsigned char, 8> b{};
	f.read(reinterpret_cast<char*>(b.data()), b.size());
	if (!f) {
		return 0;
	}
	return static_cast<std::uint64_t>(b[0])
		| (static_cast<std::uint64_t>(b[1]) << 8)
		| (static_cast<std::uint64_t>(b[2]) << 16)
		| (static_cast<std::uint64_t>(b[3]) << 24)
		| (static_cast<std::uint64_t>(b[4]) << 32)
		| (static_cast<std::uint64_t>(b[5]) << 40)
		| (static_cast<std::uint64_t>(b[6]) << 48)
		| (static_cast<std::uint64_t>(b[7]) << 56);
}

bool RespawnVPK::readBytes(std::ifstream& f, std::vector<std::byte>& out, std::size_t n) {
	if (!n) {
		return true;
	}
	f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
	return static_cast<bool>(f);
}

std::string RespawnVPK::stripPakLang(const std::string& path) {
	static constexpr std::array<std::string_view, 12> langs{
		"english", "french", "german", "italian", "japanese", "korean",
		"polish", "portugese", "russian", "spanish", "tchinese", "schinese"
	};

	std::string out = path;
	for (const auto& lang : langs) {
		for (;;) {
			auto pos = out.find(lang);
			if (pos == std::string::npos) {
				break;
			}
			out.erase(pos, lang.size());
		}
	}
	return out;
}

std::string RespawnVPK::stripPakLangFilenamePrefix(const std::string& path) {
	static constexpr std::array<std::string_view, 12> langs{
		"english", "french", "german", "italian", "japanese", "korean",
		"polish", "portugese", "russian", "spanish", "tchinese", "schinese"
	};

	const auto p = std::filesystem::path{path};
	const auto name = p.filename().string();
	std::string nameLower = name;
	std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	for (const auto& lang : langs) {
		const auto langStr = std::string{lang};
		if (nameLower.rfind(langStr, 0) == 0) {
			const auto stripped = name.substr(langStr.size());
			return (p.parent_path() / stripped).string();
		}
	}
	return path;
}

std::string RespawnVPK::buildArchivePath(const std::string& dirVpkPath, std::uint16_t archiveIndex) {
	auto tryBuild = [archiveIndex](const std::string& base) -> std::string {
		const std::filesystem::path p{base};
		auto s = p.string();
		auto endsWithCi = [](const std::string& str, std::string_view suffix) -> bool {
			if (str.size() < suffix.size()) return false;
			for (std::size_t i = 0; i < suffix.size(); i++) {
				const auto a = static_cast<unsigned char>(str[str.size() - suffix.size() + i]);
				const auto b = static_cast<unsigned char>(suffix[i]);
				if (std::tolower(a) != std::tolower(b)) return false;
			}
			return true;
		};

		// Apex/R5: `..._dir.vpk` -> `..._%03u.vpk`
		if (endsWithCi(s, "_dir.vpk")) {
			char buf[16];
			std::snprintf(buf, sizeof(buf), "_%03u.vpk", static_cast<unsigned>(archiveIndex));
			s.replace(s.size() - std::string_view{"_dir.vpk"}.size(), std::string_view{"_dir.vpk"}.size(), buf);
			return s;
		}

		// Titanfall 2: directory can live in `_000.vpk`, so `..._000.vpk` -> `..._%03u.vpk`
		if (s.size() >= 8 && endsWithCi(s, ".vpk") &&
			s[s.size() - 8] == '_' &&
			std::isdigit(static_cast<unsigned char>(s[s.size() - 7])) &&
			std::isdigit(static_cast<unsigned char>(s[s.size() - 6])) &&
			std::isdigit(static_cast<unsigned char>(s[s.size() - 5]))) {
			char buf[8];
			std::snprintf(buf, sizeof(buf), "%03u", static_cast<unsigned>(archiveIndex));
			s.replace(s.size() - 7, 3, buf);
		}
		return s;
	};

	auto candidate = tryBuild(dirVpkPath);
	std::error_code ec;
	if (std::filesystem::is_regular_file(candidate, ec)) {
		return candidate;
	}

	// Fallback: strip language markers from the filename prefix and try again
	// Examples:
	// `englishclient_...pak000_dir.vpk` -> `client_...pak000_000.vpk`
	// `englishserver_...pak000_dir.vpk` -> `server_...pak000_000.vpk`
	candidate = tryBuild(RespawnVPK::stripPakLangFilenamePrefix(dirVpkPath));
	ec.clear();
	if (std::filesystem::is_regular_file(candidate, ec)) {
		return candidate;
	}

	// Last resort: legacy behavior, strip language tokens from the entire path and try again.
	// This is less conservative, but helps with non standard layouts
	candidate = tryBuild(RespawnVPK::stripPakLang(dirVpkPath));
	return candidate;
}

std::optional<std::vector<std::byte>> RespawnVPK::readFileRange(const std::string& path, std::uint64_t offset, std::size_t length) {
	// Avoid huge allocations / crashes on malformed metadata
	constexpr std::size_t MAX_READ = 512ull * 1024ull * 1024ull;
	if (length > MAX_READ) {
		return std::nullopt;
	}

	std::ifstream f{path, std::ios::binary};
	if (!f) {
		return std::nullopt;
	}

	f.seekg(0, std::ios::end);
	const auto endPos = f.tellg();
	if (endPos < 0) {
		return std::nullopt;
	}
	const auto fileSize = static_cast<std::uint64_t>(endPos);
	if (offset > fileSize) {
		return std::nullopt;
	}
	if (static_cast<std::uint64_t>(length) > (fileSize - offset)) {
		return std::nullopt;
	}

	f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
	if (!f) {
		return std::nullopt;
	}

	std::vector<std::byte> out;
	try {
		out.resize(length);
	} catch (...) {
		return std::nullopt;
	}
	f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
	if (!f) {
		return std::nullopt;
	}

	return out;
}

std::optional<std::vector<std::byte>> RespawnVPK::lzhamDecompress(const std::byte* src, std::size_t srcLen, std::size_t dstLen) {
#ifdef VPKEDIT_HAVE_LZHAM
	std::vector<std::byte> out(dstLen);
	size_t outLen = dstLen;
	const auto rc = lzham_bridge_decompress(
		reinterpret_cast<const std::uint8_t*>(src), srcLen,
		reinterpret_cast<std::uint8_t*>(out.data()), &outLen);

	if (rc != 0 || outLen == 0 || outLen > dstLen) {
		return std::nullopt;
	}
	out.resize(outLen);
	return out;
#else
	(void)src;
	(void)srcLen;
	(void)dstLen;
	return std::nullopt;
#endif
}
