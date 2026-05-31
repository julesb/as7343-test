#!/usr/bin/env python3
"""Live terminal visualiser for the AS7343 OSC stream.

Registers with the device (so the firmware learns where to send), then listens
for /as7343/spectral and /as7343/meta messages and draws a colour-coded bar
chart of the spectral channels, updating in place.

No external dependencies -- pure stdlib with a minimal OSC decoder.

Usage:
    ./as7343_viz.py [hostname]            # default: esp32-spectral
    ./as7343_viz.py [hostname] --raw      # show the unmapped 18-channel buffer
                                          # (diagnostic: verify channel mapping)
"""

import socket
import struct
import sys
import time

RAW_MODE = "--raw" in sys.argv
_args = [a for a in sys.argv[1:] if not a.startswith("-")]
DEVICE = _args[0] if _args else "esp32-spectral"
REGISTER_PORT = 9001   # firmware learns our IP from any datagram here (LOG_PORT)
OSC_PORT = 9000        # we listen here for /as7343 messages
REGISTER_EVERY = 3.0   # re-announce so the device re-learns us after a reboot
BAR_WIDTH = 48

# Must match the firmware's /as7343/spectral argument order (+ Clear last).
BANDS = [
    ("F1", 405), ("F2", 425), ("FZ", 450), ("F3", 475), ("F4", 515),
    ("F5", 550), ("FY", 555), ("FXL", 600), ("F6", 640), ("F7", 690),
    ("F8", 745), ("NIR", 855), ("Clear", None),
]

# Unmapped buffer order as the library/SMUX returns it, for --raw diagnostics.
# Index = position in readAllChannels(); VIS = broadband clear (repeated).
RAW_BANDS = [
    ("FZ", 450), ("FY", 555), ("FXL", 600), ("NIR", 855), ("VIS", None),
    ("VIS", None), ("F2", 425), ("F3", 475), ("F4", 515), ("F6", 640),
    ("VIS", None), ("VIS", None), ("F1", 405), ("F7", 690), ("F8", 745),
    ("F5", 550), ("VIS", None), ("VIS", None),
]


def wavelength_to_rgb(nm):
    """Approximate visible wavelength (nm) -> (r, g, b) 0-255."""
    if nm is None:
        return (220, 220, 220)          # Clear / broadband -> near-white
    if nm > 780:
        return (90, 0, 0)               # NIR -> dim deep red
    if nm < 380:
        return (90, 0, 90)
    if nm < 440:
        r, g, b = -(nm - 440) / (440 - 380), 0.0, 1.0
    elif nm < 490:
        r, g, b = 0.0, (nm - 440) / (490 - 440), 1.0
    elif nm < 510:
        r, g, b = 0.0, 1.0, -(nm - 510) / (510 - 490)
    elif nm < 580:
        r, g, b = (nm - 510) / (580 - 510), 1.0, 0.0
    elif nm < 645:
        r, g, b = 1.0, -(nm - 645) / (645 - 580), 0.0
    else:
        r, g, b = 1.0, 0.0, 0.0
    # Intensity falls off near the edges of the visible range.
    if nm < 420:
        f = 0.3 + 0.7 * (nm - 380) / (420 - 380)
    elif nm > 700:
        f = 0.3 + 0.7 * (780 - nm) / (780 - 700)
    else:
        f = 1.0
    g_ = lambda c: int(round(255 * (c * f) ** 0.8))
    return (g_(r), g_(g), g_(b))


def _read_string(data, i):
    end = data.index(b"\x00", i)
    s = data[i:end].decode("ascii", "replace")
    return s, (end - i + 4) // 4 * 4 + i


def parse_osc(data):
    """Decode a simple OSC message -> (address, [args]). Ints and floats only."""
    addr, i = _read_string(data, 0)
    if i >= len(data) or data[i:i + 1] != b",":
        return addr, []
    tags, i = _read_string(data, i)
    args = []
    for t in tags[1:]:
        if t == "i":
            args.append(struct.unpack_from(">i", data, i)[0]); i += 4
        elif t == "f":
            args.append(struct.unpack_from(">f", data, i)[0]); i += 4
    return addr, args


def bar_row(prefix, label, nm, val, scale):
    r, g, b = wavelength_to_rgb(nm)
    n = min(int(round(val / scale * BAR_WIDTH)), BAR_WIDTH)
    bar = f"\x1b[38;2;{r};{g};{b}m" + "█" * n + "\x1b[0m"
    return f"{prefix}{label:>12} {bar}{' ' * (BAR_WIDTH - n)} {val:>5}\x1b[0K\n"


def render(values, meta, bands=BANDS, show_index=False):
    # In the normal (spectral) view the last entry is Clear, a broadband
    # intensity reading that's almost always the largest. Pull it out so the
    # auto-scale follows the spectral bands' shape instead of being pinned to
    # Clear; show Clear separately as an intensity/headroom readout.
    clear = None
    if not show_index and bands is BANDS:
        clear = values[-1]
        bands, values = bands[:-1], values[:-1]

    out = ["\x1b[H"]  # cursor home
    title = "AS7343 RAW (unmapped buffer)" if show_index else "AS7343"
    out.append(f"{title}  <-  {DEVICE}   (gain_idx={meta.get('gain', '?')} "
               f"int={meta.get('int_ms', '?')}ms)\x1b[0K\n")
    sat = ""
    if meta.get("asat"):
        sat += "  \x1b[1;31mANALOG SATURATED\x1b[0m"
    if meta.get("dsat"):
        sat += "  \x1b[1;31mDIGITAL SATURATED\x1b[0m"
    out.append(f"spectral peak={max(values) if values else 0}{sat}\x1b[0K\n\n")

    scale = max(max(values), 1) if values else 1
    for i, ((name, nm), val) in enumerate(zip(bands, values)):
        prefix = f"[{i:>2}] " if show_index else ""
        label = f"{name:>5}" + (f" {nm}nm" if nm else "  clear")
        out.append(bar_row(prefix, label, nm, val, scale))

    if clear is not None:
        # Clear bar is scaled to the ADC full-scale ceiling (derived from the
        # integration time: max counts = t_int / 2.78us step), so it reads as
        # absolute intensity / how much saturation headroom is left.
        ceiling = round(meta["int_ms"] * 1000 / 2.78) if meta.get("int_ms") else scale
        ceiling = min(ceiling, 65535)
        pct = round(100 * clear / ceiling) if ceiling else 0
        out.append("\x1b[0K\n")
        out.append(bar_row("", "Clear/VIS", None, clear, max(ceiling, 1)))
        out.append(f"{'':>12} broadband intensity — {pct}% of full-scale "
                   f"({ceiling})\x1b[0K\n")

    out.append("\x1b[0J")  # clear anything below
    sys.stdout.write("".join(out))
    sys.stdout.flush()


def main():
    try:
        addr = socket.gethostbyname(DEVICE)
    except socket.gaierror:
        print(f"Could not resolve '{DEVICE}'. Add it to /etc/hosts or pass an IP.",
              file=sys.stderr)
        sys.exit(1)

    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.bind(("", OSC_PORT))
    rx.settimeout(0.5)

    meta = {}
    last_register = 0.0
    sys.stdout.write("\x1b[2J\x1b[?25l")  # clear screen, hide cursor
    print(f"Registering with {DEVICE} ({addr}) and listening on UDP {OSC_PORT}...")
    try:
        while True:
            now = time.monotonic()
            if now - last_register > REGISTER_EVERY:
                tx.sendto(b"hello", (addr, REGISTER_PORT))
                last_register = now
            try:
                data, _ = rx.recvfrom(2048)
            except socket.timeout:
                continue
            address, args = parse_osc(data)
            if address == "/as7343/meta" and len(args) >= 4:
                meta = {"gain": args[0], "int_ms": round(args[1], 1),
                        "asat": args[2], "dsat": args[3]}
            elif address == "/as7343/raw" and args and RAW_MODE:
                render(args, meta, bands=RAW_BANDS, show_index=True)
            elif address == "/as7343/spectral" and args and not RAW_MODE:
                render(args, meta)
    except KeyboardInterrupt:
        pass
    finally:
        sys.stdout.write("\x1b[?25h\n")  # show cursor again


if __name__ == "__main__":
    main()
