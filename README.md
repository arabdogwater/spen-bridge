# S Pen Bridge

Use a Galaxy phone with an S Pen as a drawing tablet for a Windows PC over Wi-Fi.

- **Real Windows Ink pen**: pressure (1024 levels), tilt, S Pen side button as barrel button, eraser. Works in Photoshop, Krita, Clip Studio, Paint, OneNote and any app that takes pen input.
- **Liquid-glass hover lens** on the PC: a refracting glass circle shows exactly where the tip will land before you touch. It is its own window, separate from the mouse cursor, and it never shows up in screenshots or recordings.
- **Lockable tablet mode** on the phone: fullscreen, screen pinned, Back/Home/Recents blocked, fingers and palm ignored. Leave by pressing **Volume Up 3 times**.
- **Light PC side**: one 240 KB native `.exe`, no install, no runtime, lives in the tray.

## Install

| Where | File | How |
|---|---|---|
| PC | `dist/SpenBridge.exe` | Run it. A glass icon appears in the tray. Allow it on **Private networks** when Windows Firewall asks (or tray menu → *Allow through firewall*). |
| Phone | `dist/SpenBridge.apk` | Copy to the phone and install it (allow "install unknown apps" for your file manager). |

Both devices must be on the same Wi-Fi. Open the app: your PC shows up by itself and connects. If it doesn't, type the PC's IP and press **Connect**. Then press **Start tablet mode**.

### One-time phone settings

1. **Pin app** (for the lock): Settings → Security and privacy → More security settings → **Pin app** → On. The first time you start tablet mode, confirm the pin prompt.
2. **Stop Air Command from popping up** when you press the pen button while hovering: Settings → Advanced features → S Pen → Air command → turn off the pen-button shortcut (wording varies by One UI version).
3. **S Pen Air Actions (optional, Bluetooth S Pens only)**: Settings → Advanced features → S Pen → Air actions → S Pen Bridge → enable. Then: click = Undo, double-click = Redo, swipe left/right = brush smaller/bigger (`[` `]`), swipe up/down = zoom in/out.

## Using it

| Phone | PC |
|---|---|
| Hover the pen | Glass lens follows; the mouse stays where it is |
| Touch the screen | Pen down with pressure; the lens shrinks and a pressure arc lights up |
| Hold the side button | Barrel button (right-click / app-specific); lens turns amber |
| Volume Down | Undo (Ctrl+Z), can be switched off |
| Volume Up ×3 (within 2 s) | Leaves tablet mode and unpins |

Tray menu on the PC: pick the monitor to draw on, lens size, glass refraction on/off, **Send hover to apps** (turn on if you want apps like Krita to show their own brush outline while hovering; this moves the mouse while hovering), start with Windows.

The phone letterboxes its drawing area to the monitor's aspect ratio so circles stay round. Switch that off in the app to use the whole phone screen.

## About the lock

Without extra setup the lock is Android **screen pinning**: Home, Recents, notifications and Back are blocked, and the app exits on Volume Up ×3. Android itself still keeps one escape hatch: holding Back + Recents (or the swipe-up-and-hold gesture). That's deliberate and you can't trigger it by accident while drawing.

If you want a true kiosk with no system escape at all, make the app device owner (only possible on a phone with **no accounts** signed in, e.g. a spare device):

```bash
adb shell dpm set-device-owner com.spenbridge/.AdminReceiver
```

Undo it from the app's main screen (**Remove kiosk device-owner mode**).

## How it works

```
S Pen ─► PenView (unbuffered stylus events, history, pressure, tilt, buttons)
       ─► UDP 47800, Wi-Fi low-latency lock, DSCP voice priority
       ─► SpenBridge.exe ─► InjectSyntheticPointerInput (PT_PEN)  → apps
                          └► layered glass lens window (excluded from capture)
```

Wire format: [PROTOCOL.md](PROTOCOL.md).

## Building

| Part | Command | Needs |
|---|---|---|
| PC | `pc\build.bat` | Visual Studio 2022 C++ build tools |
| Android | `cd android && gradlew assembleRelease` | JDK 17, Android SDK 35 (`local.properties` → `sdk.dir`) |

Test the PC side without a phone: `python tools/fake_phone.py` (hover only) or `--draw` (draws a spiral, open Paint first).

Preview the lens look: `SpenBridge.exe --snapshot out.bmp X Y [pressure]`.

## Troubleshooting

| Problem | Fix |
|---|---|
| PC doesn't show up on the phone | Windows Firewall: tray → *Allow through firewall*. Some routers block broadcast ("AP/client isolation"): type the PC IP instead. |
| Strokes land off from the lens | Fixed: injection now uses virtual-desktop coordinates, which matters when a monitor sits above or left of the primary. Update to the latest exe. |
| Need to see what the pen injection is doing | Start the exe with `SPENBRIDGE_DEBUG=1` set; every injected event goes to `%APPDATA%\SpenBridge\log.txt` (failures are always logged). |
| Lag spikes | Use 5 GHz Wi-Fi, keep the phone close to the router. The app already disables Wi-Fi power save while in tablet mode. |
| Tray tooltip shows "Inject error" | Needs Windows 10 1809+. Run the exe as the same user that is signed in at the desktop. |
