// test_core.cpp — verification of the rbxvm reproduction against ground truth.
//
// Ground truth sources:
//  - T/R tables dumped from the binary (src/tables.cpp, provenance in comments)
//  - per-opcode canonical/wire/vmop rows verified in vm-research/opcode-map.csv,
//    embedded below as tests/opmap_expected.inc (93 rows)
//  - instruction formats per upstream Bytecode.h (reference tree)
//  - pointer-encoding schemes per vm-research/luau-vm.md (proof: sentinels
//    in luaF_newproto + un-encode forms in luaF_freeproto)
#include <cstdio>
#include <cstring>
#include <string>

#include "rbxvm/opcode.hpp"
#include "rbxvm/scramble.hpp"
#include "rbxvm/insn.hpp"
#include "rbxvm/ptrenc.hpp"
#include "rbxvm/layouts.hpp"
#include "rbxvm/disasm.hpp"

using namespace rbxvm;

static int g_failures = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            ++g_failures;                                   \
            std::printf("FAIL: %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                       \
            std::printf("\n");                              \
        }                                                   \
    } while (0)

struct ExpectedRow { int canon; const char* name; bool aux; int wire; int vmop; };
static const ExpectedRow kExpected[] = {
#include "opmap_expected.inc"
};
static const size_t kExpectedCount = sizeof(kExpected) / sizeof(kExpected[0]);

static void testTablesAgainstCsv() {
    CHECK(kExpectedCount == kOpcodeCount, "fixture rows %zu != %zu", kExpectedCount, kOpcodeCount);
    for (size_t r = 0; r < kExpectedCount; ++r) {
        const ExpectedRow& e = kExpected[r];
        CHECK(size_t(e.canon) == r, "row %zu: canonical mismatch (%d)", r, e.canon);
        CHECK(std::strcmp(kOpInfos[r].name, e.name) == 0, "row %zu: name %s != %s",
              r, kOpInfos[r].name, e.name);
        CHECK(kOpInfos[r].has_aux == e.aux, "row %zu (%s): aux flag mismatch", r, e.name);
        CHECK(canonToWire(uint8_t(r)) == uint8_t(e.wire), "row %zu (%s): wire %u != %d",
              r, e.name, canonToWire(uint8_t(r)), e.wire);
        CHECK(kWireToVmop[size_t(e.wire)] == uint8_t(e.vmop), "row %zu (%s): vmop %u != %d",
              r, e.name, kWireToVmop[size_t(e.wire)], e.vmop);
        CHECK(kVmopToCanon[size_t(e.vmop)] == uint8_t(r), "row %zu (%s): R[vmop] != canon", r, e.name);
        CHECK(getOpLength(uint8_t(r)) == (e.aux ? 2u : 1u), "row %zu (%s): length/aux mismatch", r, e.name);
    }
}

static void testInsnAccessors() {
    // ABC round-trip
    uint32_t i = makeInsnABC(LOP_GETTABLE, 7, 200, 255);
    CHECK(insnOp(i) == LOP_GETTABLE && insnA(i) == 7 && insnB(i) == 200 && insnC(i) == 255, "ABC");
    // AD round-trip incl. negative D
    i = makeInsnAD(LOP_JUMPIF, 3, -2);
    CHECK(insnA(i) == 3 && insnD(i) == -2, "AD neg");
    i = makeInsnAD(LOP_LOADN, 1, -32768);
    CHECK(insnD(i) == -32768, "AD min");
    i = makeInsnAD(LOP_LOADN, 1, 32767);
    CHECK(insnD(i) == 32767, "AD max");
    // E round-trip incl. extremes
    i = makeInsnE(LOP_JUMPX, -8388608);
    CHECK(insnE(i) == -8388608, "E min");
    i = makeInsnE(LOP_JUMPX, 8388607);
    CHECK(insnE(i) == 8388607, "E max");
    // jump target formula
    CHECK(jumpTarget(10, 0) == 11, "jump next");
    CHECK(jumpTarget(10, -5) == 6, "jump back");
    // GETIMPORT aux layout: components are extracted first->last as
    // (aux>>20)&0x3ff, (aux>>10)&0x3ff, aux&0x3ff, count in top 2 bits
    uint32_t aux = (3u << 30) | (7u << 20) | (9u << 10) | 5u;
    CHECK(auxImportCount(aux) == 3 && auxImportIndex(aux, 0) == 7 &&
          auxImportIndex(aux, 1) == 9 && auxImportIndex(aux, 2) == 5, "GETIMPORT aux");
    // JUMPXEQK aux layout
    aux = (1u << 31) | 1234u;
    CHECK(auxNotFlag(aux) == 1 && auxK24(aux) == 1234, "JUMPXEQK aux");
}

static void testPtrEnc() {
    const uintptr_t base = 0x7f3a00000000ull;
    struct Case { size_t off; Enc e; };
    const Case cases[] = {
        {0x08, Enc::Add},    // Proto.locvars
        {0x10, Enc::RevSub}, // Proto.lineinfo
        {0x18, Enc::Sub},    // Proto.abslineinfo
        {0x30, Enc::RevSub}, // Proto.source
        {0x40, Enc::Xor},    // Proto.userdata
        {0x50, Enc::RevSub}, // Proto.debugname
        {0x58, Enc::Xor},    // Proto.upvalues
        {0x60, Enc::Add},    // Proto.typeinfo
        {0x80, Enc::RevSub}, // Proto.debuginsn
        {0x40, Enc::Xor},    // lua_State.stacksize
        {0x10, Enc::Add},    // TString.hash
    };
    for (const Case& cs : cases) {
        const uintptr_t field = base + cs.off;
        for (uintptr_t real : {uintptr_t(0), uintptr_t(0x1000), uintptr_t(0xdeadbeef),
                               base, uintptr_t(0xffffffffffffffffull)}) {
            uintptr_t stored = ptrEncode(field, real, cs.e);
            CHECK(ptrDecode(field, stored, cs.e) == real, "round-trip off=0x%zx enc=%d real=%#zx",
                  cs.off, int(cs.e), real);
        }
        // empty sentinel decodes to 0 (the luaF_newproto property)
        CHECK(ptrDecode(field, ptrEmptySentinel(field, cs.e), cs.e) == 0,
              "sentinel off=0x%zx enc=%d", cs.off, int(cs.e));
    }
}

static void testDisasm() {
    // A tiny canonical program:
    //   0: PREPVARARGS 1
    //   1: LOADNIL R0
    //   2: LOADN R1, 42
    //   3: ADD R2, R0, R1
    //   4: GETIMPORT R3 K5 path K2.K1.K1   (2 words; aux len=3, comps 2,1,1)
    //   6: JUMPX -> 8
    //   7: JUMPX -> 4
    //   8: RETURN R2, 1
    uint32_t code[] = {
        makeInsnA(LOP_PREPVARARGS, 1),
        makeInsnA(LOP_LOADNIL, 0),
        makeInsnAD(LOP_LOADN, 1, 42),
        makeInsnABC(LOP_ADD, 2, 0, 1),
        makeInsnAD(LOP_GETIMPORT, 3, 5),
        (3u << 30) | (2u << 20) | (1u << 10) | 1u, // AUX: len 3, path K2.K1.K1
        makeInsnE(LOP_JUMPX, 1),                   // at 6: -> 8
        makeInsnE(LOP_JUMPX, -4),                  // at 7: -> 4
        makeInsnABC(LOP_RETURN, 2, 2, 0),          // B = nresults+1 = 2
    };
    const size_t n = sizeof(code) / sizeof(code[0]);

    // canonical form round trip (8 instructions, 9 words — GETIMPORT is 2 words)
    {
        std::string out;
        long lines = disassemble(code, n, OpEnc::Canon, 0, out);
        CHECK(lines == 8, "canon lines %ld", lines);
        CHECK(out.find("PREPVARARGS 1") != std::string::npos, "canon text PREPVARARGS");
        CHECK(out.find("LOADN R1 42") != std::string::npos, "canon text LOADN");
        CHECK(out.find("ADD R2 R0 R1") != std::string::npos, "canon text ADD");
        CHECK(out.find("GETIMPORT R3 K5 [K2.K1.K1]") != std::string::npos, "canon GETIMPORT path");
        CHECK(out.find("JUMPX -> 8") != std::string::npos, "canon JUMPX fwd");
        CHECK(out.find("JUMPX -> 4") != std::string::npos, "canon JUMPX back");
        CHECK(out.find("RETURN R2 1") != std::string::npos, "canon RETURN");
    }

    // the same program, wire-encoded (canonical * 227): what ships on disk
    {
        uint32_t wire[n];
        for (size_t i = 0; i < n; ++i)
            wire[i] = (code[i] & 0xffffff00u) | canonToWire(insnOp(code[i]));
        std::string out;
        disassemble(wire, n, OpEnc::Wire, 0, out);
        CHECK(out.find("LOADN R1 42") != std::string::npos, "wire LOADN");
        CHECK(out.find("GETIMPORT R3 K5") != std::string::npos, "wire GETIMPORT");
        CHECK(out.find("RETURN R2 1") != std::string::npos, "wire RETURN");
    }

    // the same program, vmop-encoded (T[wire]): what luau_load leaves in memory
    {
        uint32_t vm[n];
        for (size_t i = 0; i < n; ++i) {
            uint8_t w = canonToWire(insnOp(code[i]));
            vm[i] = (code[i] & 0xffffff00u) | kWireToVmop[w];
        }
        std::string out;
        disassemble(vm, n, OpEnc::Vmop, 0, out);
        CHECK(out.find("ADD R2 R0 R1") != std::string::npos, "vmop ADD");
        CHECK(out.find("JUMPX -> 4") != std::string::npos, "vmop JUMPX back");
    }

    // invalid byte handling: must not hang or crash. In vmop numbering the
    // byte 0x00 is invalid (R[0] = 176 >= 93), while 0xff is valid (R[0xff]=19
    // = NEWCLOSURE) — a nice demonstration that vmop != canonical. The third
    // word is NOP *in vmop form* (vmop byte 52, since T[wire 0] = 52).
    {
        uint32_t junk[] = {0xffffffffu, 0x00000000u,
                           (makeInsnA(LOP_NOP, 0) & 0xffffff00u) | canonToVmop(LOP_NOP)};
        std::string out;
        long lines = disassemble(junk, 3, OpEnc::Vmop, 0, out);
        CHECK(lines == 2, "junk lines %ld", lines);
        CHECK(out.find("NEWCLOSURE") != std::string::npos, "junk NEWCLOSURE (0xff valid)");
        CHECK(out.find("raw 00000000") != std::string::npos, "junk raw echo");
        CHECK(out.find("NOP") != std::string::npos, "junk resync NOP");
    }
}

static void testLayoutConstants() {
    // runtime mirror of the static_asserts, plus a few documented anchors
    CHECK(offs::lua_State_::singlestep == 0x06, "singlestep");
    CHECK(offs::lua_State_::stack == 0x30 && offs::lua_State_::base_ci == 0x78, "stack/base_ci");
    CHECK(offs::CallInfo_::stride == 0x30, "ci stride");
    CHECK(offs::Proto_::code == 0x28 && offs::Proto_::k == 0x20 && offs::Proto_::p == 0x48,
          "proto hot path");
    CHECK(offs::TString_::hash == 0x10 && offs::TString_::data == 0x18, "tstring");
    CHECK(offs::Table_::node == 0x10 && offs::Table_::array == 0x18, "table");
    CHECK(offs::thread_status::offset == 0xac, "scheduler thread status");
}

int main() {
    testTablesAgainstCsv();
    testInsnAccessors();
    testPtrEnc();
    testDisasm();
    testLayoutConstants();

    if (g_failures == 0) {
        std::printf("all tests passed (%zu opcode rows, tables, encodings, layouts, disasm)\n",
                    kExpectedCount);
        return 0;
    }
    std::printf("%d test failure(s)\n", g_failures);
    return 1;
}
