#include "RespawnVPKManifest.h"

#include <algorithm>
#include <cctype>

#include <kvpp/KV1.h>
#include <kvpp/KV1Writer.h>
#include <sourcepp/FS.h>
#include <sourcepp/String.h>

namespace respawn_vpk {

namespace {

static constexpr std::string_view LANGS[] = {
	"english", "french", "german", "italian", "japanese", "korean",
	"polish", "portugese", "russian", "spanish", "tchinese", "schinese"
};

[[nodiscard]] std::string toLower(std::string s) {
	for (auto& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

[[nodiscard]] std::string stripLocaleTokensFromFilename(std::string name) {
	auto lower = toLower(name);
	for (const auto lang : LANGS) {
		for (;;) {
			const auto pos = lower.find(lang);
			if (pos == std::string::npos) {
				break;
			}
			name.erase(pos, std::string{lang}.size());
			lower.erase(pos, std::string{lang}.size());
		}
	}
	return name;
}

[[nodiscard]] std::vector<std::filesystem::path> manifestCandidatePaths(const std::filesystem::path& dirVpkPath) {
	const auto parent = dirVpkPath.parent_path();
	const auto stem = dirVpkPath.stem().string();
	const auto strippedStem = stripLocaleTokensFromFilename(stem);

	std::vector<std::filesystem::path> out;
	out.reserve(3);

	out.push_back(parent / "manifest" / (stem + ".txt"));
	if (!sourcepp::string::iequals(stem, strippedStem)) {
		out.push_back(parent / "manifest" / (strippedStem + ".txt"));
	}
	return out;
}

[[nodiscard]] std::optional<ManifestEntry> parseEntryKV(const kvpp::KV1ElementReadable<>& kv) {
	ManifestEntry e{};

	bool seenAny = false;
	for (const auto& child : kv.getChildren()) {
		if (sourcepp::string::iequals(child.getKey(), "preloadSize")) {
			e.preloadSize = static_cast<std::uint16_t>(child.getValue<int32_t>());
			seenAny = true;
		} else if (sourcepp::string::iequals(child.getKey(), "loadFlags")) {
			e.loadFlags = static_cast<std::uint32_t>(child.getValue<int64_t>());
			seenAny = true;
		} else if (sourcepp::string::iequals(child.getKey(), "textureFlags")) {
			e.textureFlags = static_cast<std::uint16_t>(child.getValue<int32_t>());
			seenAny = true;
		} else if (sourcepp::string::iequals(child.getKey(), "useCompression")) {
			e.useCompression = child.getValue<bool>();
			seenAny = true;
		} else if (sourcepp::string::iequals(child.getKey(), "deDuplicate")) {
			e.deDuplicate = child.getValue<bool>();
			seenAny = true;
		}
	}

	if (!seenAny) {
		return std::nullopt;
	}
	return e;
}

} // namespace

std::string normalizeManifestPath(std::string_view path) {
	std::string s{path};
	for (auto& c : s) {
		if (c == '\\') {
			c = '/';
		}
	}
	sourcepp::string::normalizeSlashes(s, true, true);
	if (s.rfind("./", 0) == 0) {
		s.erase(0, 2);
	}
	return toLower(s);
}

std::optional<ManifestMap> readManifestForDirVpkPath(const std::filesystem::path& dirVpkPath) {
	for (const auto& cand : manifestCandidatePaths(dirVpkPath)) {
		std::error_code ec;
		if (!std::filesystem::is_regular_file(cand, ec)) {
			continue;
		}

		const auto text = sourcepp::fs::readFileText(cand);
		if (text.empty()) {
			continue;
		}

		kvpp::KV1 kv{text, false};

		// Find "BuildManifest" root
		const kvpp::KV1ElementReadable<>* root = nullptr;
		for (const auto& e : kv.getChildren()) {
			if (sourcepp::string::iequals(e.getKey(), "BuildManifest")) {
				root = &e;
				break;
			}
		}
		if (!root) {
			continue;
		}

		ManifestMap out;
		out.reserve(root->getChildCount());

		for (const auto& entry : root->getChildren()) {
			const auto key = normalizeManifestPath(entry.getKey());
			if (key.empty()) {
				continue;
			}
			if (auto parsed = parseEntryKV(entry)) {
				out.emplace(key, *parsed);
			}
		}

		return out;
	}

	return std::nullopt;
}

bool writeManifestForDirVpkPath(const std::filesystem::path& dirVpkPath, const std::vector<ManifestWriteItem>& items, std::string* outError) {
	const auto cands = manifestCandidatePaths(dirVpkPath);
	if (cands.empty()) {
		if (outError) *outError = "manifest candidate list was empty";
		return false;
	}

	kvpp::KV1Writer<> w{"", false};
	auto& root = w.addChild("BuildManifest");

	// Deterministic order: sort by path
	std::vector<ManifestWriteItem> sorted = items;
	std::sort(sorted.begin(), sorted.end(), [](const ManifestWriteItem& a, const ManifestWriteItem& b) {
		return a.path < b.path;
	});

	for (const auto& it : sorted) {
		std::string key = it.path;
		for (auto& c : key) {
			if (c == '/') c = '\\';
		}
		auto& ent = root.addChild(key);
		ent("preloadSize") = static_cast<int32_t>(it.values.preloadSize);
		ent("loadFlags") = static_cast<int64_t>(it.values.loadFlags);
		ent("textureFlags") = static_cast<int32_t>(it.values.textureFlags);
		ent("useCompression") = it.values.useCompression;
		ent("deDuplicate") = it.values.deDuplicate;
	}

	std::string baked = w.bake();
	if (baked.empty()) {
		if (outError) *outError = "failed to bake manifest KV";
		return false;
	}

	// Ensure manifest directory exists and write primary + alias (if present)
	bool okAny = false;
	for (std::size_t i = 0; i < cands.size(); i++) {
		const auto& p = cands[i];
		std::error_code ec;
		std::filesystem::create_directories(p.parent_path(), ec);
		sourcepp::fs::writeFileText(p, baked);
		okAny = true;
	}

	if (!okAny) {
		if (outError) *outError = "failed to write manifest file(s)";
		return false;
	}
	return true;
}

} // namespace respawn_vpk
