// aead.cpp — ChaCha20-Poly1305 with the RobloxPlayer RakNet framing.
// Construction provenance: include/rbxvm/aead.hpp and ../research/vm-research/networking.md.
#include "rbxvm/aead.hpp"

#include <cstring>

namespace rbxvm {
namespace aead {

namespace {

inline uint32_t rd32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline void wr32le(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}

inline uint32_t rotl32(uint32_t x, unsigned r) { return (x << r) | (x >> (32 - r)); }

inline void qr(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b; d ^= a; d = rotl32(d, 16);
    c += d; b ^= c; b = rotl32(b, 12);
    a += b; d ^= a; d = rotl32(d, 8);
    c += d; b ^= c; b = rotl32(b, 7);
}

// 20-round ChaCha20 block. words[12..15] supplied by the caller.
void chachaBlock(uint8_t out[64], const uint32_t tail[4], const uint8_t key[32]) {
    uint32_t st[16];
    st[0] = 0x61707865u;  // "expa"
    st[1] = 0x3320646eu;  // "nd 3"
    st[2] = 0x79622d32u;  // "2-by"
    st[3] = 0x6b206574u;  // "te k"
    st[4] = rd32le(key + 0);  st[5] = rd32le(key + 4);
    st[6] = rd32le(key + 8);  st[7] = rd32le(key + 12);
    st[8] = rd32le(key + 16); st[9] = rd32le(key + 20);
    st[10] = rd32le(key + 24); st[11] = rd32le(key + 28);
    st[12] = tail[0]; st[13] = tail[1]; st[14] = tail[2]; st[15] = tail[3];

    uint32_t x[16];
    for (int i = 0; i < 16; ++i) x[i] = st[i];
    for (int i = 0; i < 10; ++i) {
        qr(x[0], x[4], x[8], x[12]);
        qr(x[1], x[5], x[9], x[13]);
        qr(x[2], x[6], x[10], x[14]);
        qr(x[3], x[7], x[11], x[15]);
        qr(x[0], x[5], x[10], x[15]);
        qr(x[1], x[6], x[11], x[12]);
        qr(x[2], x[7], x[8], x[13]);
        qr(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) wr32le(out + 4 * i, x[i] + st[i]);
}

// ---------------------------------------------------------------------------
// poly1305-donna-32 (semantics of FUN_101835319 / FUN_10183539d / FUN_10183546a)
// ---------------------------------------------------------------------------
struct PolyCtx {
    uint32_t r[5], h[5], s[4];
    uint8_t buf[16];
    size_t leftover;
    bool final;
};

void polyInit(PolyCtx& st, const uint8_t key[32]) {
    st.r[0] = rd32le(key + 0) & 0x3ffffff;
    st.r[1] = (rd32le(key + 3) >> 2) & 0x3ffff03;
    st.r[2] = (rd32le(key + 6) >> 4) & 0x3ffc0ff;
    st.r[3] = (rd32le(key + 9) >> 6) & 0x3f03fff;
    st.r[4] = (rd32le(key + 12) >> 8) & 0xfffff;
    st.h[0] = st.h[1] = st.h[2] = st.h[3] = st.h[4] = 0;
    st.s[0] = rd32le(key + 16); st.s[1] = rd32le(key + 20);
    st.s[2] = rd32le(key + 24); st.s[3] = rd32le(key + 28);
    st.leftover = 0;
    st.final = false;
}

void polyBlocks(PolyCtx& st, const uint8_t* m, size_t bytes) {
    const uint32_t hibit = st.final ? 0 : (1u << 24); // the 2^128 bit, full blocks only
    uint32_t r0 = st.r[0], r1 = st.r[1], r2 = st.r[2], r3 = st.r[3], r4 = st.r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = st.h[0], h1 = st.h[1], h2 = st.h[2], h3 = st.h[3], h4 = st.h[4];

    while (bytes >= 16) {
        h0 += rd32le(m + 0) & 0x3ffffff;
        h1 += (rd32le(m + 3) >> 2) & 0x3ffffff;
        h2 += (rd32le(m + 6) >> 4) & 0x3ffffff;
        h3 += (rd32le(m + 9) >> 6);
        h4 += (rd32le(m + 12) >> 8) | hibit;

        uint64_t d0 = uint64_t(h0) * r0 + uint64_t(h1) * s4 + uint64_t(h2) * s3 +
                      uint64_t(h3) * s2 + uint64_t(h4) * s1;
        uint64_t d1 = uint64_t(h0) * r1 + uint64_t(h1) * r0 + uint64_t(h2) * s4 +
                      uint64_t(h3) * s3 + uint64_t(h4) * s2;
        uint64_t d2 = uint64_t(h0) * r2 + uint64_t(h1) * r1 + uint64_t(h2) * r0 +
                      uint64_t(h3) * s4 + uint64_t(h4) * s3;
        uint64_t d3 = uint64_t(h0) * r3 + uint64_t(h1) * r2 + uint64_t(h2) * r1 +
                      uint64_t(h3) * r0 + uint64_t(h4) * s4;
        uint64_t d4 = uint64_t(h0) * r4 + uint64_t(h1) * r3 + uint64_t(h2) * r2 +
                      uint64_t(h3) * r1 + uint64_t(h4) * r0;

        h0 = uint32_t(d0) & 0x3ffffff;
        uint64_t c = d0 >> 26; d1 += c; h1 = uint32_t(d1) & 0x3ffffff; c = d1 >> 26;
        d2 += c; h2 = uint32_t(d2) & 0x3ffffff; c = d2 >> 26;
        d3 += c; h3 = uint32_t(d3) & 0x3ffffff; c = d3 >> 26;
        d4 += c; h4 = uint32_t(d4) & 0x3ffffff; c = d4 >> 26;
        h0 += uint32_t(c) * 5; h1 += (h0 >> 26); h0 &= 0x3ffffff;

        m += 16;
        bytes -= 16;
    }
    st.h[0] = h0; st.h[1] = h1; st.h[2] = h2; st.h[3] = h3; st.h[4] = h4;
}

void polyUpdate(PolyCtx& st, const uint8_t* p, size_t n) {
    if (st.leftover) {
        size_t want = 16 - st.leftover;
        if (want > n) want = n;
        std::memcpy(st.buf + st.leftover, p, want);
        st.leftover += want; p += want; n -= want;
        if (st.leftover == 16) { polyBlocks(st, st.buf, 16); st.leftover = 0; }
    }
    if (n >= 16) {
        size_t full = n & ~size_t(15);
        polyBlocks(st, p, full);
        p += full; n -= full;
    }
    if (n) {
        std::memcpy(st.buf + st.leftover, p, n);
        st.leftover += n;
    }
}

void polyFinal(PolyCtx& st, uint8_t tag[16]) {
    if (st.leftover) {
        st.buf[st.leftover] = 1;
        for (size_t i = st.leftover + 1; i < 16; ++i) st.buf[i] = 0;
        st.final = true;
        polyBlocks(st, st.buf, 16);
    }

    uint32_t h0 = st.h[0], h1 = st.h[1], h2 = st.h[2], h3 = st.h[3], h4 = st.h[4];
    uint32_t c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    // h + -p, selected branchlessly (donna-32)
    uint64_t g0 = uint64_t(h0) + 5; c = uint32_t(g0 >> 26); g0 &= 0x3ffffff;
    uint64_t g1 = uint64_t(h1) + c; c = uint32_t(g1 >> 26); g1 &= 0x3ffffff;
    uint64_t g2 = uint64_t(h2) + c; c = uint32_t(g2 >> 26); g2 &= 0x3ffffff;
    uint64_t g3 = uint64_t(h3) + c; c = uint32_t(g3 >> 26); g3 &= 0x3ffffff;
    uint64_t g4 = uint64_t(h4) + c - (1ull << 26);
    // g4's top bit (63) is set iff h < p (underflow) -> keep h; else use g
    const uint32_t gmask = uint32_t((g4 >> 63) - 1); // 0 -> keep h, all-ones -> use g
    g0 &= gmask; g1 &= gmask; g2 &= gmask; g3 &= gmask; g4 &= gmask;
    const uint32_t hmask = ~gmask;
    h0 = (h0 & hmask) | uint32_t(g0);
    h1 = (h1 & hmask) | uint32_t(g1);
    h2 = (h2 & hmask) | uint32_t(g2);
    h3 = (h3 & hmask) | uint32_t(g3);
    h4 = (h4 & hmask) | uint32_t(g4);

    // repack the 26-bit limbs into 32-bit words (FUN_10183546a's shifts:
    // 0/26, 6/20, 12/14, 18/8) and add the s key word-wise with carries.
    // (h + s) mod 2^128 — the final carry out of word3 is discarded.
    uint64_t f = (uint64_t)h0 | ((uint64_t)(h1 & 0x3f) << 26);
    f += st.s[0];
    const uint32_t w0 = uint32_t(f);
    uint64_t carry = f >> 32;

    f = ((h1 >> 6) | ((uint64_t)(h2 & 0xfff) << 20)) + st.s[1] + carry;
    const uint32_t w1 = uint32_t(f);
    carry = f >> 32;

    f = ((h2 >> 12) | ((uint64_t)(h3 & 0x3ffff) << 14)) + st.s[2] + carry;
    const uint32_t w2 = uint32_t(f);
    carry = f >> 32;

    f = ((h3 >> 18) | ((uint64_t)h4 << 8)) + st.s[3] + carry;
    const uint32_t w3 = uint32_t(f);

    wr32le(tag + 0, w0); wr32le(tag + 4, w1); wr32le(tag + 8, w2); wr32le(tag + 12, w3);
}

void polyKey(uint8_t pk[32], const uint8_t nonce12[12], const uint8_t key[32]) {
    // Binary FUN_10183bed2: state[12..13] = 0 (64-bit counter 0),
    // state[14..15] = nonce[0..8]. Only the first 8 nonce bytes are read.
    uint32_t w[4] = {0, 0, rd32le(nonce12), rd32le(nonce12 + 4)};
    uint8_t blk[64];
    chachaBlock(blk, w, key);
    std::memcpy(pk, blk, 32);
}

// tag = Poly1305(poly_key, aad||pad16||ct||pad16||len_aad(u64le)||len_ct(u64le))
void mac(uint8_t tag[16], const uint8_t* aad, size_t aadlen, const uint8_t* ct, size_t ctlen,
         const uint8_t pk[32]) {
    uint8_t lenblk[16];
    wr32le(lenblk, uint32_t(aadlen)); wr32le(lenblk + 4, uint32_t(aadlen >> 32));
    wr32le(lenblk + 8, uint32_t(ctlen)); wr32le(lenblk + 12, uint32_t(ctlen >> 32));

    static const uint8_t zeros[16] = {0};
    PolyCtx st;
    polyInit(st, pk);
    if (aad && aadlen) polyUpdate(st, aad, aadlen);
    polyUpdate(st, zeros, (16 - (aadlen & 15)) & 15);
    if (ct && ctlen) polyUpdate(st, ct, ctlen);
    polyUpdate(st, zeros, (16 - (ctlen & 15)) & 15);
    polyUpdate(st, lenblk, 16);
    polyFinal(st, tag);
}

} // namespace

void chacha20Block(uint8_t out[64], const uint8_t key[32], uint32_t counter,
                   const uint8_t nonce[12]) {
    uint32_t w[4] = {counter, rd32le(nonce), rd32le(nonce + 4), rd32le(nonce + 8)};
    chachaBlock(out, w, key);
}

void makeRecvNonce(uint8_t out[12], uint64_t seq) {
    for (int i = 0; i < 8; ++i) out[i] = uint8_t(seq >> (8 * i));
    wr32le(out + 8, kRecvNonceSuffix);
}

void makeSendNonce(uint8_t out[12], uint64_t seq) {
    const uint64_t v = seq + kSendNonceAdd;
    for (int i = 0; i < 8; ++i) out[i] = uint8_t(v >> (8 * i));
    wr32le(out + 8, kSendNonceSuffix);
}

void keystream(uint8_t* dst, const uint8_t* src, size_t len, const uint8_t key[32],
               uint64_t counterStart, const uint8_t nonce12[12]) {
    // Payload state per the binary: word12 = counter lo, word13 = counter hi,
    // words14-15 = the FIRST 8 bytes of the nonce only.
    const uint32_t n0 = rd32le(nonce12), n1 = rd32le(nonce12 + 4);
    uint64_t counter = counterStart;
    size_t off = 0;
    uint8_t block[64];
    while (off < len) {
        uint32_t w[4] = {uint32_t(counter), uint32_t(counter >> 32), n0, n1};
        chachaBlock(block, w, key);
        size_t n = len - off < 64 ? len - off : 64;
        for (size_t i = 0; i < n; ++i) dst[off + i] = src[off + i] ^ block[i];
        off += n;
        ++counter;
    }
}

void poly1305(uint8_t tag[16], const uint8_t key[32], const uint8_t* msg, size_t len) {
    PolyCtx st;
    polyInit(st, key);
    polyUpdate(st, msg, len);
    polyFinal(st, tag);
}

void aeadEncrypt(uint8_t* dst, uint8_t tag[16], const uint8_t* pt, size_t ptlen,
                 const uint8_t* aad, size_t aadlen, const uint8_t nonce12[12],
                 const uint8_t key[32]) {
    uint8_t pk[32];
    polyKey(pk, nonce12, key);
    keystream(dst, pt, ptlen, key, 1, nonce12);
    mac(tag, aad, aadlen, dst, ptlen, pk);
}

bool aeadDecrypt(uint8_t* dst, const uint8_t* ct, size_t ctlen, const uint8_t tag[16],
                 const uint8_t* aad, size_t aadlen, const uint8_t nonce12[12],
                 const uint8_t key[32]) {
    uint8_t pk[32];
    polyKey(pk, nonce12, key);
    uint8_t computed[16];
    mac(computed, aad, aadlen, ct, ctlen, pk);
    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= computed[i] ^ tag[i];
    if (diff != 0) return false;
    keystream(dst, ct, ctlen, key, 1, nonce12);
    return true;
}

uint64_t reconstructSequence(uint64_t& local, uint16_t echo) {
    const uint64_t cand = (local & ~0xFFFFull) | echo;
    uint64_t candidate = cand;
    if (cand < local) {
        const uint64_t back = local - cand;
        if (back >= 0x8001) {
            candidate = cand + 0x10000;
            local = candidate;
        }
        // else: an old-but-in-window packet; counter unchanged
    } else {
        const uint64_t fwd = cand - local;
        if (fwd < 0x8001) {
            local = cand; // commit
        } else {
            candidate = cand - 0x10000; // previous cycle; counter unchanged
        }
    }
    return candidate;
}

} // namespace aead
} // namespace rbxvm
