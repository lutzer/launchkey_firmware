#!/usr/bin/env python3
"""Upload a firmware .syx to the Launchkey Mini MK4 (or query it).

    uv run tools/send_syx.py --list                      list MIDI ports
    uv run tools/send_syx.py --inquiry                   ask the device for model + firmware version
    uv run tools/send_syx.py build/patched.syx --dry-run validate the file only
    uv run tools/send_syx.py build/patched.syx           upload (port auto-detected)

Upload protocol (from the fw 1563 SysEx handler at 0x08030E00):
  host -> 0x71 start, 0x7C header (size/CRC/version)
  dev  -> F0 00 20 29 00 72 <n:3x7bit> F7   "ready"/ack; sent after the header (possibly only after the
                                           user confirms on the device) and after every 0x72 data block
  host -> 0x72 data blocks, one at a time, each waiting for its ack
  host -> 0x73 final block (vector table)
  dev  -> F0 00 20 29 00 73 <status> 7F 7F F7   (if sent; logged)

--no-ack falls back to blind sending with --delay-ms between messages.
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lkfw  # noqa: E402

NOVATION = (0x00, 0x20, 0x29)
PORT_HINT = "Launchkey Mini MK4"


def open_mido():
    try:
        import mido
        mido.set_backend("mido.backends.rtmidi")
        return mido
    except ImportError:
        sys.exit("mido/python-rtmidi missing: run `uv sync` and use `uv run tools/send_syx.py ...`")


def pick(names, wanted, kind):
    # Prefer the "MIDI" interface over "DAW": the update is handled on the MIDI port.
    matches = [n for n in names if wanted.lower() in n.lower()]
    midi = [n for n in matches if " MIDI " in n + " "]
    matches = midi or matches
    if len(matches) != 1:
        sys.exit(f"cannot pick a unique {kind} port for '{wanted}' from {names}; use --port")
    return matches[0]


def drain(inp):
    return [m for m in inp.iter_pending() if m.type == "sysex"]


def wait_sysex(inp, pred, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for m in inp.iter_pending():
            if m.type == "sysex" and pred(m.data):
                return m.data
        time.sleep(0.0005)
    return None


def is_ack(d, cmd):
    return len(d) >= 5 and tuple(d[:3]) == NOVATION and d[3] == 0x00 and d[4] == cmd


def ack_value(d):
    return d[5] | d[6] << 7 | d[7] << 14 if len(d) >= 8 else None


def inquiry(mido, out_name, in_name):
    with mido.open_input(in_name) as inp, mido.open_output(out_name) as out:
        drain(inp)
        out.send(mido.Message("sysex", data=[0x7E, 0x7F, 0x06, 0x01]))
        d = wait_sysex(inp, lambda d: len(d) >= 4 and d[0] == 0x7E and d[2:4] == (0x06, 0x02), 2.0)
    if d is None:
        sys.exit("no reply to device inquiry")
    d = list(d)
    print("reply:", bytes([0xF0] + d + [0xF7]).hex(" "))
    fam = d[7] << 8 | d[8] if len(d) > 8 else None
    model = {0x4101: "Launchkey Mini MK4 25", 0x4201: "Launchkey Mini MK4 37"}.get(fam, f"family 0x{fam:04x}")
    if len(d) >= 15:
        print(f"{model}, firmware {d[11]}.{d[12]} build {d[13] << 7 | d[14]}")


def upload(mido, msgs, out_name, in_name, a):
    def send(m):
        out.send(mido.Message("sysex", data=list(m[1:-1])))

    data_msgs = [m for m in msgs if m[5] == lkfw.CMD_DATA]
    total = len(data_msgs)
    with mido.open_input(in_name) as inp, mido.open_output(out_name) as out:
        drain(inp)
        start = time.monotonic()
        for m in msgs:
            if m[5] in (lkfw.CMD_START, lkfw.CMD_INFO):
                send(m)
                time.sleep(0.1)
        if a.no_ack:
            for i, m in enumerate(msgs[2:], 1):
                send(m)
                time.sleep(a.delay_ms / 1000)
                if i % 200 == 0 or i == len(msgs) - 2:
                    print(f"\r  {i}/{len(msgs) - 2}", end="", flush=True)
            print("\nsent (no acknowledgement checking)")
            return

        print("waiting for the device to accept the update...")
        print("  -> if the display shows 'Press to Upgrade', confirm on the device")
        d = wait_sysex(inp, lambda d: is_ack(d, 0x72), a.confirm_timeout)
        if d is None:
            sys.exit(f"no 'ready' from device within {a.confirm_timeout}s – nothing was written, aborting")
        expected = ack_value(d) + 1
        for i, m in enumerate(data_msgs, 1):
            send(m)
            d = wait_sysex(inp, lambda d: is_ack(d, 0x72) or is_ack(d, 0x73), a.ack_timeout)
            if d is None:
                sys.exit(f"\nno ack for block {i}/{total} – upload incomplete! Re-run the upload "
                         "(or flash the original firmware) before power-cycling.")
            if is_ack(d, 0x73) or ack_value(d) != expected:
                sys.exit(f"\nunexpected reply at block {i}/{total}: {bytes(d).hex(' ')} – aborting")
            expected += 1
            if i % 100 == 0 or i == total:
                rate = i / (time.monotonic() - start)
                print(f"\r  {i}/{total} blocks  ({rate:.0f}/s)", end="", flush=True)
        print()
        send(msgs[-1])  # 0x73 final block
        d = wait_sysex(inp, lambda d: is_ack(d, 0x73), 10.0)
        if d is None:
            print("final block sent; no 0x73 status reply seen (check the device display)")
        else:
            print(f"final status reply: {bytes(d).hex(' ')}")
        print(f"done in {time.monotonic() - start:.0f}s – the device should restart with the new firmware")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("syx", nargs="?")
    ap.add_argument("--list", action="store_true", help="list MIDI ports and exit")
    ap.add_argument("--inquiry", action="store_true", help="query model + firmware version and exit")
    ap.add_argument("--port", default=PORT_HINT, help=f"port name substring (default '{PORT_HINT}')")
    ap.add_argument("--dry-run", action="store_true", help="validate the file, do not send")
    ap.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    ap.add_argument("--confirm-timeout", type=float, default=120, help="seconds to wait for the device to accept (default 120)")
    ap.add_argument("--ack-timeout", type=float, default=3, help="seconds to wait per block ack (default 3)")
    ap.add_argument("--no-ack", action="store_true", help="blind mode: ignore acks, sleep --delay-ms between messages")
    ap.add_argument("--delay-ms", type=float, default=20.0, help="pause per message in --no-ack mode (default 20)")
    a = ap.parse_args()

    if a.list:
        mido = open_mido()
        print("outputs:", *mido.get_output_names(), sep="\n  ")
        print("inputs:", *mido.get_input_names(), sep="\n  ")
        return
    if a.inquiry:
        mido = open_mido()
        inquiry(mido, pick(mido.get_output_names(), a.port, "output"), pick(mido.get_input_names(), a.port, "input"))
        return
    if not a.syx:
        ap.error("syx file required")

    blob = Path(a.syx).read_bytes()
    image, meta = lkfw.unpack(blob)  # raises on bad CRC / format
    msgs = lkfw.split_sysex(blob)
    print(f"{a.syx}: {len(msgs)} messages, image {len(image)} bytes, crc 0x{meta['crc']:08x} OK")
    if len(image) > 0x38000:
        sys.exit("image larger than 0x38000 bytes – the firmware's updater rejects that")
    if a.dry_run:
        return

    mido = open_mido()
    out_name = pick(mido.get_output_names(), a.port, "output")
    in_name = pick(mido.get_input_names(), a.port, "input")
    print(f"device ports: out '{out_name}', in '{in_name}'")
    if not a.yes and input("Flash this firmware now? Do not unplug during the upload. [y/N] ").lower() != "y":
        sys.exit("aborted")
    upload(mido, msgs, out_name, in_name, a)


if __name__ == "__main__":
    main()
