#!/usr/bin/env python3
"""Print the cfw probe's SysEx reports (F0 00 20 29 7D ...) from every Launchkey input port.

    uv run python tools/probe_listen.py [--log build/probe.log] [--all]

--all also prints every other MIDI message from the device (e.g. CCs from Custom modes).
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from send_syx import PORT_HINT, open_mido  # noqa: E402

LISTS = {1: "ABCD", 2: ["pos", "hires", "step"]}


def decode(d):
    t = d[4]
    if t == 1:
        return f"io   list {LISTS[1][d[5]]}  id {d[6]:3d} (0x{d[6]:02x})  value {d[7] | d[8] << 7}"
    if t == 2:
        return f"enc  list {LISTS[2][d[5]]:4} id {d[6]:3d} (0x{d[6]:02x})  delta {d[7] - 64:+d}  value {d[8] | d[9] << 7}"
    if t == 3:
        return f"feat id 0x{d[5]:02x}  value {d[6] | d[7] << 7}"
    if t == 4:
        return f"stack free {d[5] | d[6] << 7} bytes"
    if t == 5:
        return "MODE " + ("TAKEOVER ON (pad Custom 4)" if d[5] else "takeover off")
    if t == 7:
        f = d[5:5 + 128]
        ids = lambda m: " ".join(f"{i:02x}" for i in range(128) if f[i] & m == m)
        free = " ".join(f"{i:02x}" for i in range(128) if not f[i] & 1)
        return (f"feature flags\n  defined:    {ids(1)}\n  persistent: {ids(3)}\n"
                f"  14-bit:     {ids(5)}\n  free:       {free}")
    if t == 6:
        return f"VETO encoder mode change {d[5]} -> {d[6]} (kept {d[5]})"
    return "unknown " + bytes(d).hex(" ")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--log", type=Path)
    ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    mido = open_mido()
    names = [n for n in mido.get_input_names() if PORT_HINT.lower() in n.lower()]
    if not names:
        sys.exit("no Launchkey input port found")
    ports = [mido.open_input(n) for n in names]
    log = a.log.open("a", buffering=1) if a.log else None
    print("listening on", ", ".join(names), flush=True)
    seen = {}
    while True:
        for name, p in zip(names, ports):
            for m in p.iter_pending():
                d = list(m.data) if m.type == "sysex" else None
                if d and d[:4] == [0x00, 0x20, 0x29, 0x7D]:
                    line = decode(d)
                    # each report goes out on both ports; print it once
                    key = (line, round(time.monotonic(), 2))
                    if seen.get(line) == key[1]:
                        continue
                    seen[line] = key[1]
                elif a.all:
                    line = f"midi [{name.split()[-2]}] {m}"
                else:
                    continue
                stamp = time.strftime("%H:%M:%S")
                print(stamp, line, flush=True)
                if log:
                    log.write(f"{stamp} {line}\n")
        time.sleep(0.001)


if __name__ == "__main__":
    main()
