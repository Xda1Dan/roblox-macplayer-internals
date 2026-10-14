# App bridge

In one paragraph: the macOS app shell around the engine. AppKit startup,
windows, menus, rendering views, input capture, web views, system message
bus, notifications, gamepads, media, and crash reporting. Engine work
starts where these callbacks hand off to the DataModel and scheduler.

## App and game flow

- `RobloxPlayerAppDelegate` (94 methods): launch, full app window,
  `handleDeeplinkUrl[V2]:`, `leaveGameWithCallback:`, join succeeded and
  failed, teleport started / succeeded / failed, Lua-start callbacks below,
  suspend and close, tray and menu bar, startup launch, telemetry,
  Crashpad machine-id upload.
- `MainAppBridge` (17) and `MenuBarBridge` (12): pipes for deeplinks and
  notifications to the menu-bar helper.
- `MacNotificationDelegate` (2): user-notification callbacks.

Key Lua-start callbacks:

| Callback | Address |
|---|---|
| `appLuaWillStartWithDataModel:` | `ram:0x10003913e` |
| `ugcGameLuaWillStartWithDataModel:` | `ram:0x100038e3c` |
| shared Lua-start core | `ram:0x100038f12` |

## Rendering and input

- `RobloxOgreView` (41): main render and input view — fullscreen, keys,
  mouse, touch, scroll, shortcuts, appearance.
- `InputCapture` (39): mouse warp, buttons, scroll, magnification, touch
  counting, fullscreen transitions, cursor show and hide.
- `InputMethodHandler` (27): IME paths and composition commit, candidate
  rects.
- `RBXWindow` (1): `canBecomeKeyWindow`.
- `AdaptiveSync` (21): display-link vsync scheduling.
- `RBDisplayRefreshRateObserver` (10), `GamepadSupport` (9),
  `SDL3_Rumble*`, thermal and battery loggers.

## Web and Lua bridge

- `EmbeddedWebView` (90): `notifyLuaFocusedIfNeeded` (`ram:0x10000cdd2`)
  and unfocused variant, `didReceiveScriptMessage` (`ram:0x10000e295`),
  auth-cookie sync, purchases, navigation policy, setup and layout.
- `RBMessageBus` (24) plus subscription and request contracts, plus
  identity, SMS, contacts, experience, experimentation, linking,
  permissions, and screen protocols: typed messages to system services.
- `NetworkUtilOSX` (10): reachability monitoring.

Native-to-Lua event example:

```cpp
// Simplified illustration.
void onWebFocusChanged(bool focused) {
  auto event = makeEvent("AppInput", "", focused ? "Focused" : "Unfocused");
  fireToEngine(event);  // engine dispatch, not a direct Lua call
}
```

Web-message example:

```cpp
// Simplified illustration.
void onWebScriptMessage(std::string bodyUtf8) {
  std::string owned = bodyUtf8;  // small-string-shaped copy
  auto target = lookupEngineTarget(owned);
  dispatchToEngine(target, owned);
}
```

At the machine level, the focus path builds `AppInput` / `""` /
`"Focused"` at `0x100016179` (0x58 bytes) and fires through
`0x102ec8158` / `0x102ec81a0`. The web-message path converts the message
body to UTF-8, copies it into an engine string (`0x102d49d80`), resolves
through `0x104929a60` / `0x104f2965c` and global `0x10003ddee`, then
dispatches at `0x10520f486`.

## Media and misc

WebRTC capture and codecs, CoreML features, media-library picker, HTTP
body transport for Crashpad, image-capture device manager, button
handlers, local storage manager, web-view open parameters.

## Addresses in this build

63 classes live in `__objc_classlist`. Method addresses are `ram:`
values. The check mark below means the body is covered in the address
ledger.

- `appLuaWillStartWithDataModel:` `0x10003913e` ✓
- `ugcGameLuaWillStartWithDataModel:` `0x100038e3c` ✓
- `notifyLuaFocusedIfNeeded` `0x10000cdd2` ✓
- `didReceiveScriptMessage` `0x10000e295` ✓
