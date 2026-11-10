#!/usr/bin/env python3
"""Send a firmware .syx file to a MIDI port, one SysEx message at a time.

    send_syx.py --list                          list MIDI output ports
    send_syx.py build/patched.syx --dry-run     parse + check only, send nothing
    send_syx.py build/patched.syx --port "Launchkey Mini MK4" [--delay-ms 20]

Requires:  pip3 install -r requirements.txt   (mido + python-rtmidi)

The file is checked with lkfw.unpack (size + CRC) before anything is sent.
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lkfw  # noqa: E402


def open_mido():
    try:
        import mido
        mido.set_backend("mido.backends.rtmidi")
        return mido
    except ImportError:
        sys.exit("mido/python-rtmidi missing: pip3 install -r requirements.txt")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("syx", nargs="?")
    ap.add_argument("--list", action="store_true", help="list MIDI output ports and exit")
    ap.add_argument("--port", help="output port name (substring match)")
    ap.add_argument("--delay-ms", type=float, default=20.0, help="pause after each message (default 20)")
    ap.add_argument("--dry-run", action="store_true", help="validate the file, do not send")
    a = ap.parse_args()

    if a.list:
        for name in open_mido().get_output_names():
            print(name)
        return
    if not a.syx:
        ap.error("syx file required")

    blob = Path(a.syx).read_bytes()
    image, meta = lkfw.unpack(blob)  # raises on bad CRC / format
    msgs = lkfw.split_sysex(blob)
    print(f"{a.syx}: {len(msgs)} messages, image {len(image)} bytes, crc 0x{meta['crc']:08x} OK")
    print(f"estimated time: {len(msgs) * a.delay_ms / 1000:.0f}s at {a.delay_ms} ms/message")
    if a.dry_run:
        return

    mido = open_mido()
    names = mido.get_output_names()
    matches = [n for n in names if a.port and a.port.lower() in n.lower()]
    if len(matches) != 1:
        sys.exit(f"--port must match exactly one of: {names}")
    print(f"sending to '{matches[0]}' – do not unplug the device")
    with mido.open_output(matches[0]) as out:
        for i, m in enumerate(msgs, 1):
            out.send(mido.Message("sysex", data=list(m[1:-1])))
            time.sleep(a.delay_ms / 1000)
            if i % 200 == 0 or i == len(msgs):
                print(f"\r  {i}/{len(msgs)}", end="", flush=True)
    print("\ndone")


if __name__ == "__main__":
    main()
