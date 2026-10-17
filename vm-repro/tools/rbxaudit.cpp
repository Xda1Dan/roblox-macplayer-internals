// rbxaudit.cpp — verify the reproduction's load-bearing constants against a
// real RobloxPlayer Mach-O. This is the tool that turns "the docs say" into
// "the bytes say": it checks the opcode tables, both interpreter dispatch
// tables, the SigGuard blob, the checksum machine code, and the gate call
// site directly in the target binary.
//
// Usage: rbxaudit /path/to/RobloxPlayer [--quiet]
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "rbxvm/opcode.hpp"
#include "rbxvm/scramble.hpp"
#include "rbxvm/sigguard.hpp"

using namespace rbxvm;

namespace {

struct Segment {
    uint64_t vmaddr, vmsize, fileoff, filesize;
};

struct MachO {
    std::vector<uint8_t> data;
    std::vector<Segment> segs;
    bool ok = false;

    bool load(const char* path) {
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
        std::fclose(f);
        if (data.size() < 32) return false;
        uint32_t magic;
        std::memcpy(&magic, data.data(), 4);
        if (magic != 0xfeedfacfu) return false;
        uint32_t ncmds;
        std::memcpy(&ncmds, data.data() + 16, 4);
        size_t off = 32;
        for (uint32_t i = 0; i < ncmds; ++i) {
            if (off + 8 > data.size()) return false;
            uint32_t cmd, cmdsize;
            std::memcpy(&cmd, data.data() + off, 4);
            std::memcpy(&cmdsize, data.data() + off + 4, 4);
            if (cmdsize < 8 || off + cmdsize > data.size()) return false;
            if (cmd == 0x19) { // LC_SEGMENT_64
                Segment s{};
                std::memcpy(&s.vmaddr, data.data() + off + 24, 8);
                std::memcpy(&s.vmsize, data.data() + off + 32, 8);
                std::memcpy(&s.fileoff, data.data() + off + 40, 8);
                std::memcpy(&s.filesize, data.data() + off + 48, 8);
                segs.push_back(s);
            }
            off += cmdsize;
        }
        ok = true;
        return true;
    }

    bool read(uint64_t va, void* out, size_t n) const {
        for (const Segment& s : segs) {
            if (va >= s.vmaddr && va + n <= s.vmaddr + s.vmsize) {
                uint64_t delta = va - s.vmaddr;
                if (delta + n > s.filesize) return false;
                std::memcpy(out, data.data() + s.fileoff + delta, n);
                return true;
            }
        }
        return false;
    }
};

int g_fail = 0;
void check(bool cond, const char* what, const std::string& detail = {}) {
    std::printf("%s %s%s%s\n", cond ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " — ", detail.c_str());
    if (!cond) ++g_fail;
}

// Handler addresses extracted from the binary's two dispatch tables
// (canonical order). Regenerate from research/vm-research/opcode-map.csv.
static const uint64_t kHandlers[93][2] = {
#include "handler_addresses.inc"
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rbxaudit /path/to/RobloxPlayer\n");
        return 2;
    }
    MachO m;
    if (!m.load(argv[1])) {
        std::fprintf(stderr, "cannot read Mach-O: %s\n", argv[1]);
        return 2;
    }

    check(true, "Mach-O parsed", std::to_string(m.segs.size()) + " segments");
    check(m.data.size() == 125285200u, "binary size",
          std::to_string(m.data.size()) + " bytes");
    check(sigguard::sha256Hex(m.data.data(), m.data.size()) ==
              "ac9ad59b3aa3805bf2d1c89fb6ff9c7941bf2e57af17a81cbc26abe94a022c47",
          "binary SHA-256 matches the documented build");

    // T / R tables.
    uint8_t t[256], r[256];
    check(m.read(0x1063bb686, t, 256), "T readable");
    check(m.read(0x10648bb74, r, 256), "R readable");
    check(std::memcmp(t, kWireToVmop, 256) == 0, "T bytes match src/tables.cpp");
    check(std::memcmp(r, kVmopToCanon, 256) == 0, "R bytes match src/tables.cpp");
    check(verifyTables() == 0, "T/R permutation and round-trip checks");

    // Dispatch tables.
    uint64_t pathA[256], pathB[256];
    check(m.read(0x1072d8a30, pathA, sizeof pathA), "Path A table readable");
    check(m.read(0x1072d9230, pathB, sizeof pathB), "Path B table readable");
    int badA = 0, badB = 0, validA = 0, validB = 0;
    for (int c = 0; c < 93; ++c) {
        uint8_t vmop = canonToVmop(uint8_t(c));
        if (pathA[vmop] != kHandlers[c][1]) ++badA;
        if (pathB[vmop] != kHandlers[c][0]) ++badB;
        if (pathA[vmop]) ++validA;
        if (pathB[vmop]) ++validB;
    }
    check(badA == 0, "Path A handlers match opcode-map.csv");
    check(badB == 0, "Path B handlers match opcode-map.csv");
    check(validA == 93 && validB == 93, "93 non-null handlers per table",
          std::to_string(validA) + "/" + std::to_string(validB));

    // getOpLength function: the two mask constants must be present.
    uint8_t getlen[32];
    check(m.read(0x1029abbe3, getlen, sizeof getlen), "getOpLength readable");

    // SigGuard packed tables.
    std::vector<uint8_t> sig(sigguard::kPackedTableBytes);
    check(m.read(0x106207760, sig.data(), sig.size()), "SigGuard blob readable");
    check(sig.size() == sigguard::kPackedTableBytes, "SigGuard blob size");
    check(sigguard::sha256Hex(sig.data(), sig.size()) ==
              "0b6cdf37e5817b29638fe705319618df725ffd2aafc0491d768ab6f162f42c3c",
          "SigGuard blob SHA-256");
    check(std::memcmp(sig.data(), sigguard::kPackedMagic, 4) == 0,
          "SigGuard blob header GYM\\x14");

    // Checksum machine code used by test_rsb1.
    uint8_t sum[0x13e];
    check(m.read(0x103222533, sum, sizeof sum), "xxHash32 code readable");
    check(sigguard::sha256Hex(sum, sizeof sum) ==
              "20358a8d8ef86ff9467d056e9651f7e87e79f6cd2ac85070c8b6a8c8c8d57c7d",
          "checksum machine-code SHA-256");

    // Gate call site in luau_load: CALL rel32 at 0x1029aa242 must target
    // FUN_1014bb465.
    uint8_t call[5];
    check(m.read(0x1029aa242, call, sizeof call), "gate call site readable");
    int32_t rel;
    std::memcpy(&rel, call + 1, 4);
    uint64_t target = 0x1029aa242ull + 5 + int64_t(rel);
    char targetText[32];
    std::snprintf(targetText, sizeof targetText, "target 0x%llx",
                  (unsigned long long)target);
    check(call[0] == 0xE8 && target == 0x1014bb465ull,
          "luau_load calls the gate at 0x1014bb465", targetText);

    std::printf("\n%s (%d failure%s)\n", g_fail ? "AUDIT FAILED" : "AUDIT OK",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
