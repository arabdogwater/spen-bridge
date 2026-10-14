# S Pen Bridge — general instructions

How to read: read CORE (down to `END CORE`) every task, then use the Index to load only the categories you touch (`Grep "^#{1,2} "` for line numbers).

## Index

| Category | Sections |
|---|---|
| Android | Stylus input, Lock / kiosk, S Pen Air Actions, Android build |
| PC | Pen injection, Glass lens, PC build |
| Network | Protocol and reliability |

# CORE

- Project: Galaxy S Pen phone → Windows drawing tablet over Wi-Fi. `android/` (Kotlin, no AndroidX, minSdk 29), `pc/spenbridge.cpp` (single-file Win32 C++, static CRT), `PROTOCOL.md` (UDP 47800).
- Hard rule: PC side stays one native exe with no dependencies. Don't add frameworks or runtimes.
- Hard rule: pen samples always carry full state (never deltas); state changes are sent 3× with the same seq. See "Protocol and reliability".
- Hard rule: never inject pen contact on the developer's live desktop to test; use `tools/fake_phone.py` hover-only, or `--draw` only with Paint open and the user's OK.
- Protocol changes must be made in all three places: `PROTOCOL.md`, `Proto.kt`, `spenbridge.cpp`.
- Builds: `pc\build.bat` → `pc/build/SpenBridge.exe`; `android/gradlew.bat assembleRelease` (JDK 17, SDK at `D:/Android/Sdk`). Copy outputs to `dist/` (gitignored).
- Lens look is checked with `SpenBridge.exe --snapshot out.bmp X Y [pressure]` (the live lens is excluded from screenshots).

END CORE

# Android

## Stylus input
- `PenView` uses `requestUnbufferedDispatch(SOURCE_CLASS_POINTER)` (API 31+) plus per-DOWN `requestUnbufferedDispatch(event)`; sends all historical samples.
- Only `TOOL_TYPE_STYLUS` / `TOOL_TYPE_ERASER` pointers count; fingers are swallowed (palm rejection).
- Samsung fires `HOVER_EXIT` right before `ACTION_DOWN`: out-of-range is delayed 120 ms (250 ms after UP) and cancelled by the next hover/down.
- Side button = `BUTTON_STYLUS_PRIMARY | BUTTON_STYLUS_SECONDARY | BUTTON_SECONDARY` (Samsung varies) → barrel flag.
- Tilt: Android `AXIS_TILT` + `AXIS_ORIENTATION` → Windows tiltX/tiltY via atan2 (Chromium-style). Most Galaxy phones report no tilt.
- `onResolvePointerIcon` returns `TYPE_NULL` to hide Samsung's hover pointer.

## Lock / kiosk
- `TabletActivity.enterLock()` → `startLockTask()` = screen pinning (user must enable "Pin app"). If device owner (`AdminReceiver`), sets lock-task packages + `LOCK_TASK_FEATURE_NONE` = hard kiosk.
- Exit only via Volume Up ×3 within 2 s (`dispatchKeyEvent`). Back swallowed via `OnBackInvokedCallback` (33+) and `onBackPressed`.
- `WIFI_MODE_FULL_LOW_LATENCY` WifiLock held while resumed.

## S Pen Air Actions
- Samsung framework route (no SDK jar): intent-filter + meta-data `com.samsung.android.support.REMOTE_ACTION` on `TabletActivity`, actions in `res/xml/spen_remote_actions.xml` (version 1.2, trigger_key like `CTRL_LEFT+Z`). Keys arrive in `onKeyDown`, mapped by `KeyMap` → KEY packet. Disabled by default per app; user enables in S Pen settings. Bluetooth S Pens only.

## Android build
- AGP 8.7.3, Kotlin 2.0.21, Gradle 8.11.1 wrapper. Release is minified and signed with the debug key for sideloading.
- `local.properties` must use forward slashes (`sdk.dir=D:/Android/Sdk`); backslashes break it.

# PC

## Pen injection
- `CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_NONE)` + `InjectSyntheticPointerInput`, process is per-monitor DPI aware v2, physical pixels.
- `ptPixelLocation` is relative to the VIRTUAL screen top-left, not screen coords: `toInjectSpace()` subtracts `SM_X/YVIRTUALSCREEN`. The dev PC has a monitor 211 px above the primary; without this every click landed 211 px above the lens.
- Inject failures always go to `%APPDATA%\SpenBridge\log.txt`; `SPENBRIDGE_DEBUG=1` logs every inject.
- Flag sequence: enter `INRANGE|UPDATE`, down `INRANGE|INCONTACT|DOWN`, move `INRANGE|INCONTACT|UPDATE`, lift `INRANGE|UP`, leave `UPDATE`. Tracked in `g_injRange/g_injContact` so invalid transitions never happen.
- Default: hover is NOT injected (lens only, mouse stays free); tray "Send hover to apps" turns it on.
- Watchdog (100 ms timer): lift if no PEN packet for 600 ms, drop client after 4 s.

## Glass lens
- Layered `WS_EX_TRANSPARENT|TOPMOST|NOACTIVATE` window, per-pixel alpha via `UpdateLayeredWindow`, `WDA_EXCLUDEFROMCAPTURE` so a `BitBlt` of the screen under it gets the real content for refraction.
- `renderLens()`: shadow, refracted background (edge-only displacement + chroma split, centre 1:1), specular crescent, edge line, accent ring, pressure arc, pen-point dot. Accent: hover ice, contact sky, barrel amber, eraser rose.
- Rendered on the UI thread (coalesced `WM_APP_OVERLAY` + 16 ms timer while visible); injection happens on the network thread and never waits for drawing.

## PC build
- `pc\build.bat` finds VS via vswhere, `cl /O2 /MT`, ~240 KB exe. Libraries come from `#pragma comment(lib)`.

# Network

## Protocol and reliability
- Full spec in `PROTOCOL.md`. Discovery: phone broadcasts DISCOVER (255.255.255.255 + subnet broadcast) every 1 s; PC replies OFFER unicast.
- Phone socket bound to the Wi-Fi `Network` (works when Wi-Fi has no internet), DSCP EF (0xB8).
- Phone repeats last in-range sample every 50 ms; PC drops seq ≤ last (wrap-safe).
