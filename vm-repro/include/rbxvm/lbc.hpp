// lbc.hpp — reader for the Luau serialized-chunk (LBC) format as consumed by
// RobloxPlayer's luau_load (VA 0x1029aa05a; version gates 3..14 and 100 match
// LBC_VERSION_MIN/MAX/CLASSES of the reference tree).
//
// Format (from VM/src/lvmload.cpp of the reference tree, cross-checked against
// the binary's deserialization structure):
//
//   u8 version                                  (0 = error message follows)
//   if version >= 4: u8 typesversion            (1..3)
//   varint stringCount; per string: varint len + bytes
//   if typesversion == 3: u8 index; while index != 0 { varint-string name; u8 index }
//   varint protoCount
//   per proto:
//     if version >= 12: varint protoSize        (section size; unknown tail skipped)
//     u8 maxstacksize, u8 numparams, u8 nups, u8 is_vararg
//     if version >= 4: u8 flags
//       varint typesize; typesize bytes         (type info, v2/v3 layout)
//     varint sizecode; sizecode * u32 LE        (instruction words)
//     varint sizek; per constant: u8 tag
//        0 NIL — | 1 BOOLEAN u8 | 2 NUMBER f64 | 3 STRING varint-index
//        4 IMPORT u32 | 5 TABLE varint keys * varint keyIdx
//        6 CLOSURE varint childProto | 7 VECTOR 4*f32
//        8 TABLE_WITH_CONSTANTS varint keys * (varint keyIdx, i32 constIdx)
//        9 INTEGER u8 neg + varint64 magnitude | 10 CLASS_SHAPE ...
//        11 VECTORD 4*f64
//     varint sizep; sizep * varint childProtoId
//     varint linedefined; varint debugNameStringIdx
//     u8 lineinfo flag; if set: u8 linegaplog2, sizecode*u8 deltas,
//        ((sizecode-1)>>gap)+1 * i32 abs lines
//     u8 debuginfo flag; if set: varint locvars * (string,varint,varint,u8),
//        varint nups * varint nameIdx
//     if version >= 11: varint feedback slots * (u8 kind [+ varint pc|shape])
//     if version >= 12 && (flags & 1): varint64 cost
//   varint mainProtoId
//
// NOTE on opcode numbering: instruction words inside an LBC stream carry the
// WIRE opcode byte ((227*c)&0xFF) in the Roblox build — luau_load rewrites
// them in place to vmop during load. Bytecode produced by the reference
// compiler (luau-compile --binary) is CANONICAL. Parse with the matching
// OpEnc so disassembly resolves correctly.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "rbxvm/disasm.hpp"

namespace rbxvm {
namespace lbc {

struct Constant {
    enum class Kind : uint8_t {
        Nil = 0, Boolean, Number, String, Import, Table, Closure, Vector,
        TableWithConstants, Integer, ClassShape, VectorD,
    };
    Kind kind = Kind::Nil;
    bool b = false;               // Boolean
    double num = 0.0;             // Number
    float vec[4] = {0, 0, 0, 0};  // Vector (float components)
    double vecd[4] = {0, 0, 0, 0};// VectorD (double components, kept exact)
    uint32_t index = 0;           // String idx / Import id / Closure child proto
    uint64_t integer = 0;         // Integer (magnitude; sign in neg)
    bool neg = false;
    std::vector<uint32_t> tableKeys;          // Table / TableWithConstants
    std::vector<int32_t> tableConstIdx;       // TableWithConstants (-1 = plain 0.0)
    uint32_t classProps = 0, classMethods = 0;
    std::vector<uint32_t> classMembers;       // ClassShape member name indices
};

struct LocVar {
    uint32_t nameIdx = 0;
    uint32_t startpc = 0, endpc = 0;
    uint8_t reg = 0;
};

struct FeedbackSlot {
    uint8_t kind = 0;
    uint32_t target = 0; // pc (CALLTARGET) or shape (CONSTRUCT)
};

struct Proto {
    uint8_t maxstacksize = 0, numparams = 0, nups = 0, is_vararg = 0, flags = 0;
    std::vector<uint8_t> typeinfo;
    std::vector<uint32_t> code;      // raw instruction words (op byte in stored numbering)
    std::vector<Constant> k;
    std::vector<uint32_t> children;  // child proto ids
    uint32_t linedefined = 0;
    uint32_t debugnameIdx = 0;
    bool hasLineInfo = false;
    uint8_t linegaplog2 = 0;
    std::vector<uint8_t> lineinfo;   // per-instruction line deltas
    std::vector<int32_t> abslineinfo;
    bool hasDebugInfo = false;
    std::vector<LocVar> locvars;
    std::vector<uint32_t> upvalueNames; // string indices
    std::vector<FeedbackSlot> feedback;
    uint64_t cost = 0;
};

struct Module {
    uint8_t version = 0, typesversion = 0;
    std::vector<std::string> strings;
    std::vector<Proto> protos;
    uint32_t mainProto = 0;

    // NOTE: string references in the stream are 1-BASED (reference loader:
    // `id == 0 ? NULL : strings[id - 1]`); 0 means "no string".
    const std::string& str(uint32_t fileIdx) const {
        static const std::string kEmpty;
        return fileIdx == 0 || fileIdx > strings.size() ? kEmpty : strings[fileIdx - 1];
    }
};

// Parse error: message + byte offset where it stopped.
struct ParseError : std::runtime_error {
    size_t offset;
    ParseError(const std::string& msg, size_t off) : std::runtime_error(msg), offset(off) {}
};

Module parse(const uint8_t* data, size_t size);

// Byte offset of every instruction HEADER word in a serialized LBC blob
// (AUX words excluded). `openc` is the numbering currently stored in the
// blob's opcode bytes. Used for opcode-numbering conversion and audit.
std::vector<size_t> codeWordOffsets(const uint8_t* data, size_t size,
                                    OpEnc openc = OpEnc::Canon);

// Rewrite the opcode byte of every instruction HEADER word from one
// numbering to another. AUX words are never touched. This mirrors the
// canonical<->wire transform used by the Roblox container pipeline; the
// in-binary wire->vmop rewrite happens inside luau_load, not here.
void convertOps(uint8_t* data, size_t size, OpEnc from, OpEnc to);

// ---------------------------------------------------------------------------
// Pretty printer: dumps the module with constant-aware disassembly of every
// proto. `openc` selects the numbering of the stored op bytes (wire for
// Roblox containers, canon for luau-compile output).
// ---------------------------------------------------------------------------
std::string dump(const Module& m, OpEnc openc);

} // namespace lbc
} // namespace rbxvm
