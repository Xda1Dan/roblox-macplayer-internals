// zstd_loader.hpp — runtime-loaded libzstd access for the container decoder.
//
// The binary links zstd statically (wrapper FUN_1032844e2 at VA 0x1032844e2:
// create-dctx → decompress → free-dctx). Our reproduction dlopens the system
// libzstd instead of requiring build-time headers — the one-shot
// ZSTD_decompress entry point is ABI-stable.
#pragma once

#include <cstddef>

namespace rbxvm {
namespace rsb1 {

// True if libzstd was found and the entry point resolved.
bool zstdAvailable();

// ZSTD_decompress(void* dst, size_t dstCapacity, const void* src, size_t
// srcSize) — returns decompressed size or a zstd error code (>0x80000000 as
// size_t). Returns the raw value; callers compare against the expected size.
long zstdDecompress(void* dst, size_t dstCap, const void* src, size_t srcLen);

// ZSTD_compress(void* dst, size_t dstCapacity, const void* src, size_t
// srcSize, int level) — returns compressed size or error. Available when
// zstdAvailable(); used by the encode side of the tooling only.
long zstdCompress(void* dst, size_t dstCap, const void* src, size_t srcLen, int level);

} // namespace rsb1
} // namespace rbxvm
