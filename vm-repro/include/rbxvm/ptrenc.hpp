// ptrenc.hpp — the four address-relative pointer-encoding schemes of the
// RobloxPlayer (macOS) Luau VM, build 0.742.0.7421053.
//
// Certain object fields are NOT stored as plain pointers: the stored value is
// scrambled relative to the field's own address (object_base + offset). There
// is no secret key — decoding is one arithmetic op. The community dump calls
// these VMValue1..4 without explaining them.
//
//   Scheme    Decode (real from stored)      Encode (stored from real)
//   Sub       real = stored - field_addr     stored = real + field_addr
//   RevSub    real = field_addr - stored     stored = field_addr - real
//   Xor       real = field_addr ^ stored     stored = field_addr ^ real
//   Add       real = field_addr + stored     stored = real - field_addr
//
// Proof sketch (full version in vm-research/luau-vm.md):
//  - luaF_freeproto (VA 0x102996918) un-encodes every field before freeing and
//    exhibits all four arithmetic forms.
//  - luaF_newproto (VA 0x1029964dd) initializes each encoded field to a
//    self-referential "empty" sentinel; all nine sentinels decode to exactly 0
//    under the schemes above. The "empty sentinel" for scheme e is therefore
//    encode(field_addr, 0, e).
//
// Fields known to use each scheme (offsets in layouts.hpp):
//   Sub    : Proto.abslineinfo
//   RevSub : Proto.source, .lineinfo, .debugname, .debuginsn; Closure.cont (C closures)
//   Xor    : Proto.upvalues, .userdata; lua_State.stacksize
//   Add    : Proto.locvars, .typeinfo; TString.hash; userdata metatable
//
// NOT encoded (plain pointers — covers the whole hot path):
//   Proto.k (+0x20), Proto.code (+0x28), Proto.p (+0x48), Closure.p/f (+0x18).
//
// Separately, global_State carries a keyed pointer-encryption table
// (ptrenckey[4] at global+0x06B8, ptrenckeynew[8] at +0x49F0, ptrencactive at
// +0x4A30); lua_State.stacksize participates in it. The keyed scheme is OUT OF
// SCOPE here — only the keyless self-address-relative schemes are implemented.
#pragma once

#include <cstdint>

namespace rbxvm {

enum class Enc : uint8_t {
    Plain = 0, // stored as-is
    Sub,       // VMValue1: real = stored - field
    RevSub,    // VMValue2: real = field - stored
    Xor,       // VMValue3: real = field ^ stored
    Add,       // VMValue4: real = field + stored
};

// Decode a stored field value into the real pointer/value.
inline uintptr_t ptrDecode(uintptr_t field_addr, uintptr_t stored, Enc e) {
    switch (e) {
    case Enc::Sub: return stored - field_addr;
    case Enc::RevSub: return field_addr - stored;
    case Enc::Xor: return field_addr ^ stored;
    case Enc::Add: return field_addr + stored;
    case Enc::Plain: default: return stored;
    }
}

// Encode a real pointer/value for storage at field_addr.
inline uintptr_t ptrEncode(uintptr_t field_addr, uintptr_t real, Enc e) {
    switch (e) {
    case Enc::Sub: return real + field_addr;
    case Enc::RevSub: return field_addr - real;
    case Enc::Xor: return field_addr ^ real;
    case Enc::Add: return real - field_addr;
    case Enc::Plain: default: return real;
    }
}

// The "empty" sentinel value luaF_newproto stores in an encoded field
// (decodes back to 0 under the scheme).
inline uintptr_t ptrEmptySentinel(uintptr_t field_addr, Enc e) {
    return ptrEncode(field_addr, 0, e);
}

} // namespace rbxvm
