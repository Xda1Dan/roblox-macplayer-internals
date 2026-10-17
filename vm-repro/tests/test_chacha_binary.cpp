// test_chacha_binary.cpp — hardening test: execute the *binary's own*
// ChaCha20 block core (FUN_10183c095 @ 0x10183c095, 875 bytes) and require it
// to agree with our aead::keystream over a sweep.
//
// Provenance (extractor over the target Mach-O, committed fixture):
//   code entry  : type 0, va 0x10183c095, 875 bytes (0x10183c095..c400)
//   constants   : type 1, four 16-byte shuffle masks (rip-relative targets)
//                 0x106251940 / 50 / 60 / cb0
//   patch table : the six rip-relative references in the code (4 const loads,
//                 2 stack-guard pointer loads), so the extraction can run
//                 from a scratch arena without MAP_FIXED tricks.
// Fixture v2 sha256:
//   a48a4e1bfcac59ccf2def15fb96f0d0548310b88f816ee31111a5b31ca8b2dff
//
// The caller FUN_10183bfc7 passes (state, bufA, bufB, len) and the state is
// "expand 32-byte k" | key | counter64 | nonce[0..8]; the core advances the
// 64-bit counter by ceil(len/64). The sweep below settles which of bufA/bufB
// is written (the binary answers; we compare both directions against our
// keystream).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "rbxvm/aead.hpp"

using namespace rbxvm;

static int g_failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            ++g_failures;                                    \
            std::printf("FAIL: %s:%d: ", __FILE__, __LINE__);\
            std::printf(__VA_ARGS__);                        \
        }                                                    \
    } while (0)

namespace {

struct Patch {
    uint32_t off;
    uint8_t len;
    uint64_t targetVa;
};

struct Fixture {
    std::vector<uint8_t> code;
    std::vector<std::vector<uint8_t>> consts; // rip-relative targets, in order
    std::vector<Patch> patches;

    bool load(const char* path) {
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        std::vector<uint8_t> blob;
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            blob.insert(blob.end(), buf, buf + n);
        std::fclose(f);
        if (blob.size() < 8) return false;
        uint32_t magic = 0;
        uint16_t version = 0, count = 0;
        std::memcpy(&magic, blob.data() + 0, 4);
        std::memcpy(&version, blob.data() + 4, 2);
        std::memcpy(&count, blob.data() + 6, 2);
        if (magic != 0x46434843u || version != 2) return false;
        size_t off = 8;
        for (uint32_t i = 0; i < count; ++i) {
            if (off + 13 > blob.size()) return false;
            const uint8_t type = blob[off];
            uint64_t va;
            uint32_t len;
            std::memcpy(&va, blob.data() + off + 1, 8);
            std::memcpy(&len, blob.data() + off + 9, 4);
            off += 13;
            if (off + len > blob.size()) return false;
            if (type == 0) code.assign(blob.begin() + off, blob.begin() + off + len);
            else consts.push_back(std::vector<uint8_t>(blob.begin() + off, blob.begin() + off + len));
            off += len;
        }
        if (off + 4 > blob.size()) return false;
        uint32_t patchCount = 0;
        std::memcpy(&patchCount, blob.data() + off, 4);
        off += 4;
        for (uint32_t i = 0; i < patchCount; ++i) {
            if (off + 13 > blob.size()) return false;
            Patch p;
            std::memcpy(&p.off, blob.data() + off, 4);
            p.len = blob[off + 4];
            std::memcpy(&p.targetVa, blob.data() + off + 5, 8);
            off += 13;
            patches.push_back(p);
        }
        return !code.empty();
    }
};

using CoreFn = void (*)(uint8_t* state, void* a, void* b, size_t len);

} // namespace

int main() {
    setbuf(stdout, NULL);

    Fixture fx;
    if (!fx.load("tests/fixtures/chacha_core.bin")) {
        std::printf("FAIL: cannot load tests/fixtures/chacha_core.bin\n");
        return 1;
    }
    CHECK(fx.code.size() == 875, "code length %zu", fx.code.size());
    CHECK(fx.consts.size() == 4, "const count %zu", fx.consts.size());
    CHECK(fx.patches.size() == 6, "patch count %zu", fx.patches.size());

    // ---- arena: [ code 875 | guardptr 8 | 4 consts ] ----------------------
    const size_t codeOff = 0;
    const size_t guardOff = 896;                 // 16-aligned
    const size_t constBase = guardOff + 16;      // keep every const 16-aligned
    std::vector<size_t> constOff(fx.consts.size());
    for (size_t i = 0; i < fx.consts.size(); ++i)
        constOff[i] = constBase + 16 * i;        // consts are 16 bytes each
    const size_t arenaLen = constBase + 16 * fx.consts.size();
    void* arena = mmap(nullptr, arenaLen, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) {
        std::printf("FAIL: arena mmap\n");
        return 1;
    }
    std::memcpy(static_cast<uint8_t*>(arena) + codeOff, fx.code.data(), fx.code.size());
    for (size_t i = 0; i < fx.consts.size(); ++i)
        std::memcpy(static_cast<uint8_t*>(arena) + constOff[i], fx.consts[i].data(),
                    fx.consts[i].size());

    // guard slot: holds a pointer to a stable scratch qword
    static uint64_t canary = 0;
    const uint64_t canaryPtr = reinterpret_cast<uint64_t>(&canary);
    std::memcpy(static_cast<uint8_t*>(arena) + guardOff, &canaryPtr, sizeof(canaryPtr));

    // ---- patch the rip-relative displacements ------------------------------
    const uint64_t codeVa = reinterpret_cast<uint64_t>(arena) + codeOff;
    for (const Patch& p : fx.patches) {
        // resolve target: the guard (two patches) or one of the four consts
        uint64_t newVa = 0;
        if (p.targetVa == 0x106e65040ull)
            newVa = reinterpret_cast<uint64_t>(arena) + guardOff;
        else {
            static const uint64_t kConstVas[4] = {0x106251940ull, 0x106251950ull,
                                                  0x106251960ull, 0x106251cb0ull};
            for (size_t i = 0; i < 4; ++i) {
                if (p.targetVa == kConstVas[i]) {
                    newVa = reinterpret_cast<uint64_t>(arena) + constOff[i];
                    break;
                }
            }
        }
        CHECK(newVa != 0, "patch target unresolved (%llx)", (unsigned long long)p.targetVa);
        const uint64_t dispAddr = codeVa + p.off + (p.len - 4);
        const int32_t newDisp = int32_t(newVa - (dispAddr + 4));
        std::memcpy(reinterpret_cast<void*>(dispAddr), &newDisp, 4);
    }

        auto core = reinterpret_cast<CoreFn>(codeVa);

    // Semantic of the caller FUN_10183bfc7 (and thus c095):
    //   out(arg3) = in(arg2) XOR keystream(state)
    // where state = consts|key|counter64|nonce[0..8]; the core advances the
    // state counter by ceil(len/64). The sweep verifies exactly that.
    auto runOne = [&](uint8_t* dst, const uint8_t* src, size_t len, const uint8_t* key,
                      const uint8_t* nonce12, uint64_t counterStart) {
        uint8_t state[64];
        std::memcpy(state, "expand 32-byte k", 16);
        std::memcpy(state + 16, key, 32);
        std::memcpy(state + 48, &counterStart, 8);
        std::memcpy(state + 56, nonce12, 8);
        core(state, const_cast<uint8_t*>(src), dst, len);
    };

    // ---- direction pin: arg3 receives in ^ ks ------------------------------
    {
        uint8_t key[32], nonce12[12];
        for (int i = 0; i < 32; ++i) key[i] = uint8_t(i);
        aead::makeRecvNonce(nonce12, 0x1122334455667788ull);

        std::vector<uint8_t> a(64, 0xAA), b(64, 0xBB), zeros(64, 0), ks(64), expect(64);
        aead::keystream(ks.data(), zeros.data(), 64, key, 1, nonce12);
        for (size_t i = 0; i < 64; ++i) expect[i] = uint8_t(a[i] ^ ks[i]);

        runOne(b.data(), a.data(), 64, key, nonce12, 1);
        CHECK(b == expect, "direction probe: arg3 must receive in^ks");
    }

    // ---- sweep: arg order is (state, src, dst) ----------------------------
    {
        for (int keyMode = 0; keyMode < 3; ++keyMode) {
            for (int nonceMode = 0; nonceMode < 3; ++nonceMode) {
                uint8_t key[32], nonce12[12];
                for (int i = 0; i < 32; ++i)
                    key[i] = keyMode == 0 ? uint8_t(i)
                             : keyMode == 1 ? uint8_t(0x5a + i * 3)
                                            : uint8_t(0xff - i);
                aead::makeRecvNonce(nonce12, 0x1122334455667788ull + nonceMode * 0x1000);

                for (int len : {1, 63, 64, 65, 127, 128, 200, 512, 1000}) {
                    std::vector<uint8_t> src((size_t)len), got((size_t)len, 0xAA),
                        zeros((size_t)len, 0), ks((size_t)len);
                    for (int i = 0; i < len; ++i) src[i] = uint8_t(i * 7 + len);
                    aead::keystream(ks.data(), zeros.data(), (size_t)len, key, 1, nonce12);
                    std::vector<uint8_t> expect((size_t)len);
                    for (int i = 0; i < len; ++i) expect[i] = uint8_t(src[i] ^ ks[i]);

                    runOne(got.data(), src.data(), (size_t)len, key, nonce12, 1);
                    CHECK(got == expect, "core mismatch key=%d nonce=%d len=%d",
                          keyMode, nonceMode, len);
                }
            }
        }
    }

    // ---- counter continuity across calls ----------------------------------
    // The exported semantics (verified): the state counter advances by
    // ceil(len/64) — full blocks and the partial tail both consume one
    // counter value — so a later call resumes after the tail's block.
    {
        uint8_t key[32], nonce12[12];
        for (int i = 0; i < 32; ++i) key[i] = uint8_t(0x20 + i);
        aead::makeSendNonce(nonce12, 42);

        std::vector<uint8_t> src(160, 0x55), zeros(160, 0), ks1(160), ks2(160);
        aead::keystream(ks1.data(), zeros.data(), 160, key, 1, nonce12);
        aead::keystream(ks2.data(), zeros.data(), 60, key, 3, nonce12);

        uint8_t state[64];
        std::memcpy(state, "expand 32-byte k", 16);
        std::memcpy(state + 16, key, 32);
        const uint64_t counter = 1;
        std::memcpy(state + 48, &counter, 8);
        std::memcpy(state + 56, nonce12, 8);

        std::vector<uint8_t> out(160, 0xEE);
        core(state, const_cast<uint8_t*>(src.data()), out.data(), 100);
        CHECK(*(uint32_t*)(state + 48) == 3, "counter after 100 bytes = %u (ceil advance)",
              *(uint32_t*)(state + 48));
        core(state, const_cast<uint8_t*>(src.data()) + 100, out.data() + 100, 60);

        std::vector<uint8_t> expect(160);
        for (int i = 0; i < 100; ++i) expect[i] = uint8_t(src[i] ^ ks1[i]);
        for (int i = 0; i < 60; ++i) expect[100 + i] = uint8_t(src[100 + i] ^ ks2[i]);
        CHECK(out == expect, "counter continuity across calls");
    }

    // ---- in-place (both pointers the same buffer) -------------------------
    {
        uint8_t key[32], nonce12[12], buf[300];
        for (int i = 0; i < 32; ++i) key[i] = uint8_t(0x11 * i + 5);
        aead::makeRecvNonce(nonce12, 9);
        for (int i = 0; i < 300; ++i) buf[i] = uint8_t(255 - i);

        std::vector<uint8_t> zeros(300, 0), ks(300);
        aead::keystream(ks.data(), zeros.data(), 300, key, 1, nonce12);
        std::vector<uint8_t> expect(300);
        for (int i = 0; i < 300; ++i) expect[i] = uint8_t(buf[i] ^ ks[i]);

        uint8_t state[64];
        std::memcpy(state, "expand 32-byte k", 16);
        std::memcpy(state + 16, key, 32);
        const uint64_t counter = 1;
        std::memcpy(state + 48, &counter, 8);
        std::memcpy(state + 56, nonce12, 8);

        core(state, buf, buf, 300);
        CHECK(std::memcmp(buf, expect.data(), 300) == 0, "in-place XOR");
    }

    if (g_failures == 0) {
        std::printf("binary chacha core executed and matched (out = in ^ ks)\n");
        return 0;
    }
    std::printf("%d test failure(s)\n", g_failures);
    return 1;
}
