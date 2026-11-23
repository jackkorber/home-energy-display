#!/usr/bin/env python3
"""Save screenshots of the display over USB as round PNGs.

    python3 tools/snap.py <out_dir> [--demo] [pages...]

pages: 0 energy, 1 now, 2 month, 3 VPP, 4 weather, 5 board battery (default: all). --demo shows sample data instead of your house.
Needs: pip install pyserial pillow
"""
import base64, glob, os, re, sys, time

import serial
from PIL import Image, ImageDraw

NAMES = {0: "energy", 1: "now", 2: "month", 3: "vpp", 4: "weather", 5: "device"}


def open_port():
    ports = glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*")
    if not ports:
        sys.exit("Board not found: plug it in by USB")
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = ports[0], 115200, 0.5
    p.dtr = p.rts = False  # don't reset the board when opening the port
    p.open()
    try:  # macOS drops DTR/RTS on close (HUPCL) and the board can end up in download mode
        import termios
        a = termios.tcgetattr(p.fileno())
        a[2] &= ~termios.HUPCL
        termios.tcsetattr(p.fileno(), termios.TCSANOW, a)
    except Exception:
        pass
    return p


def send(p, cmd):
    p.write((cmd + "\n").encode())
    p.flush()


def grab(p):
    p.reset_input_buffer()
    send(p, "s")
    hdr, data, t0 = None, [], time.time()
    while time.time() - t0 < 20:
        line = p.readline().decode(errors="replace").strip()
        if line.startswith("SNAP "):
            hdr, data = line.split(), []
        elif hdr and line == "ENDSNAP":
            break
        elif hdr and line and len(line) % 4 == 0 and re.fullmatch(r"[A-Za-z0-9+/=]+", line):
            data.append(line)
    if not hdr:
        return None
    _, _, w, h, stride, n = hdr
    w, h, stride, n = int(w), int(h), int(stride), int(n)
    raw = b"".join(base64.b64decode(x) for x in data)
    if len(raw) < n:
        return None
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        row = raw[y * stride:(y + 1) * stride]
        for x in range(w):
            v = row[2 * x] | row[2 * x + 1] << 8
            px[x, y] = ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    # Round display: make the corners transparent
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, w - 1, h - 1), fill=255)
    img.putalpha(mask)
    return img


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    out, demo = args[0], "--demo" in args
    pages = [int(a) for a in args[1:] if a.isdigit()] or list(NAMES)
    os.makedirs(out, exist_ok=True)
    p = open_port()
    time.sleep(15)  # opening the port can reset the board: wait for it to boot and get data
    if demo:
        send(p, "d")
        time.sleep(1.5)
    for n in pages:
        send(p, f"p{n}")
        time.sleep(2.5)  # let the page switch and redraw settle
        img = grab(p)
        if img is None:
            print(f"page {n}: no image")
            continue
        path = os.path.join(out, f"{n}-{NAMES.get(n, n)}.png")
        img.save(path)
        print(path)
    send(p, "p0")
    if demo:
        send(p, "d")  # back to real data
    p.close()


if __name__ == "__main__":
    main()
