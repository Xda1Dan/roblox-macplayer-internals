# 00 — Binary overview (loader facts)

Target: `MacPlayer-version-f9247f9560044102/RobloxPlayer.app/Contents/MacOS/RobloxPlayer`

| Fact | Value | Source |
|------|-------|--------|
| Format | Mach-O 64-bit LE (`0xfeedfacf`), `MH_EXECUTE`, x86_64 (`cputype 0x01000007`), PIE | header parse |
| Entry | `entryoff 0x41fe2` → `ram:0x100041fe2` | `LC_MAIN` |
| `__TEXT` | `vm 0x100000000 + 0x6e64000` (≈110 MB), 13 sections; `__text` itself `0x5bb9676` (≈96 MB code) | segment + section headers |
| `__DATA` | `vm 0x106e64000`, `__const/__cfstring/__objc_*` + 25 sections | same |
| Architecture | `x86:LE:64`, compiler `gcc`, imagebase `0x100000000` | program metadata |
| Symbol count (unanalyzed import) | 3923 total | symbol table |
| Symtab | 2275 entries: 668 defined (guard vars, typeinfo, vtables-adjacent), 1606 undefined imports | `LC_SYMTAB`/`DYSYMTAB` parse |
| minOS | 10.13 | `LC_VERSION_MIN_MACOSX` |
| UUID | `0a9f023feb4335ffb4bf764a4bac026e` | `LC_UUID` |
| Entitlements (observed keys) | `disable-executable-page-protection` (JIT), audio-input, camera | `CodeResources` strings |

Linked frameworks (34 `LC_LOAD_DYLIB` + 4 weak): Cocoa/AppKit/Foundation,
Metal, OpenGL, QuartzCore, AVFoundation/VideoToolbox/CoreMedia, AudioToolbox,
WebKit, Security, `libc++.1`, `libSystem.B`, `@rpath/libmimalloc.3.dylib`
(RPATHs: `@loader_path/./__RobloxPlayer__shared_libs_symlink_tree`,
`@executable_path`). Full list in the evidence ledger.

ObjC surface: 63 classes in `__objc_classlist`, read from the class-list
metadata (pure data): `RobloxPlayerAppDelegate` (94 methods),
`EmbeddedWebView` (90), `RobloxOgreView` (41), `InputCapture` (39),
`RBMessageBus` + protocol family, WebRTC `RTC*`, `GamepadSupport`,
`SDL3_Rumble*`. Full table: [05-objc-bridge.md](05-objc-bridge.md).

Notable: RBX C++ symbols are visibility-hidden — the shipped symtab contains
only std/boost typeinfo plus guards. All engine function addresses below were
reached via ObjC IMPs and callee tracing, not symbol lookup.
