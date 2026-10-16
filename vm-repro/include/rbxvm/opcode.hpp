// opcode.hpp — canonical Luau opcode definitions for RobloxPlayer (macOS) build
// 0.742.0.7421053 (MacPlayer-version-f9247f9560044102).
//
// The canonical numbering 0..92 matches the upstream Luau
// Common/include/Luau/Bytecode.h enum (LOP_NOP..LOP_FINCONSTRUCT), verified
// against the binary via the reverse map R at VA 0x10648bb74 (see
// ../research/vm-research/opcode-map.csv for the full verified table including
// interpreter handler addresses for both dispatch paths).
//
// Three numberings exist in this build:
//   canonical : 0..92, the real ID (this enum)
//   wire      : (227 * canonical) & 0xFF          — bytes on disk / stream
//   vmop      : T[wire], T at VA 0x1063bb686      — what the interpreter sees
// See scramble.hpp for the transforms.
#pragma once

#include <cstdint>
#include <cstddef>

namespace rbxvm {

enum LuauOpcode : uint8_t {
    LOP_NOP = 0,
    LOP_BREAK,
    LOP_LOADNIL,
    LOP_LOADB,
    LOP_LOADN,
    LOP_LOADK,
    LOP_MOVE,
    LOP_GETGLOBAL,
    LOP_SETGLOBAL,
    LOP_GETUPVAL,
    LOP_SETUPVAL,
    LOP_CLOSEUPVALS,
    LOP_GETIMPORT,
    LOP_GETTABLE,
    LOP_SETTABLE,
    LOP_GETTABLEKS,
    LOP_SETTABLEKS,
    LOP_GETTABLEN,
    LOP_SETTABLEN,
    LOP_NEWCLOSURE,
    LOP_NAMECALL,
    LOP_CALL,
    LOP_RETURN,
    LOP_JUMP,
    LOP_JUMPBACK,
    LOP_JUMPIF,
    LOP_JUMPIFNOT,
    LOP_JUMPIFEQ,
    LOP_JUMPIFLE,
    LOP_JUMPIFLT,
    LOP_JUMPIFNOTEQ,
    LOP_JUMPIFNOTLE,
    LOP_JUMPIFNOTLT,
    LOP_ADD,
    LOP_SUB,
    LOP_MUL,
    LOP_DIV,
    LOP_MOD,
    LOP_POW,
    LOP_ADDK,
    LOP_SUBK,
    LOP_MULK,
    LOP_DIVK,
    LOP_MODK,
    LOP_POWK,
    LOP_AND,
    LOP_OR,
    LOP_ANDK,
    LOP_ORK,
    LOP_CONCAT,
    LOP_NOT,
    LOP_MINUS,
    LOP_LENGTH,
    LOP_NEWTABLE,
    LOP_DUPTABLE,
    LOP_SETLIST,
    LOP_FORNPREP,
    LOP_FORNLOOP,
    LOP_FORGLOOP,
    LOP_FORGPREP_INEXT,
    LOP_FASTCALL3,
    LOP_FORGPREP_NEXT,
    LOP_NATIVECALL,
    LOP_GETVARARGS,
    LOP_DUPCLOSURE,
    LOP_PREPVARARGS,
    LOP_LOADKX,
    LOP_JUMPX,
    LOP_FASTCALL,
    LOP_COVERAGE,
    LOP_CAPTURE,
    LOP_SUBRK,
    LOP_DIVRK,
    LOP_FASTCALL1,
    LOP_FASTCALL2,
    LOP_FASTCALL2K,
    LOP_FORGPREP,
    LOP_JUMPXEQKNIL,
    LOP_JUMPXEQKB,
    LOP_JUMPXEQKN,
    LOP_JUMPXEQKS,
    LOP_IDIV,
    LOP_IDIVK,
    LOP_GETUDATAKS,
    LOP_SETUDATAKS,
    LOP_NAMECALLUDATA,
    LOP_NEWCLASSMEMBER,
    LOP_CALLFB,
    LOP_CMPPROTO,
    LOP_FASTPCALL,
    LOP_NEWCLASS,
    LOP_CONSTRUCT,
    LOP_FINCONSTRUCT,
};

constexpr size_t kOpcodeCount = 93; // LOP_FINCONSTRUCT + 1

struct OpInfo {
    const char* name;
    bool has_aux; // instruction is 2 words (verified vs upstream getOpLength)
};

// Indexed by canonical opcode; defined in tables.cpp.
extern const OpInfo kOpInfos[kOpcodeCount];

// Word count of an instruction with the given canonical opcode.
// Identical to upstream CodeGen/src/IrUtils.cpp::getOpLength; 31 of the 93
// opcodes are two-word. (NEWCLOSURE is followed by CAPTURE instructions but
// is itself one word.)
constexpr unsigned getOpLength(uint8_t canonical_op) {
    switch (canonical_op) {
    case LOP_GETGLOBAL:
    case LOP_SETGLOBAL:
    case LOP_GETIMPORT:
    case LOP_GETTABLEKS:
    case LOP_SETTABLEKS:
    case LOP_NAMECALL:
    case LOP_JUMPIFEQ:
    case LOP_JUMPIFLE:
    case LOP_JUMPIFLT:
    case LOP_JUMPIFNOTEQ:
    case LOP_JUMPIFNOTLE:
    case LOP_JUMPIFNOTLT:
    case LOP_NEWTABLE:
    case LOP_SETLIST:
    case LOP_FORGLOOP:
    case LOP_LOADKX:
    case LOP_FASTCALL2:
    case LOP_FASTCALL2K:
    case LOP_FASTCALL3:
    case LOP_JUMPXEQKNIL:
    case LOP_JUMPXEQKB:
    case LOP_JUMPXEQKN:
    case LOP_JUMPXEQKS:
    case LOP_GETUDATAKS:
    case LOP_SETUDATAKS:
    case LOP_NAMECALLUDATA:
    case LOP_NEWCLASSMEMBER:
    case LOP_CALLFB:
    case LOP_CMPPROTO:
    case LOP_NEWCLASS:
    case LOP_CONSTRUCT:
        return 2;
    default:
        return 1;
    }
}

} // namespace rbxvm
