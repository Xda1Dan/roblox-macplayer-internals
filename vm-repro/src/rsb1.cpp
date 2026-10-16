// rsb1.cpp — implementation of the RSB1 container pipeline.
// Algorithm provenance and addresses: see include/rbxvm/rsb1.hpp.
#include "rbxvm/rsb1.hpp"

#include <cstring>

#include "rbxvm/zstd_loader.hpp"

namespace rbxvm {
namespace rsb1 {

Key deriveKey(const uint8_t* wire) {
    static const uint8_t kRsb1[4] = {'R', 'S', 'B', '1'}; // 0x52 0x53 0x42 0x31
    Key k;
    for (int i = 0; i < 4; ++i) {
        // (W[i] ^ "RSB1"[i]) - i*0x29, all mod 256. The binary computes the
        // equivalent SIMD form ((W ^ RSB1) + (0, -0x29, -0x52, -0x7B)).
        k.b[i] = static_cast<uint8_t>((wire[i] ^ kRsb1[i]) - i * kKeystreamStep);
    }
    return k;
}

void scrambleInPlace(uint8_t* buf, size_t len, const Key& k) {
    for (size_t p = 0; p < len; ++p) {
        buf[p] ^= keystreamByte(k, p);
    }
}

// ---------------------------------------------------------------------------
// checksum32 — standard xxHash32 (verified lane-for-lane against the
// binary's machine code, executed directly in tests/test_rsb1.cpp).
// ---------------------------------------------------------------------------
namespace {

inline uint32_t rotl32(uint32_t x, unsigned r) {
    return (x << r) | (x >> (32 - r));
}

inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v; // little-endian host assumed (x86-64), matching the binary
}

// xxHash32 primes, as they appear in the binary (decompiler negative forms noted)
constexpr uint32_t kP1 = 0x9E3779B1u; // -0x61c8864f
constexpr uint32_t kP2 = 0x85EBCA77u; // -0x7a143589
constexpr uint32_t kP3 = 0xC2B2AE3Du; // -0x3d4d51c3
constexpr uint32_t kP4 = 0x27D4EB2Fu;
constexpr uint32_t kP5 = 0x165667B1u;

} // namespace

uint32_t checksum32(const uint8_t* data, size_t len, uint32_t seed) {
    const uint8_t* p = data;
    const uint8_t* end = data + len;
    uint32_t h;

    if (len >= 16) {
        // standard xxHash32 lane inits (confirmed against the binary's
        // assembly: lea r9d,[rdx+0x24234428] = seed+P1+P2; lea r8d,[rdx-0x7a143589]
        // = seed+P2; w2-lane = seed; lea ecx,[rdx+0x61c8864f] = seed-P1,
        // because 0x61C8864F == -P1 mod 2^32):
        //   w0-lane: seed + P1 + P2      reads w0 (+0)
        //   w1-lane: seed + P2           reads w1 (+4)
        //   w2-lane: seed                reads w2 (+8)
        //   w3-lane: seed - P1           reads w3 (+12)
        uint32_t v0 = seed + kP1 + kP2;
        uint32_t v1 = seed + kP2;
        uint32_t v2 = seed;
        uint32_t v3 = seed - kP1;
        while (p + 16 <= end) {
            v0 = rotl32(v0 + rd32(p + 0) * kP2, 13) * kP1;
            v1 = rotl32(v1 + rd32(p + 4) * kP2, 13) * kP1;
            v2 = rotl32(v2 + rd32(p + 8) * kP2, 13) * kP1;
            v3 = rotl32(v3 + rd32(p + 12) * kP2, 13) * kP1;
            p += 16;
        }
        h = rotl32(v0, 1) + rotl32(v1, 7) + rotl32(v2, 12) + rotl32(v3, 18);
    } else {
        h = seed + kP5;
    }

    h += static_cast<uint32_t>(len);

    while (p + 4 <= end) {
        h = rotl32(h + rd32(p) * kP3, 17) * kP4;
        p += 4;
    }
    while (p < end) {
        h = rotl32(h + static_cast<uint32_t>(*p) * kP5, 11) * kP1;
        ++p;
    }

    h ^= h >> 15;
    h *= kP2;
    h ^= h >> 13;
    h *= kP3;
    h ^= h >> 16;
    return h;
}

// ---------------------------------------------------------------------------
// Container decode/encode
// ---------------------------------------------------------------------------
namespace {

inline uint64_t rd64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

inline uint32_t rd32le(const uint8_t* p) {
    return rd32(p);
}

inline uint32_t keyAsU32(const Key& k) {
    uint32_t v;
    std::memcpy(&v, k.b, 4);
    return v;
}

} // namespace

Decoded decodeContainer(const uint8_t* wire, size_t len, ZstdDecompressFn zstd) {
    Decoded out;

    if (len < 9) {
        out.status = DecodeStatus::TooShort;
        return out;
    }

    // Path A: plain-bytecode magic (the K=0 scramble of "RSB1"+0xFFFFFFFF).
    if (rd64(wire) == kPlainBytecodeMagic) {
        out.status = DecodeStatus::OkPlain;
        out.magic_hit = true;
        out.declared_size = 0xFFFFFFFFu; // the magic encodes this size field
        out.payload.assign(wire + 8, wire + len);
        return out;
    }

    // Path B: descramble a copy, verify checksum == key, then either plain
    // bytecode (declared size 0) or a zstd frame.
    std::vector<uint8_t> plain(wire, wire + len);
    Key key = deriveKey(plain.data());
    scrambleInPlace(plain.data(), plain.size(), key);
    out.key = key;

    uint32_t sum = checksum32(plain.data(), plain.size(), kChecksumSeed);
    if (sum != keyAsU32(key)) {
        out.status = DecodeStatus::BadChecksum;
        return out;
    }

    if (rd32le(plain.data()) != kMagicRsb1) {
        // Cannot happen if the key derivation is correct (tautology), but the
        // binary effectively relies on it, so assert the invariant here.
        out.status = DecodeStatus::BadChecksum;
        return out;
    }

    uint32_t size = rd32le(plain.data() + 4);
    out.declared_size = size;

    if (size == 0) {
        out.status = DecodeStatus::OkPlain;
        out.payload.assign(plain.begin() + 8, plain.end());
        return out;
    }

    // The binary gates the zstd path at len >= 13 (header 8 + frame >= 5).
    if (len < 13 || rd32le(plain.data() + 8) != kZstdMagic) {
        out.status = DecodeStatus::BadPayload;
        return out;
    }

    if (!zstd) {
        out.status = DecodeStatus::ZstdUnavailable;
        return out;
    }

    std::vector<uint8_t> dst(size);
    long written = zstd(dst.data(), size, plain.data() + 8, plain.size() - 8);
    if (written < 0 || static_cast<uint64_t>(written) != size) {
        out.status = DecodeStatus::ZstdError;
        return out;
    }

    out.status = DecodeStatus::OkZstd;
    out.payload = std::move(dst);
    return out;
}

std::vector<uint8_t> buildPlain(const uint8_t* body, size_t bodyLen, uint32_t declaredSize) {
    std::vector<uint8_t> plain;
    plain.reserve(bodyLen + 8);
    const uint8_t magic[4] = {'R', 'S', 'B', '1'};
    plain.insert(plain.end(), magic, magic + 4);
    for (int i = 0; i < 4; ++i) plain.push_back(static_cast<uint8_t>(declaredSize >> (8 * i)));
    plain.insert(plain.end(), body, body + bodyLen);
    return plain;
}

std::vector<uint8_t> encodeContainer(const std::vector<uint8_t>& plain) {
    uint32_t key = checksum32(plain.data(), plain.size(), kChecksumSeed);
    Key k;
    std::memcpy(k.b, &key, 4);
    std::vector<uint8_t> wire(plain);
    scrambleInPlace(wire.data(), wire.size(), k);
    return wire;
}

Decoded decodeContainer(const uint8_t* wire, size_t len) {
    return decodeContainer(wire, len, zstdAvailable() ? &zstdDecompress : nullptr);
}

} // namespace rsb1
} // namespace rbxvm
