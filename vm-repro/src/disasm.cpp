// disasm.cpp — bytecode disassembler for the RobloxPlayer (macOS) build
// 0.742.0.7421053 wire/vmop/canonical instruction streams.
//
// Operand semantics follow the reference tree's Bytecode.h and its
// BytecodeBuilder::dumpInstruction; jump targets are resolved to absolute
// instruction indices (target = i + 1 + offset, AUX word skipped for two-word
// jumps — verified against lvmexecute.cpp).
//
// Constant references print as K<n>: resolving them needs the Proto constant
// table, which comes with the full serialized-chunk reader (phase 2, lands
// with the RSB1/container work).
#include "rbxvm/disasm.hpp"

#include <cstdio>
#include <string>

#include "rbxvm/opcode.hpp"
#include "rbxvm/insn.hpp"
#include "rbxvm/scramble.hpp"

namespace rbxvm {

namespace {

// formats a jump target as "-> N"
void appendTarget(std::string& out, long index, int32_t offset) {
    char buf[32];
    std::snprintf(buf, sizeof buf, " -> %ld", jumpTarget(index, offset));
    out += buf;
}

void appendK(std::string& out, long k, const DisasmContext* ctx) {
    char buf[32];
    std::snprintf(buf, sizeof buf, " K%ld", k);
    out += buf;
    if (ctx && ctx->kcomment) ctx->kcomment(k, out);
}

const char* captureTypeName(uint8_t t) {
    switch (t) {
    case 0: return "VAL";
    case 1: return "REF";
    case 2: return "UPVAL";
    default: return "?";
    }
}

} // namespace

const char* toString(OpEnc e) {
    switch (e) {
    case OpEnc::Wire: return "wire";
    case OpEnc::Vmop: return "vmop";
    case OpEnc::Canon: return "canon";
    }
    return "?";
}

// Decodes one instruction starting at code[index]. Returns the word count
// consumed (1 or 2; 0 on an invalid opcode). Appends one line to out.
unsigned disassembleOne(const uint32_t* code, size_t count, size_t index,
                        OpEnc openc, std::string& out) {
    return disassembleOne(code, count, index, openc, nullptr, out);
}

unsigned disassembleOne(const uint32_t* code, size_t count, size_t index,
                        OpEnc openc, const DisasmContext* ctx, std::string& out) {
    char buf[96];
    if (index >= count) return 0;

    uint32_t insn = code[index];
    uint8_t rawop = insnOp(insn);

    uint8_t canon = 0;
    switch (openc) {
    case OpEnc::Wire: canon = vmopToCanon(wireToVmop(rawop)); break;
    case OpEnc::Vmop: canon = vmopToCanon(rawop); break;
    case OpEnc::Canon: canon = rawop; break;
    }

    // Unknown byte in the chosen numbering: report and stop cleanly.
    if (canon >= kOpcodeCount) {
        std::snprintf(buf, sizeof buf, "%04zu  <bad op byte 0x%02x (%s)>\n",
                      index, rawop, toString(openc));
        out += buf;
        return 0;
    }

    uint8_t a = insnA(insn), b = insnB(insn), c = insnC(insn);
    int32_t d = insnD(insn), e = insnE(insn);
    uint32_t aux = (index + 1 < count) ? code[index + 1] : 0;
    long i = long(index);

    switch (canon) {
    case LOP_NOP: std::snprintf(buf, sizeof buf, "NOP"); out += buf; break;
    case LOP_BREAK: std::snprintf(buf, sizeof buf, "BREAK"); out += buf; break;
    case LOP_LOADNIL: std::snprintf(buf, sizeof buf, "LOADNIL R%u", a); out += buf; break;
    case LOP_LOADB:
        // C is a relative skip count, not a resolved label (upstream prints +C).
        if (c)
            std::snprintf(buf, sizeof buf, "LOADB R%u %u +%u", a, b, c);
        else
            std::snprintf(buf, sizeof buf, "LOADB R%u %u", a, b);
        out += buf;
        break;
    case LOP_LOADN: std::snprintf(buf, sizeof buf, "LOADN R%u %d", a, d); out += buf; break;
    case LOP_LOADK:
        std::snprintf(buf, sizeof buf, "LOADK R%u", a);
        out += buf; appendK(out, d, ctx);
        break;
    case LOP_MOVE: std::snprintf(buf, sizeof buf, "MOVE R%u R%u", a, b); out += buf; break;
    case LOP_GETGLOBAL:
        std::snprintf(buf, sizeof buf, "GETGLOBAL R%u", a);
        out += buf; appendK(out, aux, ctx);
        break;
    case LOP_SETGLOBAL:
        std::snprintf(buf, sizeof buf, "SETGLOBAL R%u", a);
        out += buf; appendK(out, aux, ctx);
        break;
    case LOP_GETUPVAL: std::snprintf(buf, sizeof buf, "GETUPVAL R%u %u", a, b); out += buf; break;
    case LOP_SETUPVAL: std::snprintf(buf, sizeof buf, "SETUPVAL R%u %u", a, b); out += buf; break;
    case LOP_CLOSEUPVALS: std::snprintf(buf, sizeof buf, "CLOSEUPVALS R%u", a); out += buf; break;
    case LOP_GETIMPORT: {
        std::snprintf(buf, sizeof buf, "GETIMPORT R%u", a);
        out += buf; appendK(out, d, ctx);
        out += " [";
        unsigned n = auxImportCount(aux);
        for (unsigned j = 0; j < n; ++j) {
            if (j) out += '.';
            std::snprintf(buf, sizeof buf, "K%u", auxImportIndex(aux, j));
            out += buf;
        }
        out += ']';
        break;
    }
    case LOP_GETTABLE: std::snprintf(buf, sizeof buf, "GETTABLE R%u R%u R%u", a, b, c); out += buf; break;
    case LOP_SETTABLE: std::snprintf(buf, sizeof buf, "SETTABLE R%u R%u R%u", a, b, c); out += buf; break;
    case LOP_GETTABLEKS:
        std::snprintf(buf, sizeof buf, "GETTABLEKS R%u R%u", a, b);
        out += buf; appendK(out, aux & 0xffff, ctx);
        break;
    case LOP_SETTABLEKS:
        std::snprintf(buf, sizeof buf, "SETTABLEKS R%u R%u", a, b);
        out += buf; appendK(out, aux & 0xffff, ctx);
        break;
    case LOP_GETTABLEN: std::snprintf(buf, sizeof buf, "GETTABLEN R%u R%u %u", a, b, unsigned(c) + 1); out += buf; break;
    case LOP_SETTABLEN: std::snprintf(buf, sizeof buf, "SETTABLEN R%u R%u %u", a, b, unsigned(c) + 1); out += buf; break;
    case LOP_NEWCLOSURE:
        std::snprintf(buf, sizeof buf, "NEWCLOSURE R%u proto=%u", a, unsigned(d) & 0xffff);
        out += buf;
        if (ctx && ctx->child) {
            std::string n = ctx->child(uint32_t(d) & 0xffff);
            if (!n.empty()) { out += " ; "; out += n; }
        }
        break;
    case LOP_NAMECALL:
        std::snprintf(buf, sizeof buf, "NAMECALL R%u R%u", a, b);
        out += buf; appendK(out, aux & 0xffff, ctx);
        break;
    case LOP_CALL:
        std::snprintf(buf, sizeof buf, "CALL R%u %d %d", a, b - 1, c - 1);
        out += buf;
        break;
    case LOP_RETURN:
        std::snprintf(buf, sizeof buf, "RETURN R%u %d", a, b - 1);
        out += buf;
        break;
    case LOP_JUMP:
        out += "JUMP";
        appendTarget(out, i, d);
        break;
    case LOP_JUMPBACK:
        out += "JUMPBACK";
        appendTarget(out, i, d);
        break;
    case LOP_JUMPIF:
        std::snprintf(buf, sizeof buf, "JUMPIF R%u", a);
        out += buf; appendTarget(out, i, d);
        break;
    case LOP_JUMPIFNOT:
        std::snprintf(buf, sizeof buf, "JUMPIFNOT R%u", a);
        out += buf; appendTarget(out, i, d);
        break;
    case LOP_JUMPIFEQ: case LOP_JUMPIFLE: case LOP_JUMPIFLT:
    case LOP_JUMPIFNOTEQ: case LOP_JUMPIFNOTLE: case LOP_JUMPIFNOTLT: {
        // AUX (whole word) = second source register; D = jump offset
        std::snprintf(buf, sizeof buf, "%s R%u R%u", kOpInfos[canon].name, a, aux);
        out += buf; appendTarget(out, i, d);
        break;
    }
    case LOP_ADD: case LOP_SUB: case LOP_MUL: case LOP_DIV:
    case LOP_MOD: case LOP_POW: case LOP_IDIV: case LOP_AND: case LOP_OR:
        std::snprintf(buf, sizeof buf, "%s R%u R%u R%u", kOpInfos[canon].name, a, b, c);
        out += buf;
        break;
    case LOP_ADDK: case LOP_SUBK: case LOP_MULK: case LOP_DIVK:
    case LOP_MODK: case LOP_POWK: case LOP_IDIVK: case LOP_ANDK: case LOP_ORK:
        std::snprintf(buf, sizeof buf, "%s R%u R%u", kOpInfos[canon].name, a, b);
        out += buf; appendK(out, c, ctx);
        break;
    case LOP_SUBRK: case LOP_DIVRK:
        std::snprintf(buf, sizeof buf, "%s R%u K%u R%u", kOpInfos[canon].name, a, b, c);
        out += buf;
        if (ctx && ctx->kcomment) ctx->kcomment(b, out);
        break;
    case LOP_CONCAT:
        std::snprintf(buf, sizeof buf, "CONCAT R%u R%u..R%u", a, b, c);
        out += buf;
        break;
    case LOP_NOT: case LOP_MINUS: case LOP_LENGTH:
        std::snprintf(buf, sizeof buf, "%s R%u R%u", kOpInfos[canon].name, a, b);
        out += buf;
        break;
    case LOP_NEWTABLE: {
        // B encodes the array size as 0 or 1<<(B-1); AUX is the hash size.
        unsigned asize = b == 0 ? 0u : (1u << (b - 1));
        std::snprintf(buf, sizeof buf, "NEWTABLE R%u array=%u hash=%u", a, asize, aux);
        out += buf;
        break;
    }
    case LOP_DUPTABLE:
        std::snprintf(buf, sizeof buf, "DUPTABLE R%u", a);
        out += buf; appendK(out, d, ctx);
        break;
    case LOP_SETLIST:
        // SETLIST A B C AUX: B = table register, C-1 = value count, AUX = start index.
        std::snprintf(buf, sizeof buf, "SETLIST R%u R%u count=%d start=%u", a, b, c - 1, aux);
        out += buf;
        break;
    case LOP_FORNPREP: case LOP_FORNLOOP: case LOP_FORGPREP:
    case LOP_FORGPREP_INEXT: case LOP_FORGPREP_NEXT:
        std::snprintf(buf, sizeof buf, "%s R%u", kOpInfos[canon].name, a);
        out += buf; appendTarget(out, i, d);
        break;
    case LOP_FORGLOOP: {
        // Upstream: low byte = variable count; signed AUX < 0 marks inext.
        std::snprintf(buf, sizeof buf, "FORGLOOP R%u vars=%u%s", a, aux & 0xff,
                      int32_t(aux) < 0 ? " [inext]" : "");
        out += buf; appendTarget(out, i, d);
        break;
    }
    case LOP_FASTCALL3:
        std::snprintf(buf, sizeof buf, "FASTCALL3 id=%u R%u R%u R%u", a, b, auxA(aux), auxB(aux));
        out += buf; appendTarget(out, i, c);
        break;
    case LOP_NATIVECALL: out += "NATIVECALL"; break;
    case LOP_GETVARARGS:
        std::snprintf(buf, sizeof buf, "GETVARARGS R%u %d", a, b - 1);
        out += buf;
        break;
    case LOP_DUPCLOSURE:
        std::snprintf(buf, sizeof buf, "DUPCLOSURE R%u", a);
        out += buf; appendK(out, d, ctx);
        break;
    case LOP_PREPVARARGS:
        std::snprintf(buf, sizeof buf, "PREPVARARGS %u", a);
        out += buf;
        break;
    case LOP_LOADKX:
        std::snprintf(buf, sizeof buf, "LOADKX R%u", a);
        out += buf; appendK(out, aux, ctx);
        break;
    case LOP_JUMPX:
        out += "JUMPX";
        appendTarget(out, i, e);
        break;
    case LOP_FASTCALL:
        std::snprintf(buf, sizeof buf, "FASTCALL id=%u", a);
        out += buf; appendTarget(out, i, c);
        break;
    case LOP_COVERAGE:
        out += "COVERAGE";
        break;
    case LOP_CAPTURE:
        // VAL captures a register, REF captures a register by reference,
        // UPVAL captures an upvalue (B indexes U for UPVAL).
        std::snprintf(buf, sizeof buf, "CAPTURE %s %c%u", captureTypeName(a),
                      a == 2 ? 'U' : 'R', b);
        out += buf;
        break;
    case LOP_FASTCALL1:
        std::snprintf(buf, sizeof buf, "FASTCALL1 id=%u R%u", a, b);
        out += buf; appendTarget(out, i, c);
        break;
    case LOP_FASTCALL2:
        std::snprintf(buf, sizeof buf, "FASTCALL2 id=%u R%u R%u", a, b, auxA(aux));
        out += buf; appendTarget(out, i, c);
        break;
    case LOP_FASTCALL2K:
        std::snprintf(buf, sizeof buf, "FASTCALL2K id=%u R%u", a, b);
        out += buf; appendK(out, aux, ctx);
        appendTarget(out, i, c);
        break;
    case LOP_JUMPXEQKNIL: {
        std::snprintf(buf, sizeof buf, "%s R%u%s", kOpInfos[canon].name, a,
                      (aux >> 31) ? " NOT" : "");
        out += buf; appendTarget(out, i, d);
        break;
    }
    case LOP_JUMPXEQKB: {
        std::snprintf(buf, sizeof buf, "%s R%u %u%s", kOpInfos[canon].name, a, aux & 1,
                      (aux >> 31) ? " NOT" : "");
        out += buf; appendTarget(out, i, d);
        break;
    }
    case LOP_JUMPXEQKN: case LOP_JUMPXEQKS: {
        std::snprintf(buf, sizeof buf, "%s R%u", kOpInfos[canon].name, a);
        out += buf; appendK(out, long(auxK24(aux)), ctx);
        if (auxNotFlag(aux)) out += " NOT";
        appendTarget(out, i, d);
        break;
    }
    case LOP_GETUDATAKS:
        std::snprintf(buf, sizeof buf, "GETUDATAKS R%u R%u slot=%u", a, b, c);
        out += buf; appendK(out, aux & 0xffff, ctx);
        break;
    case LOP_SETUDATAKS:
        std::snprintf(buf, sizeof buf, "SETUDATAKS R%u R%u slot=%u", a, b, c);
        out += buf; appendK(out, aux & 0xffff, ctx);
        break;
    case LOP_NAMECALLUDATA:
        std::snprintf(buf, sizeof buf, "NAMECALLUDATA R%u R%u slot=%u", a, b, c);
        out += buf; appendK(out, aux & 0xffff, ctx);
        break;
    case LOP_NEWCLASSMEMBER:
        std::snprintf(buf, sizeof buf, "NEWCLASSMEMBER R%u R%u", a, c);
        out += buf; appendK(out, aux, ctx);
        break;
    case LOP_CALLFB:
        std::snprintf(buf, sizeof buf, "CALLFB R%u %d %d fb=%u", a, b - 1, c - 1, aux);
        out += buf;
        break;
    case LOP_CMPPROTO:
        std::snprintf(buf, sizeof buf, "CMPPROTO R%u proto=%u", a, aux);
        out += buf; appendTarget(out, i, d);
        break;
    case LOP_FASTPCALL:
        // A selects the builtin: 0 = pcall, nonzero = xpcall.
        std::snprintf(buf, sizeof buf, "FASTPCALL %s", a == 0 ? "pcall" : "xpcall");
        out += buf; appendTarget(out, i, c);
        break;
    case LOP_NEWCLASS:
        if (b == 0xff)
            std::snprintf(buf, sizeof buf, "NEWCLASS R%u no_base", a);
        else
            std::snprintf(buf, sizeof buf, "NEWCLASS R%u R%u", a, b);
        out += buf; appendK(out, aux, ctx);
        std::snprintf(buf, sizeof buf, " flags=%u", c);
        out += buf;
        break;
    case LOP_CONSTRUCT:
        std::snprintf(buf, sizeof buf, "CONSTRUCT R%u R%u slot=%u", a, b, aux);
        out += buf;
        break;
    case LOP_FINCONSTRUCT:
        std::snprintf(buf, sizeof buf, "FINCONSTRUCT R%u", a);
        out += buf; appendTarget(out, i, c);
        break;
    default:
        std::snprintf(buf, sizeof buf, "<unhandled canonical %u>", canon);
        out += buf;
        return 0;
    }

    out += '\n';
    return getOpLength(canon);
}

long disassemble(const uint32_t* code, size_t count, OpEnc openc,
                 long baseIndex, std::string& out) {
    return disassemble(code, count, openc, baseIndex, nullptr, out);
}

long disassemble(const uint32_t* code, size_t count, OpEnc openc,
                 long baseIndex, const DisasmContext* ctx, std::string& out) {
    long lines = 0;
    size_t i = 0;
    char buf[48];
    while (i < count) {
        std::snprintf(buf, sizeof buf, "%04ld  ", baseIndex + long(i));
        out += buf;
        unsigned words = disassembleOne(code, count, i, openc, ctx, out);
        if (words == 0) {
            // invalid/unhandled opcode byte: show the raw word and resync at
            // the next word so a single bad byte doesn't derail the dump
            std::snprintf(buf, sizeof buf, "; raw %08x\n", code[i]);
            out += buf;
            i += 1;
            continue;
        }
        i += words;
        ++lines;
    }
    return lines;
}

} // namespace rbxvm
