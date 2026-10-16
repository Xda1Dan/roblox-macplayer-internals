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
| Lua-start, leave-game, deeplink, focus, and web-message behavior | Function bodies at the cited addresses |
| Job submit (build `0x10521311e`, dispatch `0x104f54dc6`, enqueue `0x104f54c96`), ring insert (`0x104f54876`), wake (`0x102ee57de`), start/stop (`0x102eec6da`, `0x102eec76c`), node allocator (`0x102ee3136`), worker pop/dispatch (`0x102eeac76`, `0x102ee3332`) |
| Singletons, session accessor, guards | Bodies at the cited addresses |
| Service roster, facet and method names, Luau markers, log lines | String table (883k entries), 761 log channels |
| Hidden engine symbols | Shipped symtab: 2275 entries, no Lua, DataModel, or net code symbols; mostly standard-library type names |
| Opcode numbering, pointer encodings, VM object layouts | Binary tables `T` (`0x1063bb686`) / `R` (`0x10648bb74`) and the decoder code `luaF_freeproto` (`0x102996918`), `Proto.source` decode (`0x102993f3c`) — see the [Luau VM encodings](luau-vm-encodings.md) page |
| Interpreter handlers (CALL/RETURN/NAMECALL/GETIMPORT/FASTCALL/...), TValue layout, renumbered type tags, inline caches | Handler disassembly — see [Inside the Luau interpreter](luau-interpreter.md); the builtins table `0x1072d74e0` was cross-checked against compiled one-liners from the reference compiler |
| Script containers (RSB1): scramble, key recovery, XXH32 checksum, zstd payload | `FUN_1014b91b5` decode path, `FUN_103222533` checksum (its machine code was extracted and executed against the reproduction across a 1000+-case sweep) |
| ScriptContext resume guard, admission chain, `require` state machine | `FUN_101556cb4`, `sub_10159502e`, `FUN_1015c4980`, `FUN_10144ef5e`, `sub_10158dd88` — see `research/vm-research/scriptcontext.md` |
| RakNet datagram encryption (ChaCha20-Poly1305, custom framing) | `FUN_102d94458` (decrypt), `FUN_102d93dba`/`FUN_10293f84` (encrypt), primitives `FUN_101825740`/`FUN_10182539e` — see `research/vm-research/networking.md` |
| LBC serialized-chunk format | Parsed and cross-validated against the reference `luau_load` (the reproduction's chunk reader reproduces its output exactly) |
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
- The important opcode bodies (CALL/RETURN/NAMECALL/GETTABLEKS/GETIMPORT/
  FASTCALL/FORNLOOP and the entry/dispatch machinery) are now covered in
  [Inside the Luau interpreter](luau-interpreter.md); the remaining handlers
  (SETLIST/CONCAT/string builders, vector arithmetic, the debug interpreter
  Path A) are not yet handler-by-handler.
- The script-container wire format (RSB1) and the datagram encryption are
  now documented (see the research notes); the *replication* message
  payloads (the per-message field codecs behind ids `0x81/0x83/0x86`) and
  the RakNet datagram header bit layout are still not claimed.
- Helper names behind internal calls and per-build tunable values remain
  unmapped.
