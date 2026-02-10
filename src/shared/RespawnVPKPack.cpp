#include "RespawnVPKPack.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <sourcepp/FS.h>
#include <sourcepp/String.h>
#include <sourcepp/crypto/CRC32.h>

#include "RespawnVPKManifest.h"

#ifdef VPKEDIT_HAVE_LZHAM
#include <lzham_bridge.h>
#endif

namespace respawn_vpk {

using namespace sourcepp;

namespace {

constexpr std::uint32_t RESPAWN_VPK_SIGNATURE = 0x55AA1234u;
constexpr std::uint16_t RESPAWN_VPK_MAJOR_VERSION = 2;
constexpr std::uint16_t RESPAWN_VPK_MINOR_VERSION = 3;
constexpr std::size_t RESPAWN_VPK_HEADER_LEN = 16;

constexpr std::uint32_t CAM_MAGIC = 3302889984u; // 0xC4DE1A00
constexpr std::size_t CAM_ENTRY_BYTES = 32;

constexpr std::uint16_t RESPAWN_CHUNK_END_MARKER = 0xFFFFu;
constexpr std::uint16_t RESPAWN_CHUNK_CONT_MARKER = 0x0000u;

enum EPackedLoadFlags : std::uint32_t {
	LOAD_VISIBLE     = 1u << 0,
	LOAD_CACHE       = 1u << 8,
	LOAD_ACACHE_UNK0 = 1u << 10,
};

constexpr std::uint16_t TEXTURE_DEFAULT = 1u << 3;

struct FilePart {
	std::uint32_t loadFlags = 0;
	std::uint16_t textureFlags = 0;
	std::uint64_t entryOffset = 0;
	std::uint64_t entryLength = 0;
	std::uint64_t entryLengthUncompressed = 0;
	std::uint32_t dataCrc32 = 0;
	std::vector<std::byte> data;
};

struct CamEntry {
	std::uint32_t magic = CAM_MAGIC;
	std::uint32_t originalSize = 0;
	std::uint32_t compressedSize = 0;
	std::uint32_t sampleRate = 0;
	std::uint8_t channels = 0;
	std::uint32_t sampleCount = 0;
	std::uint32_t headerSize = 44;
	std::uint64_t vpkContentOffset = 0;
	std::string path;
};

struct DirEntry {
	std::string path;
	std::string extension;
	std::string directory;
	std::string fileName;

	std::uint32_t crc32 = 0;
	std::uint16_t preloadBytes = 0; // always 0 for Respawn packedstore
	std::uint16_t packFileIndex = 0;

	std::vector<FilePart> parts;
};

struct WriteBuffer {
	std::vector<std::byte> buf;
	std::size_t pos = 0;

	explicit WriteBuffer(std::size_t reserve = 0) {
		buf.reserve(reserve);
	}

	void writeBytes(std::span<const std::byte> b) {
		if (b.empty()) {
			return;
		}
		buf.insert(buf.end(), b.begin(), b.end());
		pos = buf.size();
	}

	void writeU8(std::uint8_t v) {
		buf.push_back(static_cast<std::byte>(v));
		pos = buf.size();
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
};

struct ReadBuffer {
	const std::vector<std::byte>& buf;
	std::size_t pos = 0;

	explicit ReadBuffer(const std::vector<std::byte>& b) : buf(b) {}

	bool canRead(std::size_t n) const {
		return pos + n <= buf.size();
	}

	bool readU8(std::uint8_t& out) {
		if (!canRead(1)) return false;
		out = static_cast<std::uint8_t>(buf[pos++]);
		return true;
	}

	bool readU16(std::uint16_t& out) {
		std::uint8_t b0 = 0, b1 = 0;
		if (!readU8(b0) || !readU8(b1)) return false;
		out = static_cast<std::uint16_t>(b0 | (static_cast<std::uint16_t>(b1) << 8));
		return true;
	}

	bool readU32(std::uint32_t& out) {
		std::uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
		if (!readU8(b0) || !readU8(b1) || !readU8(b2) || !readU8(b3)) return false;
		out = static_cast<std::uint32_t>(b0)
			| (static_cast<std::uint32_t>(b1) << 8)
			| (static_cast<std::uint32_t>(b2) << 16)
			| (static_cast<std::uint32_t>(b3) << 24);
		return true;
	}

	bool readU64(std::uint64_t& out) {
		std::uint32_t lo = 0, hi = 0;
		if (!readU32(lo) || !readU32(hi)) return false;
		out = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
		return true;
	}

	bool readCString(std::string& out) {
		out.clear();
		for (;;) {
			if (!canRead(1)) {
				return false;
			}
			const auto c = static_cast<char>(buf[pos++]);
			if (c == '\0') {
				return true;
			}
			out.push_back(c);
		}
	}
};

[[nodiscard]] bool endsWithInsensitive(std::string_view s, std::string_view suffix) {
	if (s.size() < suffix.size()) {
		return false;
	}
	auto it = s.end() - static_cast<std::ptrdiff_t>(suffix.size());
	for (std::size_t i = 0; i < suffix.size(); i++) {
		const auto a = static_cast<unsigned char>(it[i]);
		const auto b = static_cast<unsigned char>(suffix[i]);
		if (std::tolower(a) != std::tolower(b)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool validateDirTreeAgainstInput(const std::vector<std::byte>& dirTree, const std::vector<DirEntry>& entries, std::string* outError) {
	std::unordered_set<std::string> expected;
	expected.reserve(entries.size());
	for (const auto& e : entries) {
		expected.insert(e.path);
	}

	ReadBuffer r(dirTree);
	std::unordered_set<std::string> seen;
	seen.reserve(entries.size());

	std::string ext, dir, file;
	std::uint32_t crc = 0;
	std::uint16_t preloadBytes = 0;
	std::uint16_t packFileIndex = 0;

	for (;;) {
		if (!r.readCString(ext)) {
			if (outError) *outError = "Dir tree parse failed while reading extension";
			return false;
		}
		if (ext.empty()) {
			break;
		}
		for (;;) {
			if (!r.readCString(dir)) {
				if (outError) *outError = "Dir tree parse failed while reading directory";
				return false;
			}
			if (dir.empty()) {
				break;
			}
			if (!dir.empty() && dir[0] == ' ' && dir != " ") {
				if (outError) *outError = "Dir tree corruption detected (directory begins with a space): '" + dir + "'";
				return false;
			}
			for (;;) {
				if (!r.readCString(file)) {
					if (outError) *outError = "Dir tree parse failed while reading filename";
					return false;
				}
				if (file.empty()) {
					break;
				}

				std::string fullPath = file;
				if (fullPath == " ") {
					fullPath.clear();
				}
				if (ext != " ") {
					fullPath += '.';
					fullPath += ext;
				}
				if (dir != " " && !dir.empty()) {
					fullPath = dir + '/' + fullPath;
				}

				if (!r.readU32(crc) || !r.readU16(preloadBytes)) {
					if (outError) *outError = "Dir tree parse failed while reading entry header";
					return false;
				}
				if (!r.readU16(packFileIndex)) {
					if (outError) *outError = "Dir tree parse failed while reading pack file index";
					return false;
				}
				(void)packFileIndex;

				for (;;) {
					std::uint32_t loadFlags = 0;
					std::uint16_t textureFlags = 0;
					std::uint64_t off = 0, len = 0, ulen = 0;
					std::uint16_t marker = 0;
					if (!r.readU32(loadFlags) || !r.readU16(textureFlags) || !r.readU64(off) || !r.readU64(len) || !r.readU64(ulen) || !r.readU16(marker)) {
						if (outError) *outError = "Dir tree parse failed while reading part";
						return false;
					}
					if (marker == RESPAWN_CHUNK_END_MARKER) {
						break;
					}
					if (marker != RESPAWN_CHUNK_CONT_MARKER) {
						if (outError) *outError = "Dir tree corruption detected (invalid chunk marker)";
						return false;
					}
				}

				seen.insert(fullPath);
			}
		}
	}

	if (seen.size() != expected.size()) {
		if (outError) {
			*outError = "Dir tree validation failed (entry count mismatch): expected " + std::to_string(expected.size()) + ", got " + std::to_string(seen.size());
		}
		return false;
	}
	for (const auto& p : expected) {
		if (!seen.contains(p)) {
			if (outError) *outError = "Dir tree validation failed (missing path): " + p;
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::string toLower(std::string s) {
	for (auto& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

[[nodiscard]] std::string stripPakLang(std::string s) {
	static constexpr std::array<std::string_view, 12> langs{
		"english", "french", "german", "italian", "japanese", "korean",
		"polish", "portugese", "russian", "spanish", "tchinese", "schinese"
	};
	for (const auto& lang : langs) {
		for (;;) {
			const auto pos = s.find(lang);
			if (pos == std::string::npos) {
				break;
			}
			s.erase(pos, lang.size());
		}
	}
	return s;
}

[[nodiscard]] std::string stripPakLangFilenamePrefix(const std::string& path) {
	static constexpr std::array<std::string_view, 12> langs{
		"english", "french", "german", "italian", "japanese", "korean",
		"polish", "portugese", "russian", "spanish", "tchinese", "schinese"
	};

	const auto p = std::filesystem::path{path};
	const auto name = p.filename().string();
	const auto nameLower = toLower(name);

	for (const auto& lang : langs) {
		if (nameLower.rfind(std::string{lang}, 0) == 0) {
			const auto stripped = name.substr(lang.size());
			return (p.parent_path() / stripped).string();
		}
	}
	return path;
}

[[nodiscard]] std::string makeArchivePath(const std::string& dirVpkPath, std::uint16_t archiveIndex) {
	std::string base = dirVpkPath;
	{
		constexpr std::string_view suffix = "_dir.vpk";
		if (endsWithInsensitive(base, suffix)) {
			char buf[16];
			std::snprintf(buf, sizeof(buf), "_%03u.vpk", static_cast<unsigned>(archiveIndex));
			base.replace(base.size() - suffix.size(), suffix.size(), buf);
		}
	}
	return stripPakLangFilenamePrefix(base);
}

[[nodiscard]] std::optional<CamEntry> tryMakeCamEntry(const std::vector<std::byte>& wavFile, const std::string& path) {
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

[[nodiscard]] std::vector<std::byte> lzhamCompress(std::span<const std::byte> in) {
#ifdef VPKEDIT_HAVE_LZHAM
	const auto slack = std::min<std::size_t>(std::max<std::size_t>(in.size() / 16, 1024), 64 * 1024);
	std::vector<std::byte> out(std::max<std::size_t>(in.size() + slack, 1));
	for (int tries = 0; tries < 6; tries++) {
		size_t outLen = out.size();
		const auto rc = lzham_bridge_compress(
			reinterpret_cast<const std::uint8_t*>(in.data()), in.size(),
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

	return {in.begin(), in.end()};
#else
	return {in.begin(), in.end()};
#endif
}

[[nodiscard]] std::string getExtensionLower(std::string_view path) {
	auto p = std::filesystem::path{path};
	auto ext = p.extension().string();
	if (!ext.empty() && ext[0] == '.') {
		ext.erase(0, 1);
	}
	return toLower(ext);
}

[[nodiscard]] DirEntry buildDirEntryFromFile(
	const std::filesystem::path& baseDir,
	const std::filesystem::path& absPath,
	const PackOptions& options,
	const ManifestMap* manifest,
	std::vector<CamEntry>& camEntries) {

	DirEntry out;

	std::error_code ec;
	auto rel = std::filesystem::relative(absPath, baseDir, ec).string();
	sourcepp::string::normalizeSlashes(rel, true, true);
	out.path = rel;

	const auto extLower = getExtensionLower(rel);
	const auto filename = std::filesystem::path{rel}.filename().string();

	if (extLower.empty()) {
		out.extension = std::string{" "} + '\0';
		out.fileName = filename + '\0';
	} else {
		out.extension = extLower + '\0';
		out.fileName = std::filesystem::path{filename}.replace_extension().string() + '\0';
	}

	auto dir = std::filesystem::path{rel}.parent_path().string();
	sourcepp::string::normalizeSlashes(dir, true, true);
	if (dir.empty()) {
		out.directory = std::string{" "} + '\0';
	} else {
		out.directory = dir + '\0';
	}

	auto file = fs::readFileBuffer(absPath.string());

	if (extLower == "wav") {
		if (auto cam = tryMakeCamEntry(file, out.path)) {
			camEntries.push_back(*cam);
		}
		stripWavHeaderInPlace(file);
	}

	out.crc32 = crypto::computeCRC32(std::span<const std::byte>{file.data(), file.size()});
	out.preloadBytes = 0;
	out.packFileIndex = options.archiveIndex;

	static const std::unordered_set<std::string> compressionExcluded{"wav", "vtf"};

	ManifestEntry values{};
	bool haveManifestValues = false;
	if (manifest) {
		const auto key = normalizeManifestPath(out.path);
		if (const auto it = manifest->find(key); it != manifest->end()) {
			values = it->second;
			haveManifestValues = true;
			out.preloadBytes = values.preloadSize;
			out.packFileIndex = options.archiveIndex;
		}
	}

	std::size_t offset = 0;
	while (offset < file.size()) {
		const auto partLen = std::min<std::size_t>(options.maxPartSize, file.size() - offset);
		const auto partSpan = std::span<const std::byte>{file.data() + offset, partLen};

		auto partData = std::vector<std::byte>(partSpan.begin(), partSpan.end());

		bool doCompress = partLen >= options.compressionThreshold && !compressionExcluded.contains(extLower);
		if (haveManifestValues) {
			doCompress = values.useCompression && !compressionExcluded.contains(extLower);
		}
		if (doCompress) {
			const auto compressed = lzhamCompress(partSpan);
			if (compressed.size() < partData.size()) {
				partData = compressed;
			} else {
				doCompress = false;
			}
		}

		FilePart part;
		part.textureFlags = 0;
		part.entryOffset = 0;
		part.entryLength = static_cast<std::uint64_t>(partData.size());
		part.entryLengthUncompressed = static_cast<std::uint64_t>(partLen);
		part.dataCrc32 = crypto::computeCRC32(std::span<const std::byte>{partData.data(), partData.size()});
		part.data = std::move(partData);

		if (haveManifestValues) {
			part.loadFlags = values.loadFlags;
			part.textureFlags = values.textureFlags;
		} else {
			part.loadFlags = LOAD_VISIBLE;
			if (extLower == "wav") {
				part.loadFlags = static_cast<std::uint32_t>(LOAD_VISIBLE | LOAD_CACHE);
			} else if (extLower == "acache") {
				part.loadFlags = static_cast<std::uint32_t>(LOAD_VISIBLE | LOAD_CACHE | LOAD_ACACHE_UNK0);
			}
			if (extLower == "vtf") {
				part.textureFlags = TEXTURE_DEFAULT;
			}
		}

		out.parts.push_back(std::move(part));
		offset += partLen;
	}

	return out;
}

[[nodiscard]] bool writeArchiveFile(
	std::vector<DirEntry>& entries,
	const ManifestMap* manifest,
	const std::string& archivePath,
	std::string* outError) {
	std::ofstream f{archivePath, std::ios::binary};
	if (!f) {
		if (outError) {
			*outError = "Failed to open for write: " + archivePath;
		}
		return false;
	}

	std::vector<char> iobuf(8 * 1024 * 1024);
	f.rdbuf()->pubsetbuf(iobuf.data(), static_cast<std::streamsize>(iobuf.size()));

	struct DedupRef {
		std::uint64_t offset = 0;
		const std::vector<std::byte>* data = nullptr;
	};
	std::unordered_map<std::uint64_t, std::vector<DedupRef>> dedup;
	dedup.reserve(entries.size() * 2);

	std::uint64_t writePos = 0;

	for (auto& e : entries) {
		bool allowDedup = true;
		if (manifest) {
			const auto key = normalizeManifestPath(e.path);
			if (const auto it = manifest->find(key); it != manifest->end()) {
				allowDedup = it->second.deDuplicate;
			}
		}

		for (auto& p : e.parts) {
			const auto size = static_cast<std::uint64_t>(p.data.size());
			if (size == 0) {
				p.entryOffset = writePos;
				continue;
			}

			bool wrote = false;
			if (allowDedup) {
				const std::uint64_t h = (static_cast<std::uint64_t>(p.dataCrc32) << 32) | size;
				if (auto it = dedup.find(h); it != dedup.end()) {
					for (const auto& cand : it->second) {
						if (!cand.data || cand.data->size() != p.data.size()) {
							continue;
						}
						if (std::memcmp(cand.data->data(), p.data.data(), p.data.size()) == 0) {
							p.entryOffset = cand.offset;
							wrote = true;
							break;
						}
					}
				}
				if (!wrote) {
					p.entryOffset = writePos;
					f.write(reinterpret_cast<const char*>(p.data.data()), static_cast<std::streamsize>(p.data.size()));
					if (!f) {
						if (outError) {
							*outError = "Failed to write archive: " + archivePath;
						}
						return false;
					}
					dedup[h].push_back(DedupRef{writePos, &p.data});
					writePos += size;
				}
			} else {
				p.entryOffset = writePos;
				f.write(reinterpret_cast<const char*>(p.data.data()), static_cast<std::streamsize>(p.data.size()));
				if (!f) {
					if (outError) {
						*outError = "Failed to write archive: " + archivePath;
					}
					return false;
				}
				writePos += size;
			}
		}
	}

	return true;
}

[[nodiscard]] std::vector<std::byte> buildCam(const std::vector<DirEntry>& entries, std::vector<CamEntry>& cams) {
	for (const auto& e : entries) {
		if (!endsWithInsensitive(e.extension, "wav\0")) {
			continue;
		}
		auto it = std::find_if(cams.begin(), cams.end(), [&](const CamEntry& c) { return c.path == e.path; });
		if (it == cams.end()) {
			continue;
		}
		if (!e.parts.empty()) {
			it->vpkContentOffset = e.parts.front().entryOffset;
		}
	}

	WriteBuffer w(cams.size() * CAM_ENTRY_BYTES);
	for (const auto& e : entries) {
		if (!endsWithInsensitive(e.extension, "wav\0")) {
			continue;
		}
		const auto it = std::find_if(cams.begin(), cams.end(), [&](const CamEntry& c) { return c.path == e.path; });
		if (it == cams.end()) {
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
	return std::move(w.buf);
}

[[nodiscard]] std::vector<std::byte> buildDirTree(const std::vector<DirEntry>& entries, std::uint16_t archiveIndex) {
	std::size_t est = 0;
	for (const auto& e : entries) {
		est += e.extension.size() + e.directory.size() + e.fileName.size();
		est += e.parts.size() * 32;
		est += 12;
	}
	WriteBuffer w(est);

	std::string lastExt;
	std::string lastDir;

	for (const auto& e : entries) {
		if (e.extension != lastExt && !lastExt.empty()) {
			w.writeU16(0);
			lastDir.clear();
		} else if (e.directory != lastDir && !lastDir.empty()) {
			w.writeU8(0);
		}

		if (e.extension != lastExt) {
			w.writeBytes(std::span<const std::byte>{reinterpret_cast<const std::byte*>(e.extension.data()), e.extension.size()});
			lastExt = e.extension;
		}
		if (e.directory != lastDir) {
			w.writeBytes(std::span<const std::byte>{reinterpret_cast<const std::byte*>(e.directory.data()), e.directory.size()});
			lastDir = e.directory;
		}

		w.writeBytes(std::span<const std::byte>{reinterpret_cast<const std::byte*>(e.fileName.data()), e.fileName.size()});

		w.writeU32(e.crc32);
		w.writeU16(e.preloadBytes);
		w.writeU16(e.packFileIndex ? e.packFileIndex : archiveIndex);

		for (std::size_t i = 0; i < e.parts.size(); i++) {
			const auto& p = e.parts[i];
			w.writeU32(p.loadFlags);
			w.writeU16(p.textureFlags);
			w.writeU64(p.entryOffset);
			w.writeU64(p.entryLength);
			w.writeU64(p.entryLengthUncompressed);
			w.writeU16((i + 1 == e.parts.size()) ? RESPAWN_CHUNK_END_MARKER : RESPAWN_CHUNK_CONT_MARKER);
		}
	}

	w.writeU24(0);
	return std::move(w.buf);
}

[[nodiscard]] std::vector<std::byte> buildHeader(std::uint32_t treeLength) {
	WriteBuffer w(RESPAWN_VPK_HEADER_LEN);
	w.writeU32(RESPAWN_VPK_SIGNATURE);
	w.writeU16(RESPAWN_VPK_MAJOR_VERSION);
	w.writeU16(RESPAWN_VPK_MINOR_VERSION);
	w.writeU32(treeLength);
	w.writeU32(0); // signature size (unused)
	return std::move(w.buf);
}

[[nodiscard]] bool writeFileBinary(const std::string& path, std::span<const std::byte> data, std::string* outError) {
	std::ofstream f{path, std::ios::binary};
	if (!f) {
		if (outError) {
			*outError = "Failed to open for write: " + path;
		}
		return false;
	}
	f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
	if (!f) {
		if (outError) {
			*outError = "Failed to write: " + path;
		}
		return false;
	}
	return true;
}

} // namespace

std::uint16_t inferArchiveIndexFromDirVpkPath(std::string_view outputDirVpkPath, std::uint16_t fallback) {
	const auto p = std::filesystem::path{std::string{outputDirVpkPath}};
	const auto nameLower = toLower(p.filename().string());

	const auto pos = nameLower.rfind("pak");
	if (pos == std::string::npos || pos + 6 > nameLower.size()) {
		return fallback;
	}

	const auto d0 = nameLower[pos + 3];
	const auto d1 = nameLower[pos + 4];
	const auto d2 = nameLower[pos + 5];
	if (!std::isdigit(static_cast<unsigned char>(d0)) ||
		!std::isdigit(static_cast<unsigned char>(d1)) ||
		!std::isdigit(static_cast<unsigned char>(d2))) {
		return fallback;
	}

	const auto idx = static_cast<unsigned>((d0 - '0') * 100 + (d1 - '0') * 10 + (d2 - '0'));
	if (idx > 999) {
		return fallback;
	}
	return static_cast<std::uint16_t>(idx);
}

bool packDirectoryToRespawnVPK(const std::string& inputDir, const std::string& outputDirVpkPath, const PackOptions& options, std::string* outError) {
	if (!endsWithInsensitive(outputDirVpkPath, "_dir.vpk")) {
		if (outError) {
			*outError = "Output path must end with _dir.vpk";
		}
		return false;
	}

	std::error_code ec;
	if (!std::filesystem::exists(inputDir, ec) || !std::filesystem::is_directory(inputDir, ec)) {
		if (outError) {
			*outError = "Input path is not a directory: " + inputDir;
		}
		return false;
	}

	std::vector<DirEntry> entries;
	std::vector<CamEntry> camEntries;

	const auto manifestOpt = readManifestForDirVpkPath(std::filesystem::path{outputDirVpkPath});
	const ManifestMap* manifest = manifestOpt ? &*manifestOpt : nullptr;

	std::vector<std::filesystem::path> filePaths;
	for (const auto& it : std::filesystem::recursive_directory_iterator{inputDir, std::filesystem::directory_options::skip_permission_denied, ec}) {
		if (ec) {
			ec.clear();
			continue;
		}
		if (!it.is_regular_file(ec)) {
			ec.clear();
			continue;
		}
		ec.clear();
		filePaths.push_back(it.path());
	}

	entries.resize(filePaths.size());

	std::mutex camMutex;
	std::mutex errMutex;
	std::string firstError;

	std::atomic_size_t nextIndex{0};
	std::atomic_bool failed{false};

	auto workerFn = [&]() {
		std::vector<CamEntry> localCams;
		for (;;) {
			if (failed.load(std::memory_order_relaxed)) {
				break;
			}
			const auto i = nextIndex.fetch_add(1, std::memory_order_relaxed);
			if (i >= filePaths.size()) {
				break;
			}
			try {
				entries[i] = buildDirEntryFromFile(std::filesystem::path{inputDir}, filePaths[i], options, manifest, localCams);
			} catch (const std::exception& e) {
				{
					std::scoped_lock lock(errMutex);
					if (firstError.empty()) {
						firstError = std::string{"Exception while reading/compressing: "} + filePaths[i].string() + "\n" + e.what();
					}
				}
				failed.store(true, std::memory_order_relaxed);
				break;
			} catch (...) {
				{
					std::scoped_lock lock(errMutex);
					if (firstError.empty()) {
						firstError = std::string{"Unknown exception while reading/compressing: "} + filePaths[i].string();
					}
				}
				failed.store(true, std::memory_order_relaxed);
				break;
			}
		}
		if (!localCams.empty()) {
			std::scoped_lock lock(camMutex);
			camEntries.insert(camEntries.end(), localCams.begin(), localCams.end());
		}
	};

	std::size_t threadCount = options.threadCount;
	if (threadCount == 0) {
		threadCount = std::max<std::size_t>(1, std::thread::hardware_concurrency());
		threadCount = std::min<std::size_t>(threadCount, std::max<std::size_t>(1, filePaths.size()));
		threadCount = std::min<std::size_t>(threadCount, 16);
	}
	threadCount = std::max<std::size_t>(1, std::min<std::size_t>(threadCount, std::max<std::size_t>(1, filePaths.size())));

	std::vector<std::thread> workers;
	workers.reserve(threadCount);
	for (std::size_t i = 0; i < threadCount; i++) {
		workers.emplace_back(workerFn);
	}
	for (auto& t : workers) {
		t.join();
	}

	if (failed.load(std::memory_order_relaxed)) {
		if (outError) {
			std::scoped_lock lock(errMutex);
			*outError = !firstError.empty() ? firstError : "Failed to pack due to an unknown error while reading/compressing files.";
		}
		return false;
	}

	std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
		return (a.extension + a.directory + a.fileName) < (b.extension + b.directory + b.fileName);
	});

	const auto dirTree = buildDirTree(entries, options.archiveIndex);
	const auto header = buildHeader(static_cast<std::uint32_t>(dirTree.size()));

	if (!validateDirTreeAgainstInput(dirTree, entries, outError)) {
		return false;
	}

	{
		std::vector<std::byte> dirVpk;
		dirVpk.reserve(header.size() + dirTree.size());
		dirVpk.insert(dirVpk.end(), header.begin(), header.end());
		dirVpk.insert(dirVpk.end(), dirTree.begin(), dirTree.end());
		if (!writeFileBinary(outputDirVpkPath, dirVpk, outError)) {
			return false;
		}
	}

	const auto archivePath = makeArchivePath(outputDirVpkPath, options.archiveIndex);
	if (!writeArchiveFile(entries, manifest, archivePath, outError)) {
		return false;
	}

	if (!camEntries.empty()) {
		auto cam = buildCam(entries, camEntries);
		if (!writeFileBinary(archivePath + ".cam", cam, outError)) {
			return false;
		}
	}

	{
		std::vector<ManifestWriteItem> mani;
		mani.reserve(entries.size());
		for (const auto& e : entries) {
			ManifestWriteItem m;
			m.path = e.path;
			m.values.preloadSize = e.preloadBytes;
			if (!e.parts.empty()) {
				m.values.loadFlags = e.parts.front().loadFlags;
				m.values.textureFlags = e.parts.front().textureFlags;
				m.values.useCompression = (e.parts.front().entryLength != e.parts.front().entryLengthUncompressed);
			}
			m.values.deDuplicate = true;
			mani.push_back(std::move(m));
		}
		std::string err;
		(void)writeManifestForDirVpkPath(std::filesystem::path{outputDirVpkPath}, mani, &err);
	}

	return true;
}

} // namespace respawn_vpk
