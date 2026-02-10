#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <vpkpp/vpkpp.h>

// Respawn VPK support
// These are still .vpk files, but use header version 196610 (0x30002) and
// per-file chunk records with 64-bit offsets/lengths, commonly LZHAM compressed
class RespawnVPK final : public vpkpp::PackFile {
public:
	/// Open a Respawn "_dir.vpk" file. Returns nullptr if the file is not a Respawn VPK
	[[nodiscard]] static std::unique_ptr<PackFile> open(const std::string& path, const EntryCallback& callback = nullptr);

	static constexpr inline std::string_view GUID = "A4E78A4C4C3D41CDA8E58B7A7D8C0FE2";

	// diagnostic string for why the last read failed
	// this is primarily used to make GUI errors actionable
	[[nodiscard]] std::string_view getLastError() const noexcept { return this->lastError; }

	[[nodiscard]] constexpr std::string_view getGUID() const override {
		return RespawnVPK::GUID;
	}

	[[nodiscard]] constexpr bool isCaseSensitive() const noexcept override {
#ifdef _WIN32
		return false;
#else
		return true;
#endif
	}

	[[nodiscard]] std::optional<std::vector<std::byte>> readEntry(const std::string& path_) const override;

	// Stream extraction to disk. Needed for large entries where readEntry() would require huge allocations
	[[nodiscard]] bool extractEntryToFile(const std::string& entryPath, const std::string& filepath, std::string* outError = nullptr) const;

	[[nodiscard]] vpkpp::Attribute getSupportedEntryAttributes() const override;

	// Write support: edits are stored as unbaked entries; baking writes an updated *_dir.vpk and (optionally)
	// a patch archive `*_999.vpk` containing changed/new files, while reusing existing archives for unchanged files
	bool bake(const std::string& outputDir_ /*= ""*/, vpkpp::BakeOptions options /*= {}*/, const EntryCallback& callback /*= nullptr*/) override;

	bool renameEntry(const std::string& oldPath_, const std::string& newPath_) override;
	bool renameDirectory(const std::string& oldDir_, const std::string& newDir_) override;
	bool removeEntry(const std::string& path_) override;
	std::size_t removeDirectory(const std::string& dirName_) override;

protected:
	using PackFile::PackFile;

private:
	struct FilePart {
		std::uint16_t archiveIndex = 0;
		// Stored as u16 on disk (at least TF2); we keep it widened for convenience.
		std::uint32_t loadFlags = 0;
		// Stored as u32 on disk.
		std::uint32_t textureFlags = 0;
		std::uint64_t entryOffset = 0;
		std::uint64_t entryLength = 0;
		std::uint64_t entryLengthUncompressed = 0;

		[[nodiscard]] bool isCompressed() const noexcept {
			return entryLength != entryLengthUncompressed;
		}
	};

	struct MetaEntry {
		std::uint32_t crc32 = 0;
		std::uint16_t preloadBytes = 0;
		std::uint16_t archiveIndex = 0;
		std::uint64_t preloadOffset = 0;
		std::vector<FilePart> parts;
	};

	// Extra per-entry metadata needed to read Respawn VPK parts
	std::unordered_map<std::string, MetaEntry> metaEntries;

	// For unbaked entries, store desired flags inferred from an existing entry or defaults
	// Key is the cleaned entry path (same case rules as PackFile)
	std::unordered_map<std::string, std::pair<std::uint32_t, std::uint32_t>> unbakedFlags;

	mutable std::string lastError;

	[[nodiscard]] static bool isRespawnVPKDirPath(std::string_view path);
	[[nodiscard]] static bool readAndValidateHeader(std::ifstream& f, std::uint32_t& treeLength);

	[[nodiscard]] static std::string readCString(std::ifstream& f);
	[[nodiscard]] static std::uint16_t readU16(std::ifstream& f);
	[[nodiscard]] static std::uint32_t readU32(std::ifstream& f);
	[[nodiscard]] static std::uint64_t readU64(std::ifstream& f);
	[[nodiscard]] static bool readBytes(std::ifstream& f, std::vector<std::byte>& out, std::size_t n);

	[[nodiscard]] static std::string buildArchivePath(const std::string& dirVpkPath, std::uint16_t archiveIndex);
	[[nodiscard]] static std::string stripPakLang(const std::string& path);
	[[nodiscard]] static std::string stripPakLangFilenamePrefix(const std::string& path);
	[[nodiscard]] static std::string makeArchivePathForWrite(const std::string& dirVpkPath, std::uint16_t archiveIndex);

	[[nodiscard]] static std::optional<std::vector<std::byte>> readFileRange(const std::string& path, std::uint64_t offset, std::size_t length);

	[[nodiscard]] static std::optional<std::vector<std::byte>> lzhamDecompress(const std::byte* src, std::size_t srcLen, std::size_t dstLen);
	[[nodiscard]] static std::vector<std::byte> lzhamCompress(const std::byte* src, std::size_t srcLen);

	void addEntryInternal(vpkpp::Entry& entry, const std::string& path, std::vector<std::byte>& buffer, vpkpp::EntryOptions options) override;

	VPKPP_REGISTER_PACKFILE_OPEN(".vpk", &RespawnVPK::open);
};
