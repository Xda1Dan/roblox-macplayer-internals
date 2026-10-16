// scramble.hpp — the opcode-numbering scrambles of RobloxPlayer (macOS) build
// MacPlayer-version-f9247f9560044102 (Luau 0.742.0.7421053).
//
// Verified scheme (see ../research/vm-research/luau-vm.md "Opcodes: three numberings"):
//
//     canonical --(x227 mod 256)--> wire --(table T)--> vmop
//
//   wire = (227 * canonical) & 0xFF      — what ships on disk / over the network
//   vmop = T[wire]                       — what the interpreter dispatches on
//   canonical = R[vmop]                  — reverse map shipped as data
//
// Note R∘T is the wire->canonical map (wire = 227*canonical), NOT the identity:
// R[T[wire]] == canonical == 227^-1 * wire (mod 256).
//
// Binary ground truth:
//   T : 256-byte permutation at VA 0x1063bb686 (RVA 0x63bb686).
//       NOTE: the community dump's "OpcodeLookupTable = 0x63bb680" is off by 6.
//   R : 256-byte permutation at VA 0x10648bb74 (RVA 0x648bb74).
//
// These tables are NOT the interpreter dispatch tables; the two dispatch jump
// tables (Path A single-step 0x1072d8a30, Path B normal 0x1072d9230) are
// separate 256-entry pointer tables indexed by vmop.
#pragma once

#include <cstdint>

namespace rbxvm {

// Multiplier taking canonical -> wire. Odd, hence a bijection mod 256.
constexpr uint16_t kWireMultiplier = 227;

// Canonical -> wire (the multiplication is its own inverse class: wire = 227*c,
// and because gcd(227,256)=1 the map is invertible, though the VM never needs
// the inverse directly — it uses R after dispatch instead).
constexpr uint8_t canonToWire(uint8_t canonical) {
    return static_cast<uint8_t>(canonical * kWireMultiplier);
}

// Defined in tables.cpp — raw bytes dumped from the binary via Ghidra.
extern const uint8_t kWireToVmop[256]; // T, at VA 0x1063bb686
extern const uint8_t kVmopToCanon[256]; // R, at VA 0x10648bb74

// wire -> vmop (what luau_load at 0x1029aa05a does in place for every byte)
inline uint8_t wireToVmop(uint8_t wire) { return kWireToVmop[wire]; }

// vmop -> canonical (what the interpreter's helpers use, e.g. getOpLength)
inline uint8_t vmopToCanon(uint8_t vmop) { return kVmopToCanon[vmop]; }

// canonical -> vmop (compose both)
inline uint8_t canonToVmop(uint8_t canonical) {
    return kWireToVmop[canonToWire(canonical)];
}

// Full verification of the tables against the documented scheme. Returns the
// number of failed checks (0 = all good). Checks:
//   1. T and R are permutations of 0..255
//   2. R[T[(227*c)&0xFF]] == c for every canonical c in 0..92
//   3. canonical -> vmop values are distinct (93 distinct vmops)
int verifyTables();

} // namespace rbxvm
