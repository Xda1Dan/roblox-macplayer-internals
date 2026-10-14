# Binary overview

In one paragraph: this is a 120 MB x86_64 macOS player executable. The
notes below record its layout, entry point, linked frameworks, app classes,
and address convention used everywhere else in this repository.

## Basic facts

| Fact | Value |
|---|---|
| File | `MacPlayer-version-f9247f9560044102/RobloxPlayer.app/Contents/MacOS/RobloxPlayer` |
| Format | Mach-O 64-bit little-endian, `MH_EXECUTE`, x86_64, PIE |
| Entry | file offset `0x41fe2` → `0x100041fe2` |
| Code | `__TEXT` ≈ 110 MB across 13 sections; `__text` itself ≈ 96 MB |
| Data | `__DATA` with `__const`, `__cfstring`, `__objc_*`, plus 25 sections |
| Image base | `0x100000000` |
| Minimum OS | 10.13 |
| UUID | `0a9f023feb4335ffb4bf764a4bac026e` |
| Symbols | 2275 symtab entries: 668 defined, 1606 undefined imports |
| SHA-256 | `ac9ad59b3aa3805bf2d1c89fb6ff9c7941bf2e57af17a81cbc26abe94a022c47` |

Addresses in this repository are virtual addresses. For this build:

```text
virtual address = file offset + 0x100000000
```

Example: the string reference at file offset `0x68d7655` is address
`0x1068d7655`.

## Frameworks

34 linked libraries plus 4 weak links, including Cocoa / AppKit /
Foundation, Metal, OpenGL, QuartzCore, AVFoundation / VideoToolbox /
CoreMedia, AudioToolbox, WebKit, Security, `libc++.1`, `libSystem.B`,
and `@rpath/libmimalloc.3.dylib`.

RPATHs point at the app’s shared-libs trees under `@loader_path` and
`@executable_path`.

Entitlement-related keys seen in the binary include executable-page
protection settings for JIT, audio input, and camera.

## App surface

63 Objective-C classes live in `__objc_classlist`. The largest ones:

- `RobloxPlayerAppDelegate` (94 methods): launch, deeplinks, join,
  teleport, leave-game, Lua start callbacks, telemetry, Crashpad upload.
- `EmbeddedWebView` (90): focus callbacks, script messages, auth cookies,
  purchases, navigation policy, layout.
- `RobloxOgreView` (41): main render and input view.
- `InputCapture` (39): mouse, scroll, touch, fullscreen, cursor.
- `RBMessageBus` family: typed messages to system services such as
  permissions, contacts, SMS, linking, experiments, and safe-area insets.
- WebRTC `RTC*`, `GamepadSupport`, `SDL3_Rumble*`, display, thermal, and
  battery helpers.

Full table: [05-objc-bridge.md](05-objc-bridge.md).

Key app callbacks in this build:

| Callback | Address |
|---|---|
| `appLuaWillStartWithDataModel:` | `0x10003913e` |
| `ugcGameLuaWillStartWithDataModel:` | `0x100038e3c` |
| shared Lua-start core | `0x100038f12` |
| `handleDeeplinkUrlV2` | `0x1000376fd` |
| `notifyLuaFocusedIfNeeded` | `0x10000cdd2` |
| `didReceiveScriptMessage` | `0x10000e295` |

## A note on names

Engine C++ names are hidden in the shipped symbol table. What remains are
mostly standard-library and support type names plus guards. Engine function
addresses in these notes are therefore identified by behavior, log lines,
string references, and call relationships — not by exported symbols.

## Limits

- Struct offsets such as `+0x778` belong to this build only.
- Flag-gated classes and services change across versions.
- No whole-program call graph is claimed here.
