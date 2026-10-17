// loader.cpp — luau_load-equivalent construction on top of lbc::parse.
// See include/rbxvm/loader.hpp for scope and provenance.
#include "rbxvm/loader.hpp"

#include "rbxvm/scramble.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace rbxvm {
namespace loader {

namespace {

uint8_t canonicalOf(uint8_t byte, OpEnc openc) {
    switch (openc) {
    case OpEnc::Canon: return byte;
    case OpEnc::Wire: return vmopToCanon(wireToVmop(byte));
    case OpEnc::Vmop: return vmopToCanon(byte);
    }
    return byte;
}

// The atom-pass targets (canonical numbering), per lvmload.cpp:
// GETTABLEKS->GETUDATAKS, SETTABLEKS->SETUDATAKS, NAMECALL->NAMECALLUDATA.
int atomTargetOf(uint8_t canonical) {
    switch (canonical) {
    case LOP_GETTABLEKS: return LOP_GETUDATAKS;
    case LOP_SETTABLEKS: return LOP_SETUDATAKS;
    case LOP_NAMECALL: return LOP_NAMECALLUDATA;
    default: return -1;
    }
}

void buildProto(const lbc::Proto& src, Module& m, OpEnc openc,
                const AtomProvider& atoms, Proto& out) {
    out.maxstacksize = src.maxstacksize;
    out.numparams = src.numparams;
    out.nups = src.nups;
    out.is_vararg = src.is_vararg;
    out.flags = src.flags;
    out.typeinfo = src.typeinfo;
    out.children = src.children;
    out.linedefined = src.linedefined;
    out.debugname = &m.str(src.debugnameIdx);

    // ---- code words: rewrite every header byte to vmop -------------------
    // This is the in-place pass luau_load performs right after the bulk copy
    // (research/vm-research/luau-load.md §3.4): op = T[stored_byte]; the
    // length is decided by getOpLength(R[op]).
    out.code = src.code;
    uint32_t pc = 0;
    while (pc < out.code.size()) {
        const uint8_t canon = canonicalOf(uint8_t(out.code[pc] & 0xff), openc);
        if (canon < kOpcodeCount)
            out.code[pc] = (out.code[pc] & 0xffffff00u) | canonToVmop(canon);
        pc += (canon < kOpcodeCount) ? getOpLength(canon) : 1;
    }

    // ---- constants -------------------------------------------------------
    out.k = src.k;

    // ---- atom pass (after constants, like lvmload.cpp) -------------------
    // Only when AUX < 0x10000 and the constant is a string with an assigned
    // atom. The binary swaps in the atom-specialized opcode byte (vmop).
    if (atoms) {
        pc = 0;
        while (pc < out.code.size()) {
            const uint8_t vmByte = uint8_t(out.code[pc] & 0xff);
            const uint8_t canon = vmopToCanon(vmByte);
            const int target = (canon < kOpcodeCount) ? atomTargetOf(canon) : -1;
            if (target >= 0 && pc + 1 < out.code.size()) {
                const uint32_t aux = out.code[pc + 1];
                if (aux < 0x10000) {
                    static const lbc::Constant kNil{};
                    const lbc::Constant& c = aux < out.k.size() ? out.k[aux] : kNil;
                    if (c.kind == lbc::Constant::Kind::String && atoms(m.str(c.index)) >= 0)
                        out.code[pc] = (out.code[pc] & 0xffffff00u) | canonToVmop(uint8_t(target));
                }
            }
            pc += (canon < kOpcodeCount) ? getOpLength(canon) : 1;
        }
    }

    // ---- line / debug / feedback -----------------------------------------
    out.hasLineInfo = src.hasLineInfo;
    out.linegaplog2 = src.linegaplog2;
    out.lineinfo = src.lineinfo;
    out.abslineinfo = src.abslineinfo;
    out.hasDebugInfo = src.hasDebugInfo;
    out.locvars = src.locvars;
    out.upvalueNames.reserve(src.upvalueNames.size());
    for (uint32_t idx : src.upvalueNames) out.upvalueNames.push_back(&m.str(idx));
    out.feedback = src.feedback;
    out.cost = src.cost;
}

} // namespace

Module load(const uint8_t* data, size_t size, OpEnc openc, const AtomProvider& atoms) {
    // Version gates, mirroring luau_load (research/vm-research/luau-load.md §3.1):
    // version 0 means the rest of the blob is an error message.
    if (size == 0) throw LoadError("empty chunk");
    const uint8_t version = data[0];
    if (version == 0)
        throw LoadError("bytecode carries an error message (version 0)");
    if ((version < 3 || version > 14) && version != 100)
        throw LoadError("bytecode version mismatch");
    if (version >= 4) {
        const uint8_t typesversion = data[1];
        if (typesversion < 1 || typesversion > 3)
            throw LoadError("bytecode type version mismatch (expected [1..3], got " +
                            std::to_string(typesversion) + ")");
    }

    const lbc::Module parsed = lbc::parse(data, size);

    Module m;
    m.version = parsed.version;
    m.typesversion = parsed.typesversion;
    m.strings = parsed.strings;
    m.mainProto = parsed.mainProto;
    m.protos.resize(parsed.protos.size());
    for (size_t i = 0; i < parsed.protos.size(); ++i)
        buildProto(parsed.protos[i], m, openc, atoms, m.protos[i]);
    return m;
}

} // namespace loader
} // namespace rbxvm
