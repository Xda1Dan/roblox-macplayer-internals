# Addresses and limits

In one paragraph: every address in these notes belongs to one tested
build. This page lists where each claim lives and where the notes stop.
Use it before copying an address into another version.

## Target

`…/RobloxPlayer.app/Contents/MacOS/RobloxPlayer`, 120 MB,
SHA-256 `ac9ad59b3aa3805bf2d1c89fb6ff9c7941bf2e57af17a81cbc26abe94a022c47`.

## Where each claim lives

| Claim | Location |
|---|---|
| Mach-O layout, entry, libraries, RPATHs | Mach-O headers (54 load commands), section table |
| 63 app classes and method addresses | `__objc_classlist` (`0x10747bab8`, 0x1f8) |
| Lua-start, leave-game, deeplink, focus, and web-message behavior | Function bodies at the cited `ram:` addresses |
| Job submit, enqueue, wake, ring, start, stop, node pool | Bodies at `0x10521311e`, `0x104f54dc6`, `0x104f54c96`, `0x104f54876`, `0x102ee57de`, `0x102eec6da`, `0x102eec76c`, `0x102ee3136` |
| Singletons, session accessor, guards | Bodies at the cited addresses |
| Service roster, facet and method names, Luau markers, log lines | String table (883k entries), 761 log channels |
| Hidden engine symbols | Shipped symtab: 2275 entries, no Lua, DataModel, or net code symbols; mostly standard-library type names |
| Luau VM mapping | Public Luau source as a reference, not a binary fact |
| Service membership | Binary names plus public Roblox API docs, labeled where used |

## Limits

These are unknowns, not oversights:

- Only focused single-function work was practical on a 120 MB code image.
  There is no whole-program call graph here, so inner replicator addresses
  and the `ScriptContext` creation call site stay unmapped.
- The identity behind `&DAT_107510308` (factory versus manager) and GC
  tunables were not established.
- Struct offsets such as `+0x778` belong to this build only and move
  between versions.
- `luau_execute` (`0x29adfd3`) is a confirmed entry whose optimized body is
  not detailed here.
- Exact wire formats, helper names behind internal calls, and per-build
  tunable values are not claimed.
