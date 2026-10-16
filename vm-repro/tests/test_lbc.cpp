// test_lbc.cpp — verification of the LBC chunk reader + the full pipeline:
//
//   luau source --(reference compiler)--> canonical LBC   [fixture, committed]
//     --(convertOps toWire)--> Roblox wire ops
//     --(RSB1 container encode)--> scrambled container
//     --(container decode)--> wire LBC
//     --(parse + convertOps toCanon)--> identical disassembly
//
// The fixture was produced by the reference tree's luau-compile (same Luau
// version family as the target build) and validated against the reference
// loader (luau_load) — K0..K9 and the 4-proto structure are its output.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rbxvm/lbc.hpp"
#include "rbxvm/opcode.hpp"
#include "rbxvm/rsb1.hpp"

using namespace rbxvm;

static int g_failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            ++g_failures;                                    \
            std::printf("FAIL: %s:%d: ", __FILE__, __LINE__);\
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                              \
        }                                                    \
    } while (0)

static std::vector<uint8_t> readFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    return data;
}

static void testParseFixture(const std::vector<uint8_t>& blob) {
    lbc::Module m = lbc::parse(blob.data(), blob.size());
    CHECK(m.version == 14, "version %u", m.version);
    CHECK(m.typesversion == 3, "typesversion %u", m.typesversion);
    CHECK(m.strings.size() == 7, "strings %zu", m.strings.size());
    CHECK(m.protos.size() == 4, "protos %zu", m.protos.size());
    CHECK(m.mainProto == 3, "main proto %u", m.mainProto);

    // proto #0: add(a, b) — 2 params, 2 instructions
    CHECK(m.str(m.protos[0].debugnameIdx) == "add", "proto0 name '%s'",
          m.str(m.protos[0].debugnameIdx).c_str());
    CHECK(m.protos[0].numparams == 2, "proto0 params");
    CHECK(m.protos[0].code.size() == 2, "proto0 code size");

    // the counter inner closure mutates its upvalue: GETUPVAL/ADDK/SETUPVAL
    CHECK(m.protos[1].nups == 1, "proto1 nups");
    bool hasSetupval = false;
    for (uint32_t w : m.protos[1].code)
        if ((w & 0xff) == LOP_SETUPVAL) hasSetupval = true;
    CHECK(hasSetupval, "proto1 has SETUPVAL");

    // main proto: 27 words, 10 constants (validated against the reference loader)
    CHECK(m.protos[3].code.size() == 27, "main code size %zu", m.protos[3].code.size());
    CHECK(m.protos[3].k.size() == 10, "main consts %zu", m.protos[3].k.size());
    // K1 = "x", K3 = "y", K4 = "two" (the table constructor constants)
    CHECK(m.protos[3].k[1].kind == lbc::Constant::Kind::String && m.str(m.protos[3].k[1].index) == "x",
          "K1 == \"x\"");
    CHECK(m.protos[3].k[4].kind == lbc::Constant::Kind::String && m.str(m.protos[3].k[4].index) == "two",
          "K4 == \"two\"");
    // K5 = table with constants
    CHECK(m.protos[3].k[5].kind == lbc::Constant::Kind::TableWithConstants,
          "K5 is a table-with-constants");
}

static void testFullPipeline(const std::vector<uint8_t>& canon) {
    using namespace rsb1;

    // reference dump (canonical)
    lbc::Module m0 = lbc::parse(canon.data(), canon.size());
    const std::string dumpCanon = lbc::dump(m0, OpEnc::Canon);

    // 1. canonical -> wire ops (what the server ships inside the container)
    std::vector<uint8_t> wire = canon;
    lbc::convertOps(wire.data(), wire.size(), /*toWire=*/true);
    // op bytes must differ somewhere (the scramble is non-identity)
    CHECK(wire != canon, "wire conversion changed bytes");

    // 2. wrap in an RSB1 container (size 0 = raw bytecode payload)
    std::vector<uint8_t> plain = buildPlain(wire.data(), wire.size(), 0);
    std::vector<uint8_t> container = encodeContainer(plain);

    // 3. decode the container (the client path)
    Decoded d = decodeContainer(container.data(), container.size(), nullptr);
    CHECK(d.status == DecodeStatus::OkPlain, "container decode status %d", int(d.status));
    CHECK(d.payload.size() == wire.size(), "payload size");

    // 4. parse the wire blob and disassemble with OpEnc::Wire — the text must
    //    match the canonical dump exactly
    lbc::Module m1 = lbc::parse(d.payload.data(), d.payload.size());
    const std::string dumpWire = lbc::dump(m1, OpEnc::Wire);
    CHECK(dumpWire == dumpCanon, "wire disassembly == canonical disassembly");

    // 5. and converting the wire blob back to canonical gives the same too
    std::vector<uint8_t> back = d.payload;
    lbc::convertOps(back.data(), back.size(), /*toWire=*/false);
    lbc::Module m2 = lbc::parse(back.data(), back.size());
    const std::string dumpBack = lbc::dump(m2, OpEnc::Canon);
    CHECK(dumpBack == dumpCanon, "converted-back disassembly == canonical");
}

int main(int argc, char** argv) {
    const char* fixture = argc > 1 ? argv[1] : "tests/fixtures/rich.lbc";
    std::vector<uint8_t> blob = readFile(fixture);
    if (blob.empty()) {
        std::printf("FAIL: cannot read fixture %s\n", fixture);
        return 1;
    }
    CHECK(blob[0] == 14, "fixture version byte");

    testParseFixture(blob);
    testFullPipeline(blob);

    if (g_failures == 0) {
        std::printf("all lbc tests passed\n");
        return 0;
    }
    std::printf("%d test failure(s)\n", g_failures);
    return 1;
}
