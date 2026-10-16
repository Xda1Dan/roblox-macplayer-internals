// tables.cpp — ground-truth data dumped from RobloxPlayer
// (MacPlayer-version-f9247f9560044102) via Ghidra.
//
// kWireToVmop (T): read_memory VA 0x1063bb686, 256 bytes.
// kVmopToCanon (R): read_memory VA 0x10648bb74, 256 bytes.
// Both verified as permutations; see tests/test_core.cpp.
#include "rbxvm/opcode.hpp"
#include "rbxvm/scramble.hpp"

namespace rbxvm {

// T — wire->vmop permutation, VA 0x1063bb686
const uint8_t kWireToVmop[256] = {
    52, 2, 42, 32, 12, 102, 38, 184, 22, 20, 62, 136, 26, 18, 80, 91,
    0, 104, 118, 164, 230, 96, 156, 46, 74, 166, 6, 245, 56, 248, 187, 167,
    100, 158, 31, 188, 161, 40, 251, 108, 97, 70, 206, 63, 39, 64, 72, 9,
    236, 172, 34, 67, 78, 58, 43, 211, 155, 83, 47, 162, 36, 82, 14, 228,
    249, 154, 231, 222, 242, 81, 68, 238, 150, 135, 84, 148, 116, 120, 146, 95,
    24, 61, 160, 128, 194, 212, 210, 132, 226, 35, 252, 220, 182, 233, 126, 127,
    152, 130, 244, 131, 142, 69, 77, 33, 48, 185, 246, 239, 51, 234, 183, 204,
    76, 196, 203, 240, 8, 197, 30, 49, 235, 225, 110, 101, 88, 198, 134, 175,
    171, 125, 190, 143, 99, 189, 66, 159, 173, 59, 237, 117, 140, 208, 10, 243,
    71, 29, 178, 89, 79, 3, 122, 113, 133, 141, 216, 224, 4, 21, 151, 207,
    218, 254, 138, 1, 92, 105, 192, 215, 112, 163, 180, 53, 200, 55, 45, 191,
    229, 247, 16, 181, 94, 195, 28, 17, 50, 232, 44, 227, 186, 213, 139, 217,
    115, 219, 124, 169, 37, 87, 107, 214, 90, 15, 111, 129, 205, 223, 157, 25,
    250, 93, 60, 253, 114, 11, 177, 241, 209, 255, 23, 5, 98, 147, 144, 121,
    13, 165, 168, 85, 75, 123, 170, 201, 7, 221, 202, 149, 106, 119, 103, 41,
    179, 65, 174, 73, 153, 145, 86, 193, 54, 19, 176, 109, 199, 137, 27, 57,
};

// R — vmop->canonical reverse map, VA 0x10648bb74
const uint8_t kVmopToCanon[256] = {
    176, 65, 203, 39, 180, 169, 158, 248, 252, 69, 154, 231, 44, 160, 42, 99,
    38, 29, 79, 115, 35, 127, 88, 222, 112, 37, 132, 106, 82, 251, 146, 246,
    97, 173, 166, 147, 148, 108, 194, 228, 87, 133, 150, 210, 126, 250, 61, 254,
    120, 93, 232, 164, 0, 153, 168, 47, 52, 53, 7, 163, 134, 59, 238, 25,
    175, 27, 66, 113, 130, 23, 131, 48, 122, 177, 8, 204, 208, 226, 60, 92,
    26, 183, 95, 51, 174, 1, 18, 55, 84, 145, 152, 229, 12, 187, 188, 165,
    167, 184, 116, 172, 96, 137, 247, 186, 123, 215, 36, 2, 237, 9, 190, 46,
    56, 189, 28, 64, 68, 57, 70, 239, 15, 213, 242, 151, 214, 75, 138, 85,
    209, 249, 235, 129, 253, 136, 234, 227, 185, 159, 118, 170, 4, 83, 76, 225,
    10, 71, 218, 63, 121, 89, 24, 74, 32, 124, 139, 104, 114, 90, 43, 13,
    6, 140, 201, 3, 17, 107, 211, 149, 54, 161, 98, 128, 219, 216, 230, 181,
    62, 178, 198, 80, 206, 241, 244, 58, 141, 67, 20, 202, 193, 119, 22, 197,
    162, 221, 156, 135, 155, 199, 31, 212, 100, 45, 142, 102, 5, 196, 78, 21,
    207, 72, 50, 157, 103, 223, 205, 109, 30, 117, 224, 11, 41, 195, 33, 143,
    233, 243, 200, 73, 245, 144, 220, 86, 179, 191, 111, 40, 16, 110, 77, 217,
    49, 125, 236, 101, 182, 105, 14, 91, 255, 192, 240, 34, 94, 81, 171, 19,
};

const OpInfo kOpInfos[kOpcodeCount] = {
    /* NOP            */ {"NOP", false},
    /* BREAK          */ {"BREAK", false},
    /* LOADNIL        */ {"LOADNIL", false},
    /* LOADB          */ {"LOADB", false},
    /* LOADN          */ {"LOADN", false},
    /* LOADK          */ {"LOADK", false},
    /* MOVE           */ {"MOVE", false},
    /* GETGLOBAL      */ {"GETGLOBAL", true},
    /* SETGLOBAL      */ {"SETGLOBAL", true},
    /* GETUPVAL       */ {"GETUPVAL", false},
    /* SETUPVAL       */ {"SETUPVAL", false},
    /* CLOSEUPVALS    */ {"CLOSEUPVALS", false},
    /* GETIMPORT      */ {"GETIMPORT", true},
    /* GETTABLE       */ {"GETTABLE", false},
    /* SETTABLE       */ {"SETTABLE", false},
    /* GETTABLEKS     */ {"GETTABLEKS", true},
    /* SETTABLEKS     */ {"SETTABLEKS", true},
    /* GETTABLEN      */ {"GETTABLEN", false},
    /* SETTABLEN      */ {"SETTABLEN", false},
    /* NEWCLOSURE     */ {"NEWCLOSURE", false},
    /* NAMECALL       */ {"NAMECALL", true},
    /* CALL           */ {"CALL", false},
    /* RETURN         */ {"RETURN", false},
    /* JUMP           */ {"JUMP", false},
    /* JUMPBACK       */ {"JUMPBACK", false},
    /* JUMPIF         */ {"JUMPIF", false},
    /* JUMPIFNOT      */ {"JUMPIFNOT", false},
    /* JUMPIFEQ       */ {"JUMPIFEQ", true},
    /* JUMPIFLE       */ {"JUMPIFLE", true},
    /* JUMPIFLT       */ {"JUMPIFLT", true},
    /* JUMPIFNOTEQ    */ {"JUMPIFNOTEQ", true},
    /* JUMPIFNOTLE    */ {"JUMPIFNOTLE", true},
    /* JUMPIFNOTLT    */ {"JUMPIFNOTLT", true},
    /* ADD            */ {"ADD", false},
    /* SUB            */ {"SUB", false},
    /* MUL            */ {"MUL", false},
    /* DIV            */ {"DIV", false},
    /* MOD            */ {"MOD", false},
    /* POW            */ {"POW", false},
    /* ADDK           */ {"ADDK", false},
    /* SUBK           */ {"SUBK", false},
    /* MULK           */ {"MULK", false},
    /* DIVK           */ {"DIVK", false},
    /* MODK           */ {"MODK", false},
    /* POWK           */ {"POWK", false},
    /* AND            */ {"AND", false},
    /* OR             */ {"OR", false},
    /* ANDK           */ {"ANDK", false},
    /* ORK            */ {"ORK", false},
    /* CONCAT         */ {"CONCAT", false},
    /* NOT            */ {"NOT", false},
    /* MINUS          */ {"MINUS", false},
    /* LENGTH         */ {"LENGTH", false},
    /* NEWTABLE       */ {"NEWTABLE", true},
    /* DUPTABLE       */ {"DUPTABLE", false},
    /* SETLIST        */ {"SETLIST", true},
    /* FORNPREP       */ {"FORNPREP", false},
    /* FORNLOOP       */ {"FORNLOOP", false},
    /* FORGLOOP       */ {"FORGLOOP", true},
    /* FORGPREP_INEXT */ {"FORGPREP_INEXT", false},
    /* FASTCALL3      */ {"FASTCALL3", true},
    /* FORGPREP_NEXT  */ {"FORGPREP_NEXT", false},
    /* NATIVECALL     */ {"NATIVECALL", false},
    /* GETVARARGS     */ {"GETVARARGS", false},
    /* DUPCLOSURE     */ {"DUPCLOSURE", false},
    /* PREPVARARGS    */ {"PREPVARARGS", false},
    /* LOADKX         */ {"LOADKX", true},
    /* JUMPX          */ {"JUMPX", false},
    /* FASTCALL       */ {"FASTCALL", false},
    /* COVERAGE       */ {"COVERAGE", false},
    /* CAPTURE        */ {"CAPTURE", false},
    /* SUBRK          */ {"SUBRK", false},
    /* DIVRK          */ {"DIVRK", false},
    /* FASTCALL1      */ {"FASTCALL1", false},
    /* FASTCALL2      */ {"FASTCALL2", true},
    /* FASTCALL2K     */ {"FASTCALL2K", true},
    /* FORGPREP       */ {"FORGPREP", false},
    /* JUMPXEQKNIL    */ {"JUMPXEQKNIL", true},
    /* JUMPXEQKB      */ {"JUMPXEQKB", true},
    /* JUMPXEQKN      */ {"JUMPXEQKN", true},
    /* JUMPXEQKS      */ {"JUMPXEQKS", true},
    /* IDIV           */ {"IDIV", false},
    /* IDIVK          */ {"IDIVK", false},
    /* GETUDATAKS     */ {"GETUDATAKS", true},
    /* SETUDATAKS     */ {"SETUDATAKS", true},
    /* NAMECALLUDATA  */ {"NAMECALLUDATA", true},
    /* NEWCLASSMEMBER */ {"NEWCLASSMEMBER", true},
    /* CALLFB         */ {"CALLFB", true},
    /* CMPPROTO       */ {"CMPPROTO", true},
    /* FASTPCALL      */ {"FASTPCALL", false},
    /* NEWCLASS       */ {"NEWCLASS", true},
    /* CONSTRUCT      */ {"CONSTRUCT", true},
    /* FINCONSTRUCT   */ {"FINCONSTRUCT", false},
};

static_assert(sizeof(kOpInfos) / sizeof(kOpInfos[0]) == kOpcodeCount, "op table size");

int verifyTables() {
    int failures = 0;

    // 1. T and R are permutations of 0..255
    {
        int seenT[256] = {}, seenR[256] = {};
        for (int i = 0; i < 256; ++i) {
            seenT[kWireToVmop[i]]++;
            seenR[kVmopToCanon[i]]++;
        }
        for (int v = 0; v < 256; ++v) {
            if (seenT[v] != 1) ++failures;
            if (seenR[v] != 1) ++failures;
        }
    }

    // 2. R[T[(227*c)&0xFF]] == c for every canonical opcode
    for (int c = 0; c < int(kOpcodeCount); ++c) {
        uint8_t wire = canonToWire(uint8_t(c));
        uint8_t vmop = kWireToVmop[wire];
        if (kVmopToCanon[vmop] != uint8_t(c)) ++failures;
    }

    // 3. the 93 canonical opcodes map to 93 distinct vmop values
    {
        int seen[256] = {};
        for (int c = 0; c < int(kOpcodeCount); ++c)
            ++seen[canonToVmop(uint8_t(c))];
        for (int v = 0; v < 256; ++v)
            if (seen[v] > 1) ++failures;
    }

    return failures;
}

} // namespace rbxvm
