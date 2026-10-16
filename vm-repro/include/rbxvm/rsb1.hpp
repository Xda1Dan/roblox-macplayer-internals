// rsb1.hpp — reproduction of the RobloxPlayer (macOS) script-container pipeline:
// the "RSB1" scramble, the xxHash32 (XXH32) integrity checksum, and container
// decoding (FUN_1014b91b5 @ VA 0x1014b91b5, build 0.742.0.7421053).
//
// Wire format (what ships from the server / on disk), `W` = wire bytes, `N` = len:
//
//   Path A (plain bytecode blob): W[0..7] == 0xE009325B4A107A52 (LE)
//       → payload = W[8..N) passed to LuaVMLoad verbatim.
//       This magic is exactly the K=0 scramble of plaintext "RSB1"+0xFFFFFFFF,
//       i.e. a container header with no checksum and no compressed size —
//       used for pre-decoded bytecode (gated in-binary by DAT_10752bd48; both
//       sub-paths load the same bytes).
//
//   Path B (scrambled container): everything else.
//       key[i] = (W[i] ^ "RSB1"[i]) - i*0x29        (mod 256, i = 0..3)
//       plain[p] = W[p] ^ (uint8)(key[p & 3] + p*0x29)   for all p in [0,N)
//       verify: checksum32(plain, N, seed 0x2A) == key read as LE u32
//       plain[0..3] == "RSB1"
//       size   = u32 LE at plain[4..7]   (decompressed size)
//       size == 0                    → payload = plain[8..N)  (raw bytecode)
//       u32 LE at plain[8..11] == 0xFD2FB528 (zstd magic)
//                                → payload = zstd_decompress(plain[8..N), expect size)
//       otherwise → decode error.
//
// Provenance (all first-hand, Ghidra):
//   - key derivation: SIMD block at 0x1014b9392 (PXOR [0x106207660]="RSB1",
//     PADDB [0x106207670]=(0,-0x29,-0x52,-0x7B) = -i*0x29 mod 256)
//   - descrambler FUN_1014bae23: builds keystream ks[j] = key[j&3] + j*0x29
//     (j = 0..15), XORs 16-byte blocks, bumping every ks byte by +0x90 per
//     block (16*0x29 ≡ 0x90 mod 256) — algebraically identical to the scalar
//     keystream formula above; tail handled with the same formula.
//   - checksum FUN_103222533: plain standard xxHash32 (primes
//     0x9E3779B1/0x85EBCA77/0xC2B2AE3D/0x27D4EB2F/0x165667B1). Verified by
//     executing the function's own extracted machine code (318 bytes at VA
//     0x103222533) in tests/test_rsb1.cpp over a 1000+-case sweep.
//   - "checksum == key": the encoder sets the scramble key to the checksum of
//     the plaintext container, so the key is self-authenticating. (CMP EAX,
//     R14D at 0x1014b9413 compares the recomputed checksum against the key.)
//   - zstd: FUN_1032844e2 wraps create-dctx / decompress / free-dctx; the
//     frame magic 0xFD2FB528 is checked at 0x1014b9459 and the u32 at
//     plain[4..7] is used as the expected decompressed size (JZ → alloc that
//     many bytes; success iff the decompressor returns exactly that count).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rbxvm {
namespace rsb1 {

// "RSB1" as the LE u32 the binary writes/compares (0x31425352).
constexpr uint32_t kMagicRsb1 = 0x31425352u;

// W[0..7] == this (LE u64) → payload is plain bytecode starting at +8.
// Equals scramble(key=0, "RSB1" + 0xFFFFFFFF).
constexpr uint64_t kPlainBytecodeMagic = 0xE009325B4A107A52ull;

// zstd frame magic checked at plain[8..11].
constexpr uint32_t kZstdMagic = 0xFD2FB528u;

// Checksum seed used by the decoder call (MOV EDX, 0x2a).
constexpr uint32_t kChecksumSeed = 0x2Au;

// Keystream step constant.
constexpr uint8_t kKeystreamStep = 0x29;

struct Key {
    uint8_t b[4];
};

// key[i] = (W[i] ^ "RSB1"[i]) - i*0x29  (mod 256). Self-authenticating: with
// this key, descrambling W[0..3] reproduces "RSB1" exactly, for any W.
Key deriveKey(const uint8_t* wire);

// Keystream byte at absolute position p: (uint8)(key[p & 3] + p * 0x29).
inline uint8_t keystreamByte(const Key& k, size_t p) {
    return static_cast<uint8_t>(k.b[p & 3] + static_cast<uint8_t>(p * kKeystreamStep));
}

// In-place XOR with the keystream. Scramble and descramble are the same
// operation (XOR involution).
void scrambleInPlace(uint8_t* buf, size_t len, const Key& k);

// xxHash32 checksum with the format's seed (0x2A) — plain standard XXH32.
uint32_t checksum32(const uint8_t* data, size_t len, uint32_t seed);

enum class DecodeStatus {
    OkPlain,      // payload is raw Luau bytecode (magic path or size==0)
    OkZstd,       // payload decompressed from a zstd frame
    TooShort,     // len < 9
    BadChecksum,  // checksum32(plain) != key
    BadPayload,   // size != 0 but no zstd magic at plain[8..11]
    ZstdError,    // decompressor failed or size mismatch
    ZstdUnavailable, // no libzstd callable provided
};

struct Decoded {
    DecodeStatus status;
    std::vector<uint8_t> payload; // Luau bytecode (LBC blob)
    Key key{};                    // recovered scramble key (Path B)
    uint32_t declared_size = 0;   // u32 at plain[4..7] (decompressed size)
    uint64_t magic_hit = false;   // true if Path A (plain bytecode magic)
};

// Decompresses a zstd frame. Returns bytes written, or a negative value on
// error (mirrors zstd's ssize_t convention). Implemented via dlopen'd libzstd
// (see zstd_loader.hpp); may be null if libzstd is unavailable.
using ZstdDecompressFn = long (*)(void* dst, size_t dstCap, const void* src, size_t srcLen);

// Decode a wire container. `wire` is not modified (the binary descrambles the
// source string in place; we copy). Pass a decompress fn to enable the zstd
// path; without one, zstd containers return ZstdUnavailable.
Decoded decodeContainer(const uint8_t* wire, size_t len, ZstdDecompressFn zstd);

// Convenience: decode with the process-wide dlopen'd libzstd (if present).
Decoded decodeContainer(const uint8_t* wire, size_t len);

// Encoder (inverse of Path B) for round-trip testing and tooling:
//   plain = "RSB1" + u32(size) + body  — body must already be a zstd frame
//   when size != 0, or raw bytecode when size == 0.
// The key is the checksum of the plain bytes (that is the format's trick).
// Returns the scrambled wire blob.
std::vector<uint8_t> encodeContainer(const std::vector<uint8_t>& plain);

// Build a plaintext container body wrapper: "RSB1" + size + body.
std::vector<uint8_t> buildPlain(const uint8_t* body, size_t bodyLen, uint32_t declaredSize);

} // namespace rsb1
} // namespace rbxvm
