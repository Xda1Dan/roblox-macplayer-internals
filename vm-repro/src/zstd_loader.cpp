// zstd_loader.cpp — dlopen-based libzstd access.
#include "rbxvm/zstd_loader.hpp"

#include <dlfcn.h>

namespace rbxvm {
namespace rsb1 {

namespace {

// Mirror of zstd's public entry points (ABI stable since 1.0).
using ZstdDecompressRaw = size_t (*)(void*, size_t, const void*, size_t);
using ZstdCompressRaw = size_t (*)(void*, size_t, const void*, size_t, int);

ZstdDecompressRaw g_decomp = nullptr;
ZstdCompressRaw g_comp = nullptr;
bool g_tried = false;

void ensureLoaded() {
    if (g_tried) return;
    g_tried = true;
    const char* candidates[] = {
        "libzstd.so.1",
        "libzstd.so",
        "/usr/lib/x86_64-linux-gnu/libzstd.so.1",
    };
    for (const char* name : candidates) {
        if (void* h = dlopen(name, RTLD_NOW | RTLD_LOCAL)) {
            g_decomp = reinterpret_cast<ZstdDecompressRaw>(dlsym(h, "ZSTD_decompress"));
            g_comp = reinterpret_cast<ZstdCompressRaw>(dlsym(h, "ZSTD_compress"));
            if (g_decomp) return;
        }
    }
}

} // namespace

bool zstdAvailable() {
    ensureLoaded();
    return g_decomp != nullptr;
}

long zstdDecompress(void* dst, size_t dstCap, const void* src, size_t srcLen) {
    ensureLoaded();
    if (!g_decomp) return -1;
    return static_cast<long>(g_decomp(dst, dstCap, src, srcLen));
}

long zstdCompress(void* dst, size_t dstCap, const void* src, size_t srcLen, int level) {
    ensureLoaded();
    if (!g_comp) return -1;
    return static_cast<long>(g_comp(dst, dstCap, src, srcLen, level));
}

} // namespace rsb1
} // namespace rbxvm
