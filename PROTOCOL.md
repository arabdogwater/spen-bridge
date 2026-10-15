# S Pen Bridge wire protocol (v1)

UDP, port **47800** on the PC. All integers little-endian. Every packet starts with a 4-byte header:

| Offset | Size | Value |
|---|---|---|
| 0 | 2 | ASCII `SP` |
| 2 | 1 | protocol version (`1`) |
| 3 | 1 | packet type |

## Packet types

| Type | Name | Direction | Payload after header |
|---|---|---|---|
| 1 | DISCOVER | phone → broadcast | none |
| 2 | OFFER | PC → phone | `u16 monW, u16 monH, u8 nameLen, name (UTF-8)` |
| 3 | HELLO | phone → PC | `u32 session, u8 nameLen, name` |
| 4 | WELCOME | PC → phone | `u32 session, u16 monW, u16 monH, u8 nameLen, name` |
| 5 | PEN | phone → PC | `u32 seq, u8 count, count × sample (16 bytes)` |
| 6 | PING | phone → PC | `u64 phoneNanos` |
| 7 | PONG | PC → phone | echo of the PING payload |
| 8 | KEY | phone → PC | `u8 mods (1 ctrl, 2 shift, 4 alt, 8 win), u8 windowsVirtualKey` |
| 9 | BYE | phone → PC | none |

WELCOME is also re-sent by the PC whenever the target monitor changes, so the phone can re-fit its active area.

## Pen sample (16 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | flags: `1` in range, `2` contact, `4` barrel (S Pen side button), `8` eraser |
| 1 | u8 | hover height, normalised: 0 = touching, 255 = edge of hover range (only meaningful if extra bit 1 is set) |
| 2 | u16 | pressure, 0..65535 |
| 4 | f32 | x, 0..1 inside the active area |
| 8 | f32 | y, 0..1 inside the active area |
| 12 | i8 | tiltX, degrees (-90..90, + = right) |
| 13 | i8 | tiltY, degrees (-90..90, + = toward the user) |
| 14 | u16 | extra flags: `1` hover height valid (the phone reports `AXIS_DISTANCE`) |

## Reliability rules

- Samples carry the **full pen state**, never deltas, so a lost packet heals on the next one.
- PEN packets that change state (touch down, lift, leave range) are sent **3×** with the same `seq`; the PC drops any `seq` it has already seen (wrap-around safe compare).
- While the pen is in range the phone repeats its last sample every 50 ms, even if the pen is still.
- The PC force-lifts the pen if no PEN packet arrives for 600 ms, and drops the client after 4 s of silence.
