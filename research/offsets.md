# Offsets Reference — RobloxPlayer (macOS, Luau 0.742.0.7421053)

Every address is given as an **RVA** (relative virtual address) and the corresponding Ghidra
**VA**: `VA = RVA + 0x100000000`. Binary build: `MacPlayer-version-f9247f9560044102`.

Legend: ✅ confirmed in this binary · ❌ wrong in the community dump · ✏️ corrected here

## Luau VM — ✅ all confirmed

| Symbol | RVA | VA | Notes |
|--------|-----|----|-------|
| `luau_execute` | `0x29adfd3` | `0x1029adfd3` | bytecode interpreter dispatch stub (computed goto into Path A / B) |
| `LuaVMLoad` | `0x29a9f36` | `0x1029a9f36` | chunk loader / protected entry above `luau_load` |
| `luau_load` | `0x29aa05a` | `0x1029aa05a` | builds `Proto` + `Closure`; rewrites wire opcodes to vmop in place |
| `luaD_rawrunprotected` | `0x2995084` | `0x102995084` | pcall / setjmp-style protected boundary |
| `luaD_throw` | `0x2995125` | `0x102995125` | non-local error raise (C++ exception in this build) |
| resume prologue | `0x2995b7f` | `0x102995b7f` | ✏️ thread-status gate + `nCcalls` C-stack guard (limit 200, "C stack overflow"); the dump label `lua_rawcheckstack` is wrong |
| `luaC_step` | `0x2996c06` | `0x102996c06` | GC step (called when the stack is low) |
| `lua_yield` | `0x2992a58` | `0x102992a58` | coroutine yield |
| `lua_newstate` | `0x299fd5b` | `0x10299fd5b` | create Lua state; ✏️ prior VA `0x1029fd5b` dropped a digit |
| `lua_resetthread` | `0x299fb71` | `0x10299fb71` | ✏️ prior VA `0x1029fb71` dropped a digit |
| `luaF_newLclosure` | `0x29965b9` | `0x1029965b9` | closure creation |
| `luaF_newproto` | `0x29964dd` | `0x1029964dd` | proto creation (seeds encoded fields) |
| `luaF_newUpval` | `0x29967b1` | `0x1029967b1` | upvalue creation |
| `luaF_freeproto` | `0x2996918` | `0x102996918` | un-encodes then frees a `Proto` (shows all four VMValue schemes) |
| `luaG_runerror` | `0x29940f4` | `0x1029940f4` | raise formatted error |
| `luaV_gettable` | `0x29abe86` | `0x1029abe86` | |
| `luaV_settable` | `0x29ac376` | `0x1029ac376` | |
| `luaV_equal` | `0x299ebe4` | `0x10299ebe4` | |
| `luaV_lessthan` | `0x29ac854` | `0x1029ac854` | |
| `luaT_objtypename` | `0x29a8ba8` | `0x1029a8ba8` | |
| `luaS_newlstr` | `0x29a06d3` | `0x1029a06d3` | string intern; writes the VMValue4-encoded `TString.hash` |
| `luaopen_base` | `0x298b6dc` | `0x10298b6dc` | base library |
| `luaB_print` | `0x298bdc6` | `0x10298bdc6` | `print` |
| `luaB_pcall` | `0x298b913` | `0x10298b913` | `pcall` |
| `luaB_error` | `0x298bbf3` | `0x10298bbf3` | `error` |

## Opcode dispatch and scrambler tables

| Symbol | RVA | VA | Notes |
|--------|-----|----|-------|
| normal-loop handler table (Path B) | `0x72d9230` | `0x1072d9230` | 256 entries indexed by vmop; handlers in `0x1029b3xxx`–`0x1029b8xxx` |
| single-step handler table (Path A) | `0x72d8a30` | `0x1072d8a30` | 256 entries, separate handler copies |
| wire→vmop permutation `T` | `0x63bb686` | `0x1063bb686` | 256-byte table; ✏️ the dump's `OpcodeLookupTable = 0x63bb680` is off by 6 |
| reverse map `R` (vmop→canonical) | `0x648bb74` | `0x10648bb74` | ships as data |

Note: the scrambler table `T` and the interpreter dispatch tables are different things. Earlier
notes conflated them.

## Roblox engine — ✅ confirmed

| Symbol | RVA | VA | Notes |
|--------|-----|----|-------|
| `ScriptContextResume` | `0x15566d6` | `0x1015566d6` | resume trampoline — exact match with the dump |
| `task_lib_init` | `0x14cebe0` | `0x1014cebe0` | registers the `task.*` luaL_Reg table |
| `task.defer` | `0x14cef92` | `0x1014cef92` | |
| `task.wait` | `0x14cf74a` | `0x1014cf74a` | |
| `task.cancel` | `0x14cf880` | `0x1014cf880` | |
| `task.synchronize` | `0x14cedfe` | `0x1014cedfe` | Parallel Luau lane switch |
| `task.desynchronize` | `0x14ceec6` | `0x1014ceec6` | Parallel Luau lane switch |
| **`task.spawn`** | **`0x14cf416`** | `0x1014cf416` | ❌ dump says `0x14cff67` (wrong) |
| **`task.delay`** | **`0x14cf56a`** | `0x1014cf56a` | ❌ dump says `0x14cf880` (wrong — that is `task.cancel`) |
| `Print` | `0x2ce6789` | `0x102ce6789` | in `__text` range |

## Task library → implementation

| Lua name | Implementation (VA) |
|----------|---------------------|
| `synchronize` | `sub_1014cedfe` |
| `desynchronize` | `sub_1014ceec6` |
| `defer` | `sub_1014cef92` |
| `spawn` | `sub_1014cf416` |
| `delay` | `sub_1014cf56a` |
| `wait` | `sub_1014cf74a` |
| `cancel` | `sub_1014cf880` |

## Core scheduler / engine functions (VA)

| Function | Role |
|----------|------|
| `sub_1015566d6` | ScriptContext resume trampoline |
| `sub_101556235` / `sub_1015562c1` | ScriptContext execute wrappers |
| `sub_101556664` | facet accessor (lifecycle ≥ 3 check) |
| `sub_1015c486e` | shutdown-deadline check |
| `sub_10298534b` | C-stack manager |
| `sub_102995af5` | resume + pop return values |
| `sub_102995dc4` | resume + drain yield queue |
| `sub_102ee6a60` | TaskScheduler job profiler |
| `sub_1045455d3` | `GenericJob::Step` |
| `func_0x0001015a8050` | scheduler core — time-ordered queue insert |
| `func_0x0001040d49da` | get scheduler queue (`lane+0xac0`) |
| `func_0x000103209848` | cross-thread futex wake |
| `sub_104c18388` | FVar registry init |

## Previously reported "out of range" — all resolve in this build

An earlier revision marked the dump entries below as ⛔ out of range. They are **not**: every one
is a valid function entry in this binary. The real problem with the dump was mislabeling, not
address range.

| Dump label | RVA | VA | Status |
|------------|-----|----|--------|
| `GetGlobalState` | `0x55d5e74` | `0x1055d5e74` | resolves — but it is a **facet accessor**, not `GetGlobalState` |
| `PushInstance` | `0x1487d49` | `0x101487d49` | resolves |
| `FireLeftMouseClick` | `0x4c75cd4` | `0x104c75cd4` | resolves |
| `FireRightMouseClick` | `0x4c75e36` | `0x104c75e36` | resolves |
| `FireMouseHoverEnter` | `0x4c76a7c` | `0x104c76a7c` | resolves |
| `FireMouseHoverLeave` | `0x4c76b62` | `0x104c76b62` | resolves |
| `FireProximityPrompt` | `0x42ea5f0` | `0x1042ea5f0` | resolves |
| `FireTouchInterest` | `0x10e3030` | `0x1010e3030` | resolves |
| `HandleConnectionState` | `0x54afa04` | `0x1054afa04` | resolves |
| `ProcessNetworkPacket` | `0x2d6df78` | `0x102d6df78` | resolves |
| `ReportNetworkError` | `0x54af5b8` | `0x1054af5b8` | resolves |
| `Send` | `0x2d74f8c` | `0x102d74f8c` | resolves |
