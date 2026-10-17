// rbxrsb1.cpp — CLI for the RSB1 script-container pipeline.
//
//   rbxrsb1 info FILE              container inspection (length, path, header)
//   rbxrsb1 key FILE               show the scramble key recovered from W[0..3]
//   rbxrsb1 decode FILE [-o OUT]   descramble + verify + decompress → bytecode
//   rbxrsb1 encode (--size0 | --zstd) FILE [-o OUT] [--level N]
//                                  bytecode → scrambled container
//
// `decode` mirrors RobloxPlayer FUN_1014b91b5 (VA 0x1014b91b5); `encode` is
// the inverse (the player only decodes — the encoder lives server-side).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rbxvm/rsb1.hpp"
#include "rbxvm/lbc.hpp"
#include "rbxvm/zstd_loader.hpp"

using namespace rbxvm;

namespace {

std::vector<uint8_t> readFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return {}; }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    return data;
}

bool writeFile(const char* path, const std::vector<uint8_t>& data) {
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::perror(path); return false; }
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return true;
}

const char* statusName(rsb1::DecodeStatus s) {
    switch (s) {
    case rsb1::DecodeStatus::OkPlain: return "OkPlain";
    case rsb1::DecodeStatus::OkZstd: return "OkZstd";
    case rsb1::DecodeStatus::TooShort: return "TooShort";
    case rsb1::DecodeStatus::BadChecksum: return "BadChecksum";
    case rsb1::DecodeStatus::BadPayload: return "BadPayload";
    case rsb1::DecodeStatus::ZstdError: return "ZstdError";
    case rsb1::DecodeStatus::ZstdUnavailable: return "ZstdUnavailable";
    }
    return "?";
}

int cmdInfo(const char* path) {
    std::vector<uint8_t> w = readFile(path);
    if (w.empty()) return 1;
    std::printf("file: %s\n", path);
    std::printf("length: %zu\n", w.size());
    std::printf("first 16 bytes:");
    for (size_t i = 0; i < 16 && i < w.size(); ++i) std::printf(" %02x", w[i]);
    std::printf("\n");
    if (w.size() >= 8) {
        uint64_t m;
        std::memcpy(&m, w.data(), 8);
        std::printf("plain-bytecode magic: %s (%016llx)\n",
                    m == rsb1::kPlainBytecodeMagic ? "YES" : "no", (unsigned long long)m);
    }
    if (w.size() >= 4) {
        rsb1::Key k = rsb1::deriveKey(w.data());
        std::printf("derived key: %02x %02x %02x %02x (as u32 %08x)\n",
                    k.b[0], k.b[1], k.b[2], k.b[3],
                    *reinterpret_cast<uint32_t*>(k.b));
    }
    rsb1::Decoded d = rsb1::decodeContainer(w.data(), w.size());
    std::printf("decode status: %s\n", statusName(d.status));
    if (d.status == rsb1::DecodeStatus::OkPlain || d.status == rsb1::DecodeStatus::OkZstd) {
        std::printf("payload: %zu bytes (declared decompressed size %u)\n",
                    d.payload.size(), d.declared_size);
    }
    return 0;
}

int cmdKey(const char* path) {
    std::vector<uint8_t> w = readFile(path);
    if (w.size() < 4) { std::fprintf(stderr, "need >= 4 bytes\n"); return 1; }
    rsb1::Key k = rsb1::deriveKey(w.data());
    std::printf("%02x%02x%02x%02x\n", k.b[0], k.b[1], k.b[2], k.b[3]);
    return 0;
}

int cmdDecode(int argc, char** argv) {
    const char* path = argv[0];
    const char* out = nullptr;
    for (int i = 1; i < argc; ++i)
        if (!std::strncmp(argv[i], "-o", 2)) out = argv[i][2] ? argv[i] + 2 : (++i < argc ? argv[i] : nullptr);
    std::vector<uint8_t> w = readFile(path);
    if (w.empty()) return 1;
    rsb1::Decoded d = rsb1::decodeContainer(w.data(), w.size());
    std::printf("status: %s\n", statusName(d.status));
    if (d.status != rsb1::DecodeStatus::OkPlain && d.status != rsb1::DecodeStatus::OkZstd)
        return 1;
    std::printf("payload: %zu bytes\n", d.payload.size());
    if (out) {
        if (!writeFile(out, d.payload)) return 1;
        std::printf("wrote %s\n", out);
    } else {
        std::printf("first 32 bytes:");
        for (size_t i = 0; i < 32 && i < d.payload.size(); ++i) std::printf(" %02x", d.payload[i]);
        std::printf("\n");
    }
    return 0;
}

int cmdEncode(int argc, char** argv) {
    bool useZstd = false;
    bool wireOps = false;
    const char* path = nullptr;
    const char* out = nullptr;
    int level = 3;
    for (int i = 0; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--size0")) useZstd = false;
        else if (!std::strcmp(a, "--zstd")) useZstd = true;
        else if (!std::strcmp(a, "--wire")) wireOps = true; // scramble op bytes: canon -> wire
        else if (!std::strncmp(a, "--level=", 8)) level = std::atoi(a + 8);
        else if (!std::strncmp(a, "-o", 2)) out = a[2] ? a + 2 : (++i < argc ? argv[i] : nullptr);
        else if (!path) path = a;
        else { std::fprintf(stderr, "unexpected arg '%s'\n", a); return 2; }
    }
    if (!path) { std::fprintf(stderr, "encode: need an input bytecode file\n"); return 2; }

    std::vector<uint8_t> body = readFile(path);
    if (body.empty()) return 1;

    if (wireOps) lbc::convertOps(body.data(), body.size(), OpEnc::Canon, OpEnc::Wire);

    std::vector<uint8_t> plain;
    if (useZstd) {
        if (!rsb1::zstdAvailable()) { std::fprintf(stderr, "libzstd unavailable\n"); return 1; }
        // worst case bound per zstd docs: srcSize + srcSize/255 + 64
        std::vector<uint8_t> comp(body.size() + body.size() / 255 + 64);
        long n = rsb1::zstdCompress(comp.data(), comp.size(), body.data(), body.size(), level);
        if (n <= 0) { std::fprintf(stderr, "zstd compress failed (%ld)\n", n); return 1; }
        comp.resize(static_cast<size_t>(n));
        plain = rsb1::buildPlain(comp.data(), comp.size(), static_cast<uint32_t>(body.size()));
    } else {
        plain = rsb1::buildPlain(body.data(), body.size(), 0);
    }

    std::vector<uint8_t> wire = rsb1::encodeContainer(plain);
    if (out) {
        if (!writeFile(out, wire)) return 1;
        std::printf("wrote %s (%zu bytes, key ", out, wire.size());
    } else {
        std::printf("container: %zu bytes, key ", wire.size());
    }
    rsb1::Key k = rsb1::deriveKey(wire.data());
    std::printf("%02x%02x%02x%02x\n", k.b[0], k.b[1], k.b[2], k.b[3]);
    if (out) return 0;
    // no -o: round-trip check to stdout instead of dumping
    rsb1::Decoded d = rsb1::decodeContainer(wire.data(), wire.size());
    std::printf("self-check: %s, payload %zu bytes\n", statusName(d.status), d.payload.size());
    return (d.status == rsb1::DecodeStatus::OkPlain || d.status == rsb1::DecodeStatus::OkZstd) ? 0 : 1;
}

void usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  rbxrsb1 info FILE\n"
                 "  rbxrsb1 key FILE\n"
                 "  rbxrsb1 decode FILE [-o OUT]\n"
                 "  rbxrsb1 encode (--size0 | --zstd) FILE [-o OUT] [--level=N]\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    if (!std::strcmp(argv[1], "info")) return cmdInfo(argv[2]);
    if (!std::strcmp(argv[1], "key")) return cmdKey(argv[2]);
    if (!std::strcmp(argv[1], "decode")) return cmdDecode(argc - 2, argv + 2);
    if (!std::strcmp(argv[1], "encode")) return cmdEncode(argc - 2, argv + 2);
    usage();
    return 2;
}
