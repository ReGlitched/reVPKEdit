#include "VTFConversion.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <QFile>
#include <QImage>
#include <QString>

#include <vtfpp/vtfpp.h>

using namespace vtfpp;

static bool writeTgaUncompressed(const QImage& imgIn, const QString& outPath, QString* error) {
	// Force a format we can write predictably
	QImage img = imgIn;
	if (img.format() != QImage::Format_RGBA8888 && img.format() != QImage::Format_RGB888) {
		img = img.convertToFormat(QImage::Format_RGBA8888);
	}

	const int w = img.width();
	const int h = img.height();
	if (w <= 0 || h <= 0) {
		if (error) *error = "invalid image size";
		return false;
	}

	const bool hasAlpha = (img.format() == QImage::Format_RGBA8888);
	const uint8_t bpp = hasAlpha ? 32 : 24;

	QFile f(outPath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		if (error) *error = "failed to open output path for write";
		return false;
	}

	// TGA header (18 bytes)
	// Uncompressed true-color image (type 2). We'll write with top-left origin (bit 5 set)
	uint8_t header[18]{};
	header[2] = 2; // image type
	header[12] = static_cast<uint8_t>(w & 0xFF);
	header[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
	header[14] = static_cast<uint8_t>(h & 0xFF);
	header[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
	header[16] = bpp;
	header[17] = static_cast<uint8_t>((hasAlpha ? 8 : 0) | 0x20); // alpha bits + top-left origin

	if (f.write(reinterpret_cast<const char*>(header), sizeof(header)) != sizeof(header)) {
		if (error) *error = "failed to write TGA header";
		return false;
	}

	// Write pixels as BGR(A), top-to-bottom
	for (int y = 0; y < h; ++y) {
		const uchar* scan = img.constScanLine(y);
		if (hasAlpha) {
			// RGBA8888 -> BGRA
			for (int x = 0; x < w; ++x) {
				const uchar r = scan[x * 4 + 0];
				const uchar g = scan[x * 4 + 1];
				const uchar b = scan[x * 4 + 2];
				const uchar a = scan[x * 4 + 3];
				const char px[4]{static_cast<char>(b), static_cast<char>(g), static_cast<char>(r), static_cast<char>(a)};
				if (f.write(px, 4) != 4) {
					if (error) *error = "failed to write TGA pixel data";
					return false;
				}
			}
		} else {
			// RGB888 -> BGR
			for (int x = 0; x < w; ++x) {
				const uchar r = scan[x * 3 + 0];
				const uchar g = scan[x * 3 + 1];
				const uchar b = scan[x * 3 + 2];
				const char px[3]{static_cast<char>(b), static_cast<char>(g), static_cast<char>(r)};
				if (f.write(px, 3) != 3) {
					if (error) *error = "failed to write TGA pixel data";
					return false;
				}
			}
		}
	}

	return true;
}

QString vtfGetConvertedOutputPath(const QString& extractedVtfPath, VTFConvertFormat fmt) {
	const QString lower = extractedVtfPath.toLower();
	if (!lower.endsWith(".vtf")) {
		return {};
	}
	const QString base = extractedVtfPath.left(extractedVtfPath.size() - 4);
	switch (fmt) {
		case VTFConvertFormat::PNG: return base + ".png";
		case VTFConvertFormat::TGA: return base + ".tga";
		case VTFConvertFormat::DDS_BC7: return base + ".dds";
	}
	return {};
}

// Minimal DDS writer for BC7, using a DX10 header (DXGI_FORMAT_BC7_*)
// Limitations (by design, for now):
// - 2D textures only (no cubemaps, no arrays, no 3D)
// - first frame/face/slice only
static bool writeDdsBc7FromVtf(const VTF& vtf, const QString& outPath, QString* error) {
	const auto frameCount = vtf.getFrameCount();
	const auto faceCount = vtf.getFaceCount();
	const auto depth = vtf.getDepth(0);
	if (frameCount != 1 || faceCount != 1 || depth != 1) {
		if (error) *error = "DDS(BC7) export currently supports only 2D single-frame textures (no cubemaps/arrays/3D)";
		return false;
	}

	const uint32_t w0 = vtf.getWidth(0);
	const uint32_t h0 = vtf.getHeight(0);
	if (w0 == 0 || h0 == 0) {
		if (error) *error = "invalid VTF dimensions";
		return false;
	}

	// Some games ship VTFs where the metadata mip count is non-zero but the data for deeper mips is missing/invalid
	// For robustness (and to match the request), export only the top-level mip
	const uint32_t mipCount = 1;

	// BC7 is 16 bytes per 4x4 block
	const uint32_t blocksWide0 = std::max<uint32_t>(1, (w0 + 3) / 4);
	const uint32_t blocksHigh0 = std::max<uint32_t>(1, (h0 + 3) / 4);
	const uint32_t topLevelLinearSize = blocksWide0 * blocksHigh0 * 16;

	QFile f(outPath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		if (error) *error = "failed to open output path for write";
		return false;
	}

	auto writeU32 = [&](uint32_t v) -> bool {
		return f.write(reinterpret_cast<const char*>(&v), 4) == 4;
	};
	auto writeBytes = [&](const void* p, qsizetype n) -> bool {
		return f.write(reinterpret_cast<const char*>(p), n) == n;
	};

	// DDS magic
	const char magic[4] = {'D', 'D', 'S', ' '};
	if (!writeBytes(magic, 4)) {
		if (error) *error = "failed to write DDS magic";
		return false;
	}

	// DDS_HEADER (124 bytes)
	// See: Microsoft DDS format (DX10 extension)
	const uint32_t DDSD_CAPS        = 0x1;
	const uint32_t DDSD_HEIGHT      = 0x2;
	const uint32_t DDSD_WIDTH       = 0x4;
	const uint32_t DDSD_PITCH       = 0x8;
	const uint32_t DDSD_PIXELFORMAT = 0x1000;
	const uint32_t DDSD_MIPMAPCOUNT = 0x20000;
	const uint32_t DDSD_LINEARSIZE  = 0x80000;

	const uint32_t DDPF_FOURCC = 0x4;

	const uint32_t DDSCAPS_COMPLEX = 0x8;
	const uint32_t DDSCAPS_TEXTURE = 0x1000;
	const uint32_t DDSCAPS_MIPMAP  = 0x400000;

	uint32_t headerFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE;
	uint32_t caps = DDSCAPS_TEXTURE;
	if (mipCount > 1) {
		headerFlags |= DDSD_MIPMAPCOUNT;
		caps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;
	}

	// dwSize
	if (!writeU32(124)) return false;
	// dwFlags
	if (!writeU32(headerFlags)) return false;
	// dwHeight, dwWidth
	if (!writeU32(h0)) return false;
	if (!writeU32(w0)) return false;
	// dwPitchOrLinearSize
	if (!writeU32(topLevelLinearSize)) return false;
	// dwDepth
	if (!writeU32(0)) return false;
	// dwMipMapCount
	if (!writeU32(mipCount > 1 ? mipCount : 0)) return false;
	// dwReserved1[11]
	for (int i = 0; i < 11; ++i) {
		if (!writeU32(0)) return false;
	}

	// DDS_PIXELFORMAT (32 bytes)
	// DX10 extension: FOURCC = 'DX10'
	if (!writeU32(32)) return false;          // ddspf.dwSize
	if (!writeU32(DDPF_FOURCC)) return false; // ddspf.dwFlags
	const uint32_t fourCC = static_cast<uint32_t>('D') | (static_cast<uint32_t>('X') << 8) | (static_cast<uint32_t>('1') << 16) | (static_cast<uint32_t>('0') << 24);
	if (!writeU32(fourCC)) return false;      // ddspf.dwFourCC
	for (int i = 0; i < 5; ++i) {             // ddspf.dwRGBBitCount + masks (unused)
		if (!writeU32(0)) return false;
	}

	// dwCaps, dwCaps2, dwCaps3, dwCaps4, dwReserved2
	if (!writeU32(caps)) return false;
	if (!writeU32(0)) return false;
	if (!writeU32(0)) return false;
	if (!writeU32(0)) return false;
	if (!writeU32(0)) return false;

	// DDS_HEADER_DXT10 (20 bytes)
	// DXGI_FORMAT_BC7_UNORM = 98, DXGI_FORMAT_BC7_UNORM_SRGB = 99
	const uint32_t dxgiBc7 = vtf.isSRGB() ? 99u : 98u;
	const uint32_t D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3u;
	if (!writeU32(dxgiBc7)) return false;
	if (!writeU32(D3D10_RESOURCE_DIMENSION_TEXTURE2D)) return false;
	if (!writeU32(0)) return false; // miscFlag (no cubemap)
	if (!writeU32(1)) return false; // arraySize
	if (!writeU32(0)) return false; // miscFlags2 (alpha mode unknown)

	// Data: mip 0, frame=0, face=0, slice=0
	const auto data = vtf.getImageDataAs(ImageFormat::BC7, 0, 0, 0, 0);
	if (data.empty()) {
		if (error) *error = "failed to convert mip 0 to BC7";
		return false;
	}
	if (f.write(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size())) != static_cast<qsizetype>(data.size())) {
		if (error) *error = "failed to write DDS pixel data";
		return false;
	}

	return true;
}

bool vtfConvertToFile(const std::vector<std::byte>& vtfBytes, VTFConvertFormat fmt, const QString& outPath, QString* error) {
	try {
		if (vtfBytes.empty()) {
			if (error) *error = "empty VTF";
			return false;
		}

		const VTF vtf{{vtfBytes.data(), static_cast<std::span<const std::byte>::size_type>(vtfBytes.size())}};

		const auto w = static_cast<int>(vtf.getWidth());
		const auto h = static_cast<int>(vtf.getHeight());
		if (w <= 0 || h <= 0) {
			if (error) *error = "invalid VTF dimensions";
			return false;
		}

		// Always convert to RGBA in-memory so alpha is preserved when present
		const auto rgba = vtf.getImageDataAsRGBA8888();
		if (rgba.empty()) {
			if (error) *error = "failed to decode VTF pixels";
			return false;
		}

		QImage img(reinterpret_cast<const uchar*>(rgba.data()), w, h, QImage::Format_RGBA8888);
		img = img.copy(); // own the data

		if (fmt == VTFConvertFormat::PNG) {
			if (!img.save(outPath, "PNG")) {
				if (error) *error = "failed to save PNG (missing Qt image plugin?)";
				return false;
			}
			return true;
		}
		if (fmt == VTFConvertFormat::TGA) {
			return writeTgaUncompressed(img, outPath, error);
		}
		if (fmt == VTFConvertFormat::DDS_BC7) {
			return writeDdsBc7FromVtf(vtf, outPath, error);
		}

		if (error) *error = "unsupported output format";
		return false;
	} catch (const std::exception& e) {
		if (error) *error = QString("exception: ") + QString::fromLocal8Bit(e.what());
		return false;
	} catch (...) {
		if (error) *error = "unknown exception";
		return false;
	}
}
