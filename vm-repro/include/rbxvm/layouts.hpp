// layouts.hpp — verified VM object layouts of RobloxPlayer (macOS) build
// MacPlayer-version-f9247f9560044102 (Luau 0.742.0.7421053).
//
// All offsets were recovered from decompiled accessors and cross-validated:
// see research/vm-research/luau-vm.md ("Object layouts") for the evidence per field.
// "Enc" refers to the address-relative pointer-encoding schemes of ptrenc.hpp;
// fields without an encoding are plain.
//
// These are *offset tables*, not C struct definitions with binary-compatible
// packing: the binary's objects are only ever accessed through these offsets
// by our tooling, so we expose them as constants instead of fragile packed
// structs. (A packed-struct view can be layered later for direct casting.)
#pragma once

#include <cstdint>
#include <cstddef>
#include "ptrenc.hpp"

namespace rbxvm {
namespace offs {

// ---------------- lua_State (a Lua thread / coroutine) ----------------
// Status codes: 0=OK, 1=YIELD, 6=error (matches ScriptContextResume result
// codes 0=done, 1=yield, 6=error).
namespace lua_State_ {
constexpr size_t tt            = 0x00;
constexpr size_t marked        = 0x01;
constexpr size_t memcat        = 0x02;
constexpr size_t status        = 0x03;
constexpr size_t activememcat  = 0x04;
constexpr size_t isactive      = 0x05;
constexpr size_t singlestep    = 0x06; // u8: Path A (debug) vs Path B selector
constexpr size_t ci            = 0x08; // CallInfo*
constexpr size_t stack_last    = 0x10;
constexpr size_t global        = 0x18; // global_State*
constexpr size_t top           = 0x20;
constexpr size_t base          = 0x28;
constexpr size_t stack         = 0x30; // confirmed via lua_resetthread (0x10299fb71)
constexpr size_t finalizers    = 0x38;
constexpr size_t stacksize     = 0x40; // Enc::Xor; participates in global ptrenckey
constexpr size_t size_ci       = 0x44;
constexpr size_t openupval     = 0x48;
constexpr size_t gclist        = 0x50;
constexpr size_t nCcalls       = 0x58; // u16, C-stack guard limit 200
constexpr size_t baseCcalls    = 0x5A; // u16
constexpr size_t cachedslot    = 0x5C;
constexpr size_t namecall      = 0x60; // cached lua_CFunction for NAMECALL
constexpr size_t gt            =0x68;  // globals table
constexpr size_t end_ci        = 0x70;
constexpr size_t base_ci       = 0x78; // confirmed via lua_resetthread
constexpr size_t userdata      = 0x80;
} // namespace lua_State_

// ScriptContext per-thread status byte observed at (thread + 0xac) by the
// scheduler layer (structures.md); NOT part of lua_State above — the Roblox
// scheduler keeps its own bookkeeping adjacent to the VM object.
namespace thread_status {
constexpr size_t offset  = 0xac;
constexpr uint8_t Running  = 0;
constexpr uint8_t Yielded  = 1;
constexpr uint8_t Deferred = 2;
constexpr uint8_t Waiting  = 3;
} // namespace thread_status

// ---------------- CallInfo (one stacked call; stride 0x30) ----------------
// NOTE: unlike upstream (base, func, top, ...), this build stores the Proto
// pointer at +0x10 and the called-function StkId at +0x18 — verified from the
// interpreter entry register sequence (cl = **(ci+0x18), p = *(ci+0x10)).
namespace CallInfo_ {
constexpr size_t base     = 0x00;
constexpr size_t top      = 0x08; // = L->top + LUA_MINSTACK (0x140) at call setup
constexpr size_t p        = 0x10; // Proto* of the executing function
constexpr size_t func     = 0x18; // StkId (TValue* of the called function)
constexpr size_t savedpc  = 0x20; // union savedpc / errfunc
constexpr size_t nresults = 0x28; // (whether extra state is packed here: unconfirmed)
constexpr size_t flags    = 0x2C;
constexpr size_t stride   = 0x30;

// flags bits (upstream LuaCallInfo flags)
constexpr uint32_t flagReturn  = 1;
constexpr uint32_t flagHandle  = 2; // tested by lua_resumeerror
constexpr uint32_t flagNative  = 4; // tested at interpreter entry (NATIVE)
constexpr uint32_t flagOpYield = 8;
constexpr uint32_t flagPcall   = 16;
} // namespace CallInfo_

// NOTE on the two pointer fields: the findings table lists ci+0x10 as `p`
// (Proto*) and ci+0x18 as `func` (StkId). The interpreter entry sequence reads
// cl = **(ci+0x18) and p = *(ci+0x10).

// ---------------- Proto (one compiled function; size 0xD8) ----------------
namespace Proto_ {
constexpr size_t tt              = 0x00; // tt/marked/memcat packed bytes
constexpr size_t nups            = 0x03;
constexpr size_t flags           = 0x04;
constexpr size_t numparams       = 0x05;
constexpr size_t maxstacksize    = 0x06;
constexpr size_t is_vararg       = 0x07;
constexpr size_t locvars         = 0x08;  // Enc::Add    (VMValue4)
constexpr size_t lineinfo        = 0x10;  // Enc::RevSub (VMValue2)
constexpr size_t abslineinfo     = 0x18;  // Enc::Sub    (VMValue1)
constexpr size_t k               = 0x20;  // plain — hot path
constexpr size_t code            = 0x28;  // plain — hot path (uint32_t* instructions)
constexpr size_t source          = 0x30;  // Enc::RevSub (VMValue2) -> TString
constexpr size_t gclist          = 0x38;
constexpr size_t userdata        = 0x40;  // Enc::Xor    (VMValue3)
constexpr size_t p               = 0x48;  // plain — hot path (Proto* children)
constexpr size_t debugname       = 0x50;  // Enc::RevSub (VMValue2) -> TString
constexpr size_t upvalues        = 0x58;  // Enc::Xor    (VMValue3)
constexpr size_t typeinfo        = 0x60;  // Enc::Add    (VMValue4)
constexpr size_t execdata         = 0x68;
constexpr size_t exectarget      = 0x70;
constexpr size_t codeentry       = 0x78;
constexpr size_t debuginsn       = 0x80;  // Enc::RevSub (VMValue2) (uint8_t* per-insn op)
constexpr size_t linedefined     = 0x88;
constexpr size_t sizetypeinfo    = 0x8C;
constexpr size_t sizeupvalues    = 0x90;
constexpr size_t linegaplog2     = 0x94;
constexpr size_t sizelocvars     = 0x98;
constexpr size_t bytecodeid      = 0x9C;
constexpr size_t sizep           = 0xA0;
constexpr size_t sizelineinfo    = 0xA4;
constexpr size_t sizecode        = 0xA8;
constexpr size_t sizek           = 0xAC;
constexpr size_t feedbackvec     = 0xB0;
constexpr size_t feedbackvecsize = 0xB8;
constexpr size_t funid           = 0xBC;
constexpr size_t optimized       = 0xC0;
constexpr size_t deoptimized     = 0xC8;
constexpr size_t cost            = 0xD0;
constexpr size_t total_size      = 0xD8;
} // namespace Proto_

// ---------------- Closure (Lua or C) ----------------
namespace Closure_ {
constexpr size_t tt           = 0x00; // tt/marked/memcat
constexpr size_t isC          = 0x03; // 0 = Lua, 1 = C
constexpr size_t preload      = 0x04;
constexpr size_t stacksize    = 0x05;
constexpr size_t nupvalues    = 0x06;
constexpr size_t gclist       = 0x08;
constexpr size_t env          = 0x10;
constexpr size_t p_or_f       = 0x18; // Lua: Proto* p (plain) | C: lua_CFunction f
constexpr size_t upvalue0_or_cont = 0x20; // Lua: first upvalue TValue | C: cont (Enc::RevSub)
constexpr size_t c_debugname  = 0x28; // C only
constexpr size_t c_upvalue0   = 0x30; // C only: first upvalue TValue
// allocation: Lua nupvalues*16 + 0x20; C nupvalues*16 + 0x30
} // namespace Closure_

// ---------------- TString ----------------
namespace TString_ {
constexpr size_t tt        = 0x00; // tt/marked/memcat
constexpr size_t trap      = 0x04; // i16
constexpr size_t atom      = 0x06; // i16
constexpr size_t next      = 0x08; // hash-chain link
constexpr size_t hash      = 0x10; // Enc::Add (VMValue4); luaS_newlstr stores hash - (ts+0x10)
constexpr size_t len       = 0x14; // u32
constexpr size_t data      = 0x18; // char data[]
constexpr size_t alloc     = 0x19; // allocation size = len + 0x19
} // namespace TString_

// ---------------- Table (size 0x30) ----------------
namespace Table_ {
constexpr size_t tt        = 0x00; // tt/marked/memcat
constexpr size_t tmcache   = 0x03;
constexpr size_t readonly  = 0x04;
constexpr size_t lsizenode = 0x05;
constexpr size_t nodemask8 = 0x06;
constexpr size_t safeenv   = 0x07;
constexpr size_t sizearray = 0x08;
constexpr size_t lastfree  = 0x0C; // union lastfree / aboundary
constexpr size_t node      = 0x10; // initially luaH_dummynode (VA 0x1063bb640)
constexpr size_t array     = 0x18;
constexpr size_t metatable = 0x20;
constexpr size_t gclist    = 0x28;
constexpr size_t total_size = 0x30;
} // namespace Table_

// ---------------- Udata (Lua userdata; size 0x10 + payload) ----------------
namespace Udata_ {
constexpr size_t tt          = 0x00; // tt/marked/memcat
constexpr size_t tag         = 0x03; // u8 userdata tag
constexpr size_t len         = 0x04; // u32 payload length
constexpr size_t metatable   = 0x08; // Enc::Add (VMValue4): real = stored + (u+8)
constexpr size_t data        = 0x10; // payload follows the header
// Evidence: the builtin fast-path impl at 0x10298f40a (getmetatable-shaped)
// reads, per type: tag 7 (table) -> [t+0x20]; tag 9 (userdata) ->
// LEA r,[u+8]; ADD r,[u+8]; everything else -> global->mt[tag]
// (global+0x328+tag*8). luaM_newgco sizes len + 0x10.
} // namespace Udata_

} // namespace offs

// Compile-time sanity anchors for the offset tables (self-checks, not claims
// about sizeof of any real struct).
static_assert(offs::Proto_::total_size == 0xD8, "Proto size per findings");
static_assert(offs::Table_::total_size == 0x30, "Table size per findings");
static_assert(offs::CallInfo_::stride == 0x30, "CallInfo stride per findings");
// transcription guards for fields whose values were corrected during research
static_assert(offs::lua_State_::stack == 0x30, "lua_State.stack at +0x30 (not 0x78)");
static_assert(offs::lua_State_::base_ci == 0x78, "lua_State.base_ci at +0x78 (not 0x70)");
static_assert(offs::lua_State_::nCcalls == 0x58, "nCcalls at +0x58");

} // namespace rbxvm
