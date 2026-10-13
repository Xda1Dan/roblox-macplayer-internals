# 06 — Evidence ledger + method limits

## Target

`…/RobloxPlayer.app/Contents/MacOS/RobloxPlayer`, 120 MB,
sha256 `ac9ad59b3aa3805bf2d1c89fb6ff9c7941bf2e57af17a81cbc26abe94a022c47`.

## What backs each claim

| Claim | Evidence |
|---|---|
| Mach-O layout, entry, libs, RPATHs | Python `LC_*` parse (54 cmds), `InfoDump.java` blocks |
| 63 ObjC classes + IMPs | `ObjCDump.java` on `__objc_classlist` (`0x10747bab8`, 0x1f8) |
| `appLuaWillStart…` / `ugcGame…` / `leaveGame…` / `handleDeeplinkUrlV2` / `notifyLua*` / `didReceiveScriptMessage` behavior | Decompiled bodies (`decomp_*.log` in research notes), sizes + callees cited |
| Job submit/enqueue/wake mechanics | Decompiled `0x10521311e/0x104f54dc6/0x104f54c96/0x104f54876/0x102ee57de` |
| Singletons (`0x105235e96`, `0x10520e072`, `0x10118fab6`), session accessor (`0x1051e5369`) | Decompiled bodies |
| Seqlock global (`0x10003ddee`), SSO assign (`0x102d49d80`) | Decompiled bodies |
| Service roster, facet/method names, Luau counts, FLog channels (761) | `strings` census (883k strings) |
| RBX symbols hidden | `LC_SYMTAB`: 2275 entries, 0 Lua/DataModel/net code symbols; only std/boost typeinfo |
| Mangled-name metadata region (`0x6230a47`) is labels, not pointers | Neighbor inspection (alignment + short scope names) |
| Luau VM mapping table | Luau OSS `master` (`lobject.h/lstate.h/lvm*.cpp/Compiler/Require/lgc`) — reference, not binary fact |
| Service membership of DataModel | Binary names + Roblox public API docs (labeled where used) |

## Limits (unknowns, not gaps in effort)

* Full Ghidra auto-analysis never completed on the host (120 MB `__text`,
  3.9 GB `/tmp` tmpfs, loaded 4-core box; REA runs died on quota/timeout/
  restarts). All decompilation is targeted single-function work — no call-graph
  or cross-reference database exists.
* Inner replicator wire format, `ScriptContext` creation call-site,
  `&DAT_107510308` identity (factory vs manager), GC tunables: unobserved.
* Decompiler output is paraphrased in docs; register-level details
  (exact struct offsets like `+0x778`) are analysis observations of this build
  only and will shift between versions.
