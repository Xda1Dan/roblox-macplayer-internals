// loader.hpp — luau_load-equivalent: build the closure tree the binary
// builds, on top of the LBC reader.
//
// Provenance: VM/src/lvmload.cpp of the reference tree, cross-checked against
// the binary's deserializer (0x1029aa05a; see research/vm-research/luau-load.md).
// What this loader mirrors:
//   1. the version/type-version gates (version 0 = "error message blob"),
//   2. the per-proto wire->vmop opcode rewrite (in place, headers only),
//   3. the atom pass: GETTABLEKS/SETTABLEKS/NAMECALL with AUX < 0x10000 are
//      swapped to GETUDATAKS/SETTABLEKS/NAMECALLUDATA when the referenced
//      string constant has an assigned atom (hook at global+0x658),
//   4. proto fields (maxstacksize/numparams/nups/is_vararg/flags/children/
//      linedefined/debugname), line/debug info, feedback slots, and, for
//      version >= 12, the section-size skip.
// What it does not do: GC, constant *evaluation* (IMPORT resolution needs the
// environment), or closure preloading — those live in the embedder.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "rbxvm/lbc.hpp"
#include "rbxvm/opcode.hpp"

namespace rbxvm {
namespace loader {

// Optional atom provider: returns the assigned atom for a string constant
// (>= 0 means assigned), or -1 when unassigned/absent. In the binary this is
// the luaS_updateatom hook at `*(L->global + 0x658)`.
using AtomProvider = std::function<int32_t(const std::string&)>;

struct Proto {
    uint8_t maxstacksize = 0, numparams = 0, nups = 0, is_vararg = 0, flags = 0;
    std::vector<uint8_t> typeinfo;              // raw (v2/v3 bytes)
    std::vector<uint32_t> code;                 // VMOP bytes after load
    std::vector<lbc::Constant> k;
    std::vector<uint32_t> children;             // proto ids
    uint32_t linedefined = 0;
    const std::string* debugname = nullptr;     // points into Module.strings
    bool hasLineInfo = false;
    uint8_t linegaplog2 = 0;
    std::vector<uint8_t> lineinfo;              // per-word deltas
    std::vector<int32_t> abslineinfo;
    bool hasDebugInfo = false;
    std::vector<lbc::LocVar> locvars;
    std::vector<const std::string*> upvalueNames;
    std::vector<lbc::FeedbackSlot> feedback;
    uint64_t cost = 0;
};

struct Module {
    uint8_t version = 0, typesversion = 0;
    std::vector<std::string> strings;           // 1-based map: id -> string
    std::vector<Proto> protos;
    uint32_t mainProto = 0;

    const std::string& str(uint32_t idx) const {
        static const std::string kEmpty;
        return idx == 0 || idx > strings.size() ? kEmpty : strings[idx - 1];
    }
};

struct LoadError : std::runtime_error {
    explicit LoadError(const std::string& msg) : std::runtime_error(msg) {}
};

// Parse + build. `openc` is the numbering of the stored opcode bytes
// (Wire for Roblox containers, Canon for luau-compile output). The result's
// code arrays always carry VMOP bytes, like the binary after luau_load.
Module load(const uint8_t* data, size_t size, OpEnc openc,
            const AtomProvider& atoms = nullptr);

} // namespace loader
} // namespace rbxvm
