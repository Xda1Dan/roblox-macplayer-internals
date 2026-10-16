// insn.hpp — Luau 32-bit instruction word accessors.
//
// Word code: each instruction is one or more 32-bit words; the low byte of the
// first word is the opcode byte (its numbering depends on where the code came
// from — wire on disk/stream, vmop in memory after luau_load, canonical only
// inside tooling). See Bytecode.h of the reference tree and
// vm-research/luau-vm.md.
//
// Encodings (static per opcode):
//   ABC: op | A<<8 | B<<16 | C<<24
//   AD : op | A<<8 | D<<16        (D is a signed 16-bit half word)
//   E  : op | E<<8                (E is a signed 24-bit value)
// An optional second 32-bit word (AUX) follows for the 31 two-word opcodes.
//
// Jump offsets are in word increments. With the jump at index i:
//   target = i + 1 + offset          (offset in the D/E field)
// which for two-word (AUX) jumps skips over the AUX word; taken-jump targets
// are relative to the instruction *after* the header (verified against the VM:
// lvmexecute.cpp reads AUX without advancing pc, and jumps pc += D from there;
// the not-taken case advances by 1 more to skip AUX).
#pragma once

#include <cstdint>

namespace rbxvm {

inline uint8_t insnOp(uint32_t insn) { return static_cast<uint8_t>(insn & 0xff); }
inline uint8_t insnA(uint32_t insn) { return static_cast<uint8_t>((insn >> 8) & 0xff); }
inline uint8_t insnB(uint32_t insn) { return static_cast<uint8_t>((insn >> 16) & 0xff); }
inline uint8_t insnC(uint32_t insn) { return static_cast<uint8_t>((insn >> 24) & 0xff); }

// D: signed 16-bit (arithmetic shift of the top half word)
inline int32_t insnD(uint32_t insn) { return static_cast<int32_t>(insn) >> 16; }

// E: signed 24-bit (arithmetic shift of the top three bytes)
inline int32_t insnE(uint32_t insn) { return static_cast<int32_t>(insn) >> 8; }

// --- AUX word accessors (second word of two-word instructions) ---
inline uint8_t auxA(uint32_t aux) { return static_cast<uint8_t>(aux & 0xff); }
inline uint8_t auxB(uint32_t aux) { return static_cast<uint8_t>((aux >> 8) & 0xff); }

// K index packed in AUX for FASTCALL2K / JUMPXEQKN / JUMPXEQKS (low 24 bits)
inline uint32_t auxK24(uint32_t aux) { return aux & 0xffffff; }

// NOT flag (flips comparison result) for JUMPXEQK* — high bit
inline uint32_t auxNotFlag(uint32_t aux) { return static_cast<uint32_t>(aux) >> 31; }

// GETIMPORT AUX: length (1..3) in the top 2 bits, then up to three 10-bit
// constant indices of the import path components.
inline unsigned auxImportCount(uint32_t aux) { return aux >> 30; }
inline uint32_t auxImportIndex(uint32_t aux, unsigned j) {
    return (aux >> (20 - 10 * j)) & 0x3ff; // j in 0..2
}

// Instruction encoder helpers (canonical opcode byte — callers choose the wire
// transform explicitly via scramble.hpp).
constexpr uint32_t makeInsnABC(uint8_t op, uint8_t a, uint8_t b, uint8_t c) {
    return uint32_t(op) | (uint32_t(a) << 8) | (uint32_t(b) << 16) | (uint32_t(c) << 24);
}
constexpr uint32_t makeInsnAD(uint8_t op, uint8_t a, int32_t d) {
    return uint32_t(op) | (uint32_t(a) << 8) | (uint32_t(d & 0xffff) << 16);
}
constexpr uint32_t makeInsnE(uint8_t op, int32_t e) {
    return uint32_t(op) | (uint32_t(e & 0xffffff) << 8);
}
constexpr uint32_t makeInsnA(uint8_t op, uint8_t a) { return makeInsnABC(op, a, 0, 0); }

// Jump target for a D/E-offset jump currently at instruction index i.
constexpr long jumpTarget(long index, int32_t offset) { return index + 1 + offset; }

} // namespace rbxvm
