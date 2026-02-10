#include "Folder.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include <sourcepp/FS.h>

using namespace sourcepp;
using namespace vpkpp;

static bool shouldHideVpkInFolderView(const std::filesystem::path& p)
{
    if (p.extension() != ".vpk")
        return false;

    std::string name = p.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    // Source-engine "multi-part" VPKs: `pak01_dir.vpk` + `pak01_000.vpk`, `pak01_001.vpk`
    // Hide the numbered part files when the corresponding `_dir.vpk` exists beside them
    {
        const std::string ext = ".vpk";
        if (name.size() > ext.size() && name.ends_with(ext) && !name.ends_with("_dir.vpk")) {
            const std::string stem = name.substr(0, name.size() - ext.size());
            const auto us = stem.find_last_of('_');
            if (us != std::string::npos) {
                const std::string suffix = stem.substr(us + 1);
                const bool allDigits = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
                if (allDigits && suffix.size() == 3) {
                    const std::string baseStem = stem.substr(0, us);
                    std::error_code ec;
                    const auto siblingDir = p.parent_path() / (baseStem + "_dir.vpk");
                    if (std::filesystem::exists(siblingDir, ec) && !ec) {
                        return true;
                    }
                }
            }
        }
    }

    if ((name.rfind("englishclient", 0) == 0 ||
         name.rfind("englishserver", 0) == 0) &&
        name.ends_with("_dir.vpk"))
    {
        return false;
    }

    if (name.rfind("client", 0) == 0 ||
        name.rfind("server", 0) == 0)
    {
        return true;
    }

    return false;
}

std::unique_ptr<PackFile> Folder::open(const std::string& path, const EntryCallback& callback) {
	auto* folder = new Folder{path};
	std::unique_ptr<PackFile> packFile{folder};

	std::error_code ec;
	for (const auto& dirEntry : std::filesystem::recursive_directory_iterator{ path, std::filesystem::directory_options::skip_permission_denied, ec })
	{
		if (!dirEntry.is_regular_file(ec)) {
			continue;
		}
		ec.clear();

		if (shouldHideVpkInFolderView(dirEntry.path())) {
			continue;
		}

		auto entryPath = std::filesystem::relative(dirEntry.path(), path, ec).string();
		if (ec || entryPath.empty()) {
			continue;
		}

		Entry entry = createNewEntry();
		entry.length = std::filesystem::file_size(dirEntry.path(), ec);
		folder->entries.insert(folder->cleanEntryPath(entryPath), entry);
	}

	return packFile;
}

std::optional<std::vector<std::byte>> Folder::readEntry(const std::string& path_) const {
	auto path = this->fullFilePath + '/' + this->cleanEntryPath(path_);
	std::error_code ec;
	if (!std::filesystem::is_regular_file(path, ec)) {
		return std::nullopt;
	}
	return fs::readFileBuffer(path);
}

Attribute Folder::getSupportedEntryAttributes() const {
	using enum Attribute;
	return LENGTH;
}
