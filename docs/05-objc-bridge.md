# 05 — ObjC bridge (63 classes, parsed from `__objc_classlist`)

Parsed with `ObjCDump.java` (pure data reads, no disassembly). Method IMPs are
`ram:` addresses; several are decompiled (see ledger).

## App / game flow

* `RobloxPlayerAppDelegate` (94) — `applicationDidFinishLaunching:`,
  `launchFullAppWindow`, `handleDeeplinkUrl[V2]:`, `leaveGameWithCallback:`,
  `onJoinGame{Succeeded,Failed}`, `onTeleport{Started,Succeeded,Failed}`,
  `appLuaWillStartWithDataModel:` **`0x10003913e`** ✅,
  `ugcGameLuaWillStartWithDataModel:` **`0x100038e3c`** ✅,
  `ugcGameWillClose/DidClose/Suspend`, tray/menu-bar + `launchAtStartup`,
  `report*Telemetry`, Crashpad machine-id upload.
* `MainAppBridge` (17) / `MenuBarBridge` (12) — deeplink + notification pipes
  to the menu-bar helper.
* `MacNotificationDelegate` (2) — `UNUserNotificationCenter` callbacks.

## Rendering + input (the visible client)

* `RobloxOgreView` (41) — main render/input view: fullscreen, key/mouse/touch,
  scroll, folder shortcuts, appearance.
* `InputCapture` (39) — mouse warp, buttons, scroll, magnification, touch
  counting, fullscreen transitions, cursor show/hide.
* `InputMethodHandler` (27) — IME: Japanese/Korean paths, composition commit
  (`commitCompositionRBXEvent`), candidate window rects.
* `RBXWindow` (1) — `canBecomeKeyWindow`.
* `AdaptiveSync` (21) — display-link vsync scheduling.
* `RBDisplayRefreshRateObserver` (10), `GamepadSupport` (9),
  `SDL3_Rumble{Context,Motor}` (13+10), `RBThermalStateLogger`,
  `RBBatteryStateLogger`.

## Web / Lua bridge

* `EmbeddedWebView` (90) — `notifyLuaFocusedIfNeeded` ✅ (`0x10000cdd2`) /
  `notifyLuaUnfocusedIfNeeded`, `userContentController:didReceiveScriptMessage:`
  ✅ (`0x10000e295`), `syncAuthCookieFromWebViewToApp`, robux/purchase,
  navigation policy, `setupWebView/Constraints`.
* `RBMessageBus` (24) + `SubscriptionContract` / `RawSubscriptionContract` /
  `MakeRequestContract` (17/17/12) + `RB{Identity,SMS,Contacts,Experience,
  Experimentation,Linking,Permissions,Screen}Protocol*` — typed message bus to
  system services (permissions, contacts, SMS/OTP, linking/attribution,
  experiments via `getUserLayerVariables`, safe-area insets).
* `NetworkUtilOSX` (10) — reachability monitoring.

## Media / misc

* WebRTC `RTC*` (VideoCapturer, I420 buffers, codecs, `RTCDispatcher`),
  `CoreMLFeatures`, `AppleMediaLibraryPicker`, `CrashpadHTTPBodyStreamTransport`,
  `ImageCaptureDeviceManagerImpl`, `BlockButtonActionHandler`,
  `OSXLocalStorageManager`, `WebViewOpenParameters`.

✅ = decompiled in this repo's research (see ledger for findings).
