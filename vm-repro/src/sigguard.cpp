// sigguard.cpp — transcription of the SigGuard bytecode-auth gate.
// See include/rbxvm/sigguard.hpp for provenance and the gate's return
// convention.
#include "rbxvm/sigguard.hpp"

#include <algorithm>
#include <cstring>

namespace rbxvm {
namespace sigguard {

namespace {

// ---------------------------------------------------------------------------
// SHA-256 (standard; the binary's FUN_102f8fa5c/8fc08/9109d are OpenSSL
// SHA-256 and the gate requests a 32-byte digest).
// ---------------------------------------------------------------------------
struct Sha256 {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t used;
};

inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

void shaInit(Sha256& s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(s.h, iv, sizeof iv);
    s.total = 0;
    s.used = 0;
}

void shaBlock(Sha256& s, const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
               (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3];
    uint32_t e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d;
    s.h[4] += e; s.h[5] += f; s.h[6] += g; s.h[7] += h;
}

void shaUpdate(Sha256& s, const uint8_t* p, size_t n) {
    s.total += n;
    if (s.used) {
        size_t take = std::min(n, size_t(64) - s.used);
        std::memcpy(s.buf + s.used, p, take);
        s.used += take; p += take; n -= take;
        if (s.used == 64) { shaBlock(s, s.buf); s.used = 0; }
    }
    while (n >= 64) { shaBlock(s, p); p += 64; n -= 64; }
    if (n) { std::memcpy(s.buf, p, n); s.used = n; }
}

void shaFinal(Sha256& s, uint8_t out[32]) {
    const uint64_t bits = s.total * 8;
    uint8_t pad[72];
    size_t padlen = (s.used < 56) ? (56 - s.used) : (120 - s.used);
    pad[0] = 0x80;
    std::memset(pad + 1, 0, padlen - 1);
    for (int i = 0; i < 8; ++i) pad[padlen + i] = uint8_t(bits >> (56 - 8 * i));
    shaUpdate(s, pad, padlen + 8);
    for (int i = 0; i < 8; ++i) {
        out[4 * i + 0] = uint8_t(s.h[i] >> 24);
        out[4 * i + 1] = uint8_t(s.h[i] >> 16);
        out[4 * i + 2] = uint8_t(s.h[i] >> 8);
        out[4 * i + 3] = uint8_t(s.h[i]);
    }
}

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

inline void xorRecord(Record& dst, const Record& src) {
    for (int i = 0; i < 16; ++i) dst[i] ^= src[i];
}

} // namespace

std::string sha256Hex(const uint8_t* data, size_t len) {
    Sha256 s;
    shaInit(s);
    shaUpdate(s, data, len);
    uint8_t out[32];
    shaFinal(s, out);
    static const char* hex = "0123456789abcdef";
    std::string r(64, '0');
    for (int i = 0; i < 32; ++i) {
        r[2 * i] = hex[out[i] >> 4];
        r[2 * i + 1] = hex[out[i] & 15];
    }
    return r;
}

// ---------------------------------------------------------------------------
// Packed table loading + expansion (FUN_1014bd3e4 / bd60c / bd6aa / bd73a /
// bd81c).
// ---------------------------------------------------------------------------
bool loadPackedTables(Tables& out, const uint8_t* blob, size_t len) {
    if (!blob || len < kPackedTableBytes) return false;
    if (std::memcmp(blob, kPackedMagic, sizeof kPackedMagic) != 0) return false;
    for (size_t i = 0; i < kRecordCount; ++i) {
        std::memcpy(out.t1[i].data(), blob + i * 12, 12);
        std::memset(out.t1[i].data() + 12, 0, 4);
    }
    out.a.clear();
    out.b.clear();
    out.c.clear();
    out.expanded = false;
    return true;
}

void expandTables(Tables& tables) {
    if (tables.expanded) return;

    // Pair-index table built by FUN_1014bd49c: 128 entries of
    // {offset, count}; the offset walks the 8257 T1 records.
    std::array<std::pair<uint64_t, uint64_t>, 128> pairs{};
    {
        uint64_t a = 0x81, b = 0x7f;
        for (size_t e = 0; e < pairs.size(); ++e) {
            pairs[e] = {a, b};
            a += b;
            if (b) --b;
        }
    }

    auto t1At = [&](uint64_t idx) -> const Record& {
        static const Record zero{};
        return idx < tables.t1.size() ? tables.t1[size_t(idx)] : zero;
    };

    // Table A (FUN_1014bd6aa): linear rows. Row[i*256+n] is the XOR of the
    // eight basis records for the set bits of n (bit 7-j -> basis j).
    tables.a.assign(0x1000, Record{});
    for (size_t i = 0; i < 16; ++i) {
        for (size_t n = 0; n < 256; ++n) {
            Record row{};
            if (n) {
                for (size_t j = 0; j < 8; ++j) {
                    if (int8_t(uint8_t(n << j)) < 0)
                        xorRecord(row, t1At(1 + i * 8 + j));
                }
            }
            tables.a[i * 256 + n] = row;
        }
    }

    // Table B (FUN_1014bd73a): pairwise cross terms within one selector
    // position, indexed through the triangular pair table.
    tables.b.assign(0x1000, Record{});
    for (size_t i = 0; i < 16; ++i) {
        for (size_t n = 0; n < 256; ++n) {
            Record row{};
            if (n) {
                for (size_t j = 0; j < 7; ++j) {
                    if (!(int8_t(uint8_t(n << j)) < 0)) continue;
                    for (size_t k = j + 1; k < 8; ++k) {
                        if (int8_t(uint8_t(n << k)) < 0)
                            xorRecord(row, t1At(pairs[i * 8 + j].first + (k - j - 1)));
                    }
                }
            }
            tables.b[i * 256 + n] = row;
        }
    }

    // Table C (FUN_1014bd81c): cross terms between two selector positions.
    // Block index for (i,j) is
    //   ((~j + i + j*16) - ((j+1)*j>>1)) * 0x800
    // and the row for (bit, n) is block + bit*0x100 + n.
    tables.c.assign(0x3c000, Record{});
    for (size_t i = 1; i < 16; ++i) {
        for (size_t j = 0; j < i; ++j) {
            const size_t block =
                ((~j + i + j * 16) - ((j + 1) * j >> 1)) * 0x800;
            const int64_t base = int64_t(8 * i) - 1 - int64_t(8 * j);
            for (size_t bit = 0; bit < 8; ++bit) {
                for (size_t n = 0; n < 256; ++n) {
                    Record row{};
                    if (n) {
                        for (size_t b = 0; b < 8; ++b) {
                            if (!(int8_t(uint8_t(n << b)) < 0)) continue;
                            const uint64_t idx =
                                pairs[j * 8 + b].first +
                                uint64_t(int64_t(base) + int64_t(bit) - int64_t(b));
                            xorRecord(row, t1At(idx));
                        }
                    }
                    tables.c[block + bit * 0x100 + n] = row;
                }
            }
        }
    }

    tables.expanded = true;
}

std::array<uint8_t, 12> computeMac(const Tables& tables, const uint8_t selector[16]) {
    std::array<uint8_t, 12> state{};
    std::memcpy(state.data(), tables.t1[0].data(), 12);

    auto xor12 = [&](const Record& r) {
        for (int i = 0; i < 12; ++i) state[i] ^= r[i];
    };

    for (size_t i = 0; i < 16; ++i) {
        const size_t v = selector[i];
        if (!v) continue;
        xor12(tables.a[i * 256 + v]);
        xor12(tables.b[i * 256 + v]);
    }

    for (size_t i = 1; i < 16; ++i) {
        if (!selector[i]) continue;
        for (size_t bit = 0; bit < 8; ++bit) {
            if (!(int8_t(uint8_t(selector[i] << bit)) < 0)) continue;
            for (size_t j = 0; j < i; ++j) {
                if (!selector[j]) continue;
                const size_t block =
                    ((~j + i + j * 16) - ((j + 1) * j >> 1)) * 0x800;
                const size_t idx = block + bit * 0x100 + selector[j];
                xor12(tables.c[idx]);
            }
        }
    }
    return state;
}

bool codeRejects(int code) { return code >= 3 && code <= 9; }

bool codeClearsBit30(int code) {
    // FUN_1014bb465's first opaque condition. Codes 3,4,6,7,9 set it;
    // codes 0,1,2,5,8 do not.
    switch (code) {
    case 3: case 4: case 6: case 7: case 9: return true;
    default: return false;
    }
}

namespace {

int versionMagic(const uint8_t* data, size_t len) {
    if (len <= 0x17 || data[0] == 0) return -1;
    const uint8_t b0 = data[len - 0x18] ^ data[len - 4];
    const uint8_t b1 = data[len - 0x17] ^ data[len - 3];
    const uint8_t b2 = data[len - 0x16] ^ data[len - 2];
    if (b2 != 0x6A || b0 != 0x32 || b1 != 0xC4) return -1;
    return data[len - 0x15] ^ data[len - 1];
}

bool secondaryMagicPresent(const uint8_t* data, size_t len) {
    if (len <= 0x27 || data[0] == 0) return false;
    return (rd32(data + len - 0x20) ^ rd32(data + len - 0x28)) == 0x946AC432u;
}

} // namespace

Result verify(const Tables& tables, const uint8_t* data, size_t len, const Config& cfg) {
    Result out;

    // FUN_1014bb64e's trivial pass: tiny/null/empty blobs yield code 1,
    // which the gate maps to "accept" (wrapper proceeds).
    if (len <= 0x117 || data == nullptr || data[0] == 0) {
        out.code = 1;
        out.reject = codeRejects(out.code);
        out.clears_bit30 = codeClearsBit30(out.code);
        return out;
    }

    // Mode chain (FUN_1014bc1a8).
    int mode;
    const int ver = versionMagic(data, len);
    if (secondaryMagicPresent(data, len)) {
        mode = (ver < 0) ? 0 : ver;
    } else {
        mode = ver;
    }
    if (mode == 0 && cfg.force_signed > 0) mode = -1;
    out.mode = mode < 0 ? Mode::ForcedReject
                        : (mode == 0 ? Mode::V0 : (mode == 1 ? Mode::V1 : Mode::Other));

    if (mode < 0) {
        out.code = 3;
    } else if (mode > cfg.max_mode) {
        out.code = 4;
    } else if (mode >= 2) {
        out.code = 5;
    } else {
        const size_t trailerLen = (mode == 0) ? 0x28 : 0x18;
        const size_t trailerOff = len - trailerLen;
        out.trailer_offset = trailerOff;
        out.payload_size = trailerOff;
        const uint8_t* trailer = data + trailerOff;
        out.checkword = rd32(trailer) ^ rd32(trailer + ((mode == 0) ? 0x10 : 4));

        const int threshold = (cfg.threshold_flag >= 2) ? 2 : 1;
        if (mode < threshold) {
            out.code = 6; // hex(trailer) simple path; always rejects mode 0
        } else {
            size_t minPayload = size_t(std::min(0x400, std::max(0, cfg.min_payload)));
            minPayload = std::max<size_t>(0x100, minPayload);
            if (out.payload_size < minPayload) {
                out.code = 1;
            } else if (mode == 1) {
                if (out.checkword != 0x74171870u) {
                    out.code = 9;
                } else if (cfg.verify_mac) {
                    const std::string digest = sha256Hex(data, out.payload_size);
                    const std::array<uint8_t, 12> mac =
                        computeMac(tables, trailer);
                    out.code = (std::memcmp(mac.data(), digest.data() + 4, 12) == 0) ? 0 : 7;
                } else {
                    out.code = 0; // DAT_10752be50 == 0: MAC check disabled
                }
            } else {
                out.code = 6; // mode 0 full path is unreachable with threshold>=1
            }
        }
    }

    out.reject = codeRejects(out.code);
    out.clears_bit30 = codeClearsBit30(out.code);
    return out;
}

} // namespace sigguard
} // namespace rbxvm
