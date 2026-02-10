#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace respawn_vpk {

struct PackOptions {
	// Archive suffix index. Respawn mod/patch vpks commonly use 999
	std::uint16_t archiveIndex = 999;

	// Split each input file into parts of at most this many bytes (uncompressed)
	std::size_t maxPartSize = 1024 * 1024;

	// Compress file parts >= threshold (bytes), excluding some file types
	std::size_t compressionThreshold = 4096;

	// Number of worker threads used while building entries from disk
	std::size_t threadCount = 0;
};

// Packs a directory into a Respawn VPK:
// - Writes `outputDirVpkPath` (must end with `_dir.vpk`)
// - Writes archive vpk next to it with `_XYZ.vpk` where XYZ = options.archiveIndex
// - Writes optional `.cam` file next to the archive vpk (if any .wav were added)
[[nodiscard]] bool packDirectoryToRespawnVPK(
	const std::string& inputDir,
	const std::string& outputDirVpkPath,
	const PackOptions& options = {},
	std::string* outError = nullptr);

// helper for repacking:
// Respawn archives are commonly named like `...pak000_000.vpk` while the dir vpk is `...pak000_dir.vpk`
// If we can infer the 3-digit index from the dir vpk filename, return it; otherwise return fallback
[[nodiscard]] std::uint16_t inferArchiveIndexFromDirVpkPath(std::string_view outputDirVpkPath, std::uint16_t fallback = 999);

} // namespace respawn_vpk
