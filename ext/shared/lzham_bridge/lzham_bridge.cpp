#define LZHAM_BRIDGE_EXPORTS 1
#include "lzham_bridge.h"

#include <lzham.h>

namespace {

constexpr lzham_uint32 kDictSizeLog2 = 20;

static const lzham_decompress_params kDecompressParams = {
	.m_struct_size = sizeof(lzham_decompress_params),
	.m_dict_size_log2 = kDictSizeLog2,
	.m_decompress_flags = LZHAM_DECOMP_FLAG_OUTPUT_UNBUFFERED | LZHAM_DECOMP_FLAG_COMPUTE_ADLER32 | LZHAM_DECOMP_FLAG_COMPUTE_CRC32,
};

static const lzham_compress_params kCompressParams = {
	.m_struct_size = sizeof(lzham_compress_params),
	.m_dict_size_log2 = kDictSizeLog2,
	// Match revpk/engine defaults ("default" level). This corresponds to the typical "level 6" expectation.
	.m_level = lzham_compress_level::LZHAM_COMP_LEVEL_DEFAULT,
	.m_compress_flags = LZHAM_COMP_FLAG_DETERMINISTIC_PARSING,
};

} // namespace

extern "C" int lzham_bridge_decompress(
	const std::uint8_t* src, std::size_t srcLen,
	std::uint8_t* dst, std::size_t* dstLen) {
	if (!src || !dst || !dstLen || !*dstLen) {
		return 1;
	}

	size_t outLen = *dstLen;
	lzham_uint32 adler32 = 0, crc32 = 0;
	const auto status = lzham_decompress_memory(
		&kDecompressParams,
		dst, &outLen,
		src, srcLen,
		&adler32, &crc32);

	if (status != LZHAM_DECOMP_STATUS_SUCCESS || outLen == 0 || outLen > *dstLen) {
		return 2;
	}

	*dstLen = outLen;
	return 0;
}

extern "C" int lzham_bridge_compress(
	const std::uint8_t* src, std::size_t srcLen,
	std::uint8_t* dst, std::size_t* dstLen) {
	if (!src || !dst || !dstLen || !*dstLen) {
		return 1;
	}

	lzham_compress_init(&kCompressParams);

	size_t outLen = *dstLen;
	lzham_uint32 adler32 = 0, crc32 = 0;
	const auto status = lzham_compress_memory(
		&kCompressParams,
		dst, &outLen,
		src, srcLen,
		&adler32, &crc32);

	// Propagate the only retryable condition.
	if (status == LZHAM_COMP_STATUS_OUTPUT_BUF_TOO_SMALL) {
		*dstLen = outLen;
		return 3;
	}
	if (status != LZHAM_COMP_STATUS_SUCCESS || outLen == 0 || outLen > *dstLen) {
		return 2;
	}

	*dstLen = outLen;
	return 0;
}
