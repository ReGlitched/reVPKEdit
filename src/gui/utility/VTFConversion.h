#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

class QString;
class QImage;

enum class VTFConvertFormat {
	PNG,
	TGA,
	DDS_BC7,
};

// Returns output filepath by replacing the extension on `extractedVtfPath`
// If the path doesn't end with ".vtf" (case-insensitive), returns empty string
QString vtfGetConvertedOutputPath(const QString& extractedVtfPath, VTFConvertFormat fmt);

// Convert VTF bytes to an image file on disk. Supports writing:
// - PNG via Qt's image plugins
// - TGA via a minimal uncompressed writer (no Qt plugin required)
bool vtfConvertToFile(const std::vector<std::byte>& vtfBytes, VTFConvertFormat fmt, const QString& outPath, QString* error = nullptr);
