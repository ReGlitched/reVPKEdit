#pragma once

#include <cstddef>
#include <cstdint>

// Small DLL wrapper around the prebuilt lzham static library.
// We build this DLL with /MT to match the bundled lzham.lib, so the main
// application can remain /MD (Qt) without RuntimeLibrary mismatches.

#if defined(_WIN32)
	#if defined(LZHAM_BRIDGE_EXPORTS)
		#define LZHAM_BRIDGE_API __declspec(dllexport)
	#else
		#define LZHAM_BRIDGE_API __declspec(dllimport)
	#endif
#else
	#define LZHAM_BRIDGE_API
#endif

extern "C" {

// Returns 0 on success, non-zero on failure.
// On success, *dstLen is set to the number of bytes written to dst.
LZHAM_BRIDGE_API int lzham_bridge_decompress(
	const std::uint8_t* src, std::size_t srcLen,
	std::uint8_t* dst, std::size_t* dstLen);

// Returns 0 on success.
// Returns 3 if the output buffer is too small.
// Returns other non-zero codes on failure.
// Caller must provide dst buffer capacity via *dstLen. On success (or too small), *dstLen is updated.
LZHAM_BRIDGE_API int lzham_bridge_compress(
	const std::uint8_t* src, std::size_t srcLen,
	std::uint8_t* dst, std::size_t* dstLen);

}
