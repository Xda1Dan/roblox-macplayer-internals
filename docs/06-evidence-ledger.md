# 06 — Provenance + limits

## Target

`…/RobloxPlayer.app/Contents/MacOS/RobloxPlayer`, 120 MB,
sha256 `ac9ad59b3aa3805bf2d1c89fb6ff9c7941bf2e57af17a81cbc26abe94a022c47`.

## What backs each claim

| Claim | Location |
|---|---|
| Mach-O layout, entry, libs, RPATHs | Mach-O headers (54 load commands), section table |
| 63 ObjC classes + IMPs | `__objc_classlist` (`0x10747bab8`, 0x1f8) |
| `appLuaWillStart…` / `ugcGame…` / `leaveGame…` / `handleDeeplinkUrlV2` / `notifyLua*` / `didReceiveScriptMessage` behavior | Analyzed function bodies at the cited `ram:` addresses |
| Job submit/enqueue/wake mechanics | Analyzed `0x10521311e/0x104f54dc6/0x104f54c96/0x104f54876/0x102ee57de` |
| Singletons, session accessor, guards | Analyzed bodies at the cited addresses |
| Service roster, facet/method names, Luau markers, log channels (761) | String table (883k entries) |
| RBX symbols hidden | Shipped symtab: 2275 entries, 0 Lua/DataModel/net code symbols; only std/boost typeinfo |
| Luau VM mapping table | Luau OSS `master` (`lobject.h/lstate.h/lvm*.cpp/Compiler/Require/lgc`) — reference, not binary fact |
| Service membership of DataModel | Binary names + Roblox public API docs (labeled where used) |

## Limits (unknowns, not gaps in effort)

* Only targeted single-function analysis was practical on this binary
  (120 MB of code); no whole-program call graph or cross-reference database
  exists, so inner-replicator addresses and the `ScriptContext` creation
  call-site stay unmapped.
* `&DAT_107510308` identity (factory vs manager) and GC tunables: unobserved.
* Struct offsets (e.g. `+0x778`) are observations of this build only and shift
  between versions.
