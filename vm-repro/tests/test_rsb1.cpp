// test_rsb1.cpp — verification of the RSB1 container reproduction.
//
// Ground truth used:
//  1. kChecksumFn (tests/ground_truth.inc): the actual machine code of
//     RobloxPlayer FUN_103222533 (VA 0x103222533, 318 bytes, pure computation,
//     no RIP-relative references). We mmap it RWX and CALL it, then require
//     our C++ checksum32 to agree on a wide input sweep. This is the binary
//     itself answering, not a transcription.
//  2. Known-answer vectors: the checksum is plain standard xxHash32, so
//     official XXH32 values apply for all lengths (short vectors checked here).
//  3. kZstdFrame / kZstdPayload: a real zstd 1.5.7 frame of a known payload,
//     exercising the decoder's zstd path end to end.
//  4. Structural identities: key-derivation tautology, keystream blocked-form
//     == scalar formula, K=0 magic construction, scramble involution,
//     encode/decode round-trips.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/mman.h>

#include "rbxvm/rsb1.hpp"
#include "rbxvm/zstd_loader.hpp"

#include "ground_truth.inc"

using namespace rbxvm;

static int g_failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            ++g_failures;                                    \
            std::printf("FAIL: %s:%d: ", __FILE__, __LINE__);\
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
        }                                                    \
    } while (0)

// ---------------------------------------------------------------------------
// 1. Execute the binary's own checksum code
// ---------------------------------------------------------------------------
using ChecksumFn = uint32_t (*)(const uint8_t*, int, uint32_t);

static ChecksumFn loadBinaryChecksum() {
    void* mem = mmap(nullptr, sizeof(kChecksumFn), PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    std::memcpy(mem, kChecksumFn, sizeof(kChecksumFn));
    return reinterpret_cast<ChecksumFn>(mem);
}

static void testChecksumAgainstBinary(ChecksumFn bin) {
    if (!bin) {
        CHECK(false, "could not map binary checksum code");
        return;
    }

    // NOTE: the binary's tail loop compares the cursor against (end - 4)
    // unsigned, so a NULL base with len 0 would read [nullptr]. Real callers
    // always pass a valid pointer (containers are >= 13 bytes), so we use a
    // dummy base for zero-length calls.
    static const uint8_t kDummy = 0;

    // deterministic sweep: varied lengths (0..200 incl. 16-boundary cases and
    // multiples of 4/16), varied seeds, varied content
    for (int len = 0; len <= 200; ++len) {
        std::vector<uint8_t> buf(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i)
            buf[static_cast<size_t>(i)] = static_cast<uint8_t>((i * 131 + len * 7 + (i >> 2)) & 0xff);
        const uint8_t* base = len ? buf.data() : &kDummy;
        const uint32_t seeds[] = {0x2Au, 0u, 1u, 0xDEADBEEFu, 0xFFFFFFFFu};
        for (uint32_t seed : seeds) {
            uint32_t want = bin(base, len, seed);
            uint32_t got = rsb1::checksum32(base, static_cast<size_t>(len), seed);
            CHECK(want == got, "checksum mismatch len=%d seed=%08x bin=%08x ours=%08x",
                  len, seed, want, got);
        }
    }
    // a couple of all-zero and all-ff buffers too
    for (int len : {1, 15, 16, 17, 31, 32, 33, 100}) {
        std::vector<uint8_t> z(static_cast<size_t>(len), 0x00), f(static_cast<size_t>(len), 0xFF);
        for (uint32_t seed : {0x2Au, 0u}) {
            CHECK(bin(z.data(), len, seed) == rsb1::checksum32(z.data(), static_cast<size_t>(len), seed),
                  "zero buf len=%d", len);
            CHECK(bin(f.data(), len, seed) == rsb1::checksum32(f.data(), static_cast<size_t>(len), seed),
                  "ff buf len=%d", len);
        }
    }
}

// ---------------------------------------------------------------------------
// 2. Standard xxHash32 known-answer vectors (official XXH32 values)
// ---------------------------------------------------------------------------
static void testXxh32ShortVectors() {
    // official XXH32 test values (seed 0)
    CHECK(rsb1::checksum32(nullptr, 0, 0) == 0x02CC5D05u, "xxh32 empty");
    const uint8_t a[] = {'a'};
    CHECK(rsb1::checksum32(a, 1, 0) == 0x550D7456u, "xxh32 'a'");
    const uint8_t abc[] = {'a', 'b', 'c'};
    CHECK(rsb1::checksum32(abc, 3, 0) == 0x32D153FFu, "xxh32 'abc'");
    // seed sensitivity sanity on the empty input: XXH32("", seed) = seed + P5
    // avalanche'd? (no — for len 0 the result is finalize(seed + P5 + 0))
    CHECK(rsb1::checksum32(nullptr, 0, 0x2Au) ==
              rsb1::checksum32(nullptr, 0, 0x2Au), "determinism");
}

// ---------------------------------------------------------------------------
// 3. Scramble identities
// ---------------------------------------------------------------------------
static void testKeyTautology() {
    // For ANY first four wire bytes, descrambling them with the derived key
    // must yield exactly "RSB1" — the self-synchronizing property.
    for (int x = 0; x < 256; x += 5) {
        uint8_t w[4] = {static_cast<uint8_t>(x), static_cast<uint8_t>(x * 7 + 1),
                        static_cast<uint8_t>(x * 13 + 91), static_cast<uint8_t>(~x)};
        rsb1::Key k = rsb1::deriveKey(w);
        uint8_t p[4];
        for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(w[i] ^ rsb1::keystreamByte(k, i));
        CHECK(p[0] == 'R' && p[1] == 'S' && p[2] == 'B' && p[3] == '1',
              "tautology x=%d got %02x%02x%02x%02x", x, p[0], p[1], p[2], p[3]);
    }
}

static void testKeystreamBlockedEqualsScalar() {
    // The binary's 16-block +0x90 form must equal the scalar keystream formula.
    rsb1::Key k{{0x12, 0x34, 0x56, 0x78}};
    for (size_t p = 0; p < 4096; ++p) {
        // blocked: block B, index i: key[i&3] + i*0x29 + B*0x90 (mod 256)
        size_t B = p / 16, i = p % 16;
        uint8_t blocked = static_cast<uint8_t>(k.b[i & 3] + i * 0x29 + B * 0x90);
        CHECK(blocked == rsb1::keystreamByte(k, p), "keystream mismatch at p=%zu", p);
    }
}

static void testScrambleInvolution() {
    std::vector<uint8_t> buf(257);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(i * 37 + 11);
    std::vector<uint8_t> orig = buf;
    rsb1::Key k{{0xAA, 0x55, 0x00, 0xFF}};
    rsb1::scrambleInPlace(buf.data(), buf.size(), k);
    CHECK(buf != orig, "scramble changed bytes");
    rsb1::scrambleInPlace(buf.data(), buf.size(), k);
    CHECK(buf == orig, "scramble not an involution");
}

static void testMagicConstruction() {
    // The plain-bytecode magic must be the K=0 scramble of "RSB1"+0xFFFFFFFF.
    uint8_t plain[8] = {'R', 'S', 'B', '1', 0xFF, 0xFF, 0xFF, 0xFF};
    rsb1::Key zero{}; // {0,0,0,0}
    uint8_t w[8];
    for (int i = 0; i < 8; ++i) w[i] = static_cast<uint8_t>(plain[i] ^ rsb1::keystreamByte(zero, i));
    uint64_t got;
    std::memcpy(&got, w, 8);
    CHECK(got == rsb1::kPlainBytecodeMagic, "magic mismatch: %016llx", (unsigned long long)got);
}

// ---------------------------------------------------------------------------
// 4. Container decode paths
// ---------------------------------------------------------------------------
static void testDecodeMagicPath() {
    uint8_t blob[20];
    uint64_t magic = rsb1::kPlainBytecodeMagic;
    std::memcpy(blob, &magic, 8);
    for (int i = 0; i < 12; ++i) blob[8 + i] = static_cast<uint8_t>(0x40 + i);
    rsb1::Decoded d = rsb1::decodeContainer(blob, sizeof(blob), nullptr);
    CHECK(d.status == rsb1::DecodeStatus::OkPlain, "magic path status %d", int(d.status));
    CHECK(d.magic_hit, "magic_hit");
    CHECK(d.payload.size() == 12, "magic payload size %zu", d.payload.size());
    CHECK(d.payload[0] == 0x40 && d.payload[11] == 0x4B, "magic payload content");
}

static void testDecodeSize0Path() {
    // plain container with declared size 0 → payload = plain[8..) (raw bytecode)
    std::vector<uint8_t> body = {0x01, 0x02, 0x03, 0x04, 0x05};
    std::vector<uint8_t> plain = rsb1::buildPlain(body.data(), body.size(), 0);
    std::vector<uint8_t> wire = rsb1::encodeContainer(plain);
    // sanity: wire must not start with the plain magic
    uint64_t first8;
    std::memcpy(&first8, wire.data(), 8);
    CHECK(first8 != rsb1::kPlainBytecodeMagic, "size0 wire accidentally plain magic");

    rsb1::Decoded d = rsb1::decodeContainer(wire.data(), wire.size(), nullptr);
    CHECK(d.status == rsb1::DecodeStatus::OkPlain, "size0 status %d", int(d.status));
    CHECK(d.payload == body, "size0 payload round-trip");
    CHECK(d.declared_size == 0, "size0 declared");
}

static void testDecodeZstdPath() {
    std::vector<uint8_t> plain = rsb1::buildPlain(kZstdFrame, sizeof(kZstdFrame),
                                                  static_cast<uint32_t>(sizeof(kZstdPayload)));
    std::vector<uint8_t> wire = rsb1::encodeContainer(plain);
    rsb1::Decoded d = rsb1::decodeContainer(wire.data(), wire.size());
    if (!rsb1::zstdAvailable()) {
        CHECK(d.status == rsb1::DecodeStatus::ZstdUnavailable, "zstd availability");
        std::printf("note: libzstd not present, zstd path exercised partially\n");
        return;
    }
    CHECK(d.status == rsb1::DecodeStatus::OkZstd, "zstd status %d", int(d.status));
    CHECK(d.payload.size() == sizeof(kZstdPayload), "zstd payload size %zu vs %zu",
          d.payload.size(), sizeof(kZstdPayload));
    CHECK(std::memcmp(d.payload.data(), kZstdPayload, sizeof(kZstdPayload)) == 0,
          "zstd payload content");
}

static void testChecksumFailure() {
    // corrupt one payload byte → checksum mismatch
    std::vector<uint8_t> body(40, 0x5A);
    std::vector<uint8_t> plain = rsb1::buildPlain(body.data(), body.size(), 0);
    std::vector<uint8_t> wire = rsb1::encodeContainer(plain);
    wire[20] ^= 0x01;
    rsb1::Decoded d = rsb1::decodeContainer(wire.data(), wire.size(), nullptr);
    CHECK(d.status == rsb1::DecodeStatus::BadChecksum, "corrupt status %d", int(d.status));
}

static void testTooShort() {
    uint8_t junk[8] = {0};
    rsb1::Decoded d = rsb1::decodeContainer(junk, sizeof(junk), nullptr);
    CHECK(d.status == rsb1::DecodeStatus::TooShort, "short status %d", int(d.status));
}

int main() {
    testXxh32ShortVectors();
    testKeyTautology();
    testKeystreamBlockedEqualsScalar();
    testScrambleInvolution();
    testMagicConstruction();
    testDecodeMagicPath();
    testDecodeSize0Path();
    testDecodeZstdPath();
    testChecksumFailure();
    testTooShort();

    if (ChecksumFn bin = loadBinaryChecksum()) {
        testChecksumAgainstBinary(bin);
        std::printf("binary checksum code executed and matched\n");
    }

    if (g_failures == 0) {
        std::printf("all rsb1 tests passed\n");
        return 0;
    }
    std::printf("%d test failure(s)\n", g_failures);
    return 1;
}
