"""Fake phone for testing SpenBridge.exe without an Android device.

  python tools/fake_phone.py            # discover, connect, hover a circle (no clicks)
  python tools/fake_phone.py --draw     # also draws a spiral (real pen input: open Paint first!)
  python tools/fake_phone.py --ip 192.168.1.20
"""
import argparse
import math
import random
import socket
import struct
import time

PORT = 47800
T_DISCOVER, T_OFFER, T_HELLO, T_WELCOME, T_PEN, T_PING, T_PONG, T_KEY, T_BYE = range(1, 10)
F_IN_RANGE, F_CONTACT = 1, 2


def hdr(t):
    return b"SP" + bytes([1, t])


def sample(flags, x, y, pressure=0.0):
    return struct.pack("<BBHffbbH", flags, 0, int(pressure * 65535), x, y, 0, 0, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", help="PC address (default: broadcast discovery)")
    ap.add_argument("--draw", action="store_true", help="touch down and draw a spiral")
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(1.0)

    s.sendto(hdr(T_DISCOVER), (a.ip or "255.255.255.255", PORT))
    data, pc = s.recvfrom(1500)
    assert data[3] == T_OFFER, data
    w, h, n = struct.unpack_from("<HHB", data, 4)
    print(f"OFFER from {pc[0]}: '{data[9:9 + n].decode()}' monitor {w}x{h}")

    session = random.getrandbits(32)
    name = b"FakePhone"
    s.sendto(hdr(T_HELLO) + struct.pack("<IB", session, len(name)) + name, pc)
    data, _ = s.recvfrom(1500)
    assert data[3] == T_WELCOME and struct.unpack_from("<I", data, 4)[0] == session, data
    print("WELCOME ok")

    t0 = time.perf_counter_ns()
    s.sendto(hdr(T_PING) + struct.pack("<Q", t0), pc)
    data, _ = s.recvfrom(1500)
    assert data[3] == T_PONG
    print(f"PONG rtt {(time.perf_counter_ns() - t0) / 1e6:.2f} ms")

    seq = 0

    def pen(*samples):
        nonlocal seq
        s.sendto(hdr(T_PEN) + struct.pack("<IB", seq, len(samples)) + b"".join(samples), pc)
        seq += 1

    # hover a circle for ~2 s (only moves the glass lens)
    for i in range(240):
        t = i / 240 * 2 * math.pi
        pen(sample(F_IN_RANGE, 0.5 + 0.12 * math.cos(t), 0.5 + 0.2 * math.sin(t)))
        time.sleep(1 / 120)

    if a.draw:
        for i in range(360):
            t = i / 360 * 6 * math.pi
            r = 0.02 + 0.1 * i / 360
            pen(sample(F_IN_RANGE | F_CONTACT, 0.5 + r * math.cos(t) * 0.56, 0.5 + r * math.sin(t), 0.2 + 0.8 * i / 360))
            time.sleep(1 / 240)
        for _ in range(3):  # lift (sent 3x like the phone does)
            s.sendto(hdr(T_PEN) + struct.pack("<IB", seq, 1) + sample(F_IN_RANGE, 0.5, 0.5), pc)
        seq += 1

    for _ in range(3):  # leave range
        s.sendto(hdr(T_PEN) + struct.pack("<IB", seq, 1) + sample(0, 0.5, 0.5), pc)
    s.sendto(hdr(T_BYE), pc)
    print("done")


if __name__ == "__main__":
    main()
