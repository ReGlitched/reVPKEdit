#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace respawn_vpk {

struct ManifestEntry {
	std::uint16_t preloadSize = 0;
	std::uint32_t loadFlags = (1u << 0) | (1u << 8);
	std::uint16_t textureFlags = (1u << 3);
	bool useCompression = true;
	bool deDuplicate = true;
};

using ManifestMap = std::unordered_map<std::string, ManifestEntry>;

struct ManifestWriteItem {
	std::string path; // normalized to forward slashes (relative path inside vpk)
	ManifestEntry values;
};

// Try to locate and parse the build manifest associated with a Respawn *_dir.vpk
// Looks for `<dirParent>/manifest/<name>.txt` with multiple `<name>` candidates
[[nodiscard]] std::optional<ManifestMap> readManifestForDirVpkPath(const std::filesystem::path& dirVpkPath);

// Write a manifest file next to a Respawn *_dir.vpk (in `<dirParent>/manifest/`)
// Writes to the best name, and also writes an alias name if it differs (to improve interoperability)
[[nodiscard]] bool writeManifestForDirVpkPath(
	const std::filesystem::path& dirVpkPath,
	const std::vector<ManifestWriteItem>& items,
	std::string* outError = nullptr);

[[nodiscard]] std::string normalizeManifestPath(std::string_view path);

} // namespace respawn_vpk
