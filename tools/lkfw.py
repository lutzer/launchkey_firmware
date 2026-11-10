#!/usr/bin/env python3
"""Pack/unpack Novation Launchkey Mini MK4 firmware SysEx files.

Container format (reverse engineered from launchkeyminimk4-firmware-1563.syx):

  Every message is  F0 00 20 29 00 <cmd> <payload...> F7   (Novation manufacturer ID 00 20 29)

  cmd 0x71  start     payload: 02 13 <6 bytes "build id">
  cmd 0x7C  header    payload: 00 <6 bytes "build id"> <size: 8 nibbles> <crc: 8 nibbles>
  cmd 0x72  data      payload: 37 x 7-bit bytes = 259-bit MSB-first bitstream,
                               first 256 bits = 32 image bytes, last 3 bits = 0
  cmd 0x73  last data same encoding as 0x72; carries image bytes 0x00..0x1F
                      (the vector table head is sent LAST, so an interrupted
                      update never leaves a bootable-looking half image)

  Data messages carry image[0x20:] in order (final chunk padded with 0xFF),
  then the 0x73 message carries image[0x00:0x20].

  size = number of meaningful image bytes (rest of the last chunk is 0xFF)
  crc  = CRC-32/MPEG-2 (poly 0x04C11DB7, init 0xFFFFFFFF, no reflection, no xorout)
         over image[:size]

  The image is an ARM Cortex-M (Thumb) application linked at 0x08008000
  (STM32-style flash; a 32 KiB bootloader lives at 0x08000000).
"""
import argparse
import json
import sys
from pathlib import Path

SYSEX_HDR = bytes([0xF0, 0x00, 0x20, 0x29, 0x00])
CMD_START, CMD_DATA, CMD_LAST, CMD_INFO = 0x71, 0x72, 0x73, 0x7C
CHUNK = 32
PAYLOAD7 = 37  # ceil(32*8/7)
LOAD_ADDR = 0x08008000


def crc32_mpeg2(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def split_sysex(blob: bytes):
    msgs, start = [], None
    for i, b in enumerate(blob):
        if b == 0xF0:
            start = i
        elif b == 0xF7 and start is not None:
            msgs.append(blob[start:i + 1])
            start = None
    return msgs


def unpack7(payload: bytes) -> bytes:
    v = 0
    for b in payload:
        v = (v << 7) | b
    nbits = len(payload) * 7
    extra = nbits % 8
    if v & ((1 << extra) - 1):
        raise ValueError("non-zero padding bits in data message")
    return (v >> extra).to_bytes(nbits // 8, "big")


def pack7(data: bytes) -> bytes:
    assert len(data) == CHUNK
    v = int.from_bytes(data, "big") << (PAYLOAD7 * 7 - CHUNK * 8)
    return bytes((v >> (7 * (PAYLOAD7 - 1 - i))) & 0x7F for i in range(PAYLOAD7))


def nibbles_to_int(n: bytes) -> int:
    v = 0
    for x in n:
        v = (v << 4) | (x & 0xF)
    return v


def int_to_nibbles(v: int, count: int = 8) -> bytes:
    return bytes((v >> (4 * (count - 1 - i))) & 0xF for i in range(count))


def msg(cmd: int, payload: bytes) -> bytes:
    return SYSEX_HDR + bytes([cmd]) + payload + b"\xF7"


def unpack(syx: bytes):
    msgs = split_sysex(syx)
    meta, data, last = {}, b"", None
    for m in msgs:
        if not m.startswith(SYSEX_HDR):
            raise ValueError("not a Novation sysex message: " + m[:8].hex())
        cmd, p = m[5], m[6:-1]
        if cmd == CMD_START:
            meta["start_payload"] = p.hex()
        elif cmd == CMD_INFO:
            meta["info_prefix"] = p[:-16].hex()
            meta["size"] = nibbles_to_int(p[-16:-8])
            meta["crc"] = nibbles_to_int(p[-8:])
        elif cmd == CMD_DATA:
            data += unpack7(p)
        elif cmd == CMD_LAST:
            last = unpack7(p)
        else:
            raise ValueError(f"unknown command 0x{cmd:02x}")
    if last is None:
        raise ValueError("missing final (0x73) data message")
    image = (last + data)[:meta["size"]]
    calc = crc32_mpeg2(image)
    if calc != meta["crc"]:
        raise ValueError(f"CRC mismatch: header 0x{meta['crc']:08x}, computed 0x{calc:08x}")
    return image, meta


def pack(image: bytes, meta: dict) -> bytes:
    size = len(image)
    padded = image + b"\xFF" * (-size % CHUNK)
    out = [msg(CMD_START, bytes.fromhex(meta["start_payload"])),
           msg(CMD_INFO, bytes.fromhex(meta["info_prefix"]) + int_to_nibbles(size) + int_to_nibbles(crc32_mpeg2(image)))]
    for off in range(CHUNK, len(padded), CHUNK):
        out.append(msg(CMD_DATA, pack7(padded[off:off + CHUNK])))
    out.append(msg(CMD_LAST, pack7(padded[:CHUNK])))
    return b"".join(out)


def cmd_unpack(a):
    image, meta = unpack(Path(a.syx).read_bytes())
    Path(a.bin).write_bytes(image)
    meta.update(load_address=f"0x{LOAD_ADDR:08x}", source=Path(a.syx).name,
                crc=f"0x{meta['crc']:08x}")
    Path(a.meta or a.bin + ".json").write_text(json.dumps(meta, indent=2) + "\n")
    print(f"image: {len(image)} bytes @ 0x{LOAD_ADDR:08x}, crc {meta['crc']} -> {a.bin}")


def cmd_pack(a):
    meta = json.loads(Path(a.meta or a.bin + ".json").read_text())
    image = Path(a.bin).read_bytes()
    syx = pack(image, meta)
    Path(a.syx).write_bytes(syx)
    print(f"packed {len(image)} bytes, crc 0x{crc32_mpeg2(image):08x} -> {a.syx}")


def cmd_info(a):
    image, meta = unpack(Path(a.syx).read_bytes())
    import struct
    sp, reset = struct.unpack_from("<II", image)
    print(json.dumps(meta | {"crc": f"0x{meta['crc']:08x}"}, indent=2))
    print(f"initial SP 0x{sp:08x}, reset 0x{reset:08x}, end 0x{LOAD_ADDR + len(image):08x}")


def cmd_verify(a):
    syx = Path(a.syx).read_bytes()
    image, meta = unpack(syx)
    again = pack(image, meta)
    if again != syx:
        sys.exit("round-trip FAILED: repacked file differs from original")
    print(f"round-trip OK ({len(syx)} bytes identical)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("unpack", help="syx -> raw image (+ .json metadata)")
    p.add_argument("syx"); p.add_argument("bin"); p.add_argument("--meta")
    p.set_defaults(fn=cmd_unpack)
    p = sub.add_parser("pack", help="raw image (+ .json metadata) -> syx, recomputing size/CRC")
    p.add_argument("bin"); p.add_argument("syx"); p.add_argument("--meta")
    p.set_defaults(fn=cmd_pack)
    p = sub.add_parser("info", help="print header fields")
    p.add_argument("syx"); p.set_defaults(fn=cmd_info)
    p = sub.add_parser("verify", help="check unpack+pack reproduces the file byte-for-byte")
    p.add_argument("syx"); p.set_defaults(fn=cmd_verify)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
