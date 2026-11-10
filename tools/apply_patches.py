#!/usr/bin/env python3
"""Apply byte patches to a raw firmware image.

Usage: apply_patches.py in.bin out.bin patches/*.patch

Patch file format (one edit per line, '#' starts a comment):

    <address> <original bytes hex> <new bytes hex>

    0x08012344  0b4b  00bf          # replace ldr r3,[pc,#44] with nop
    0x080372b4  -     70470000      # '-' = extend/overwrite free space (must be 0xFF or past end)

Addresses are absolute (image is linked at 0x08008000). Every original byte
sequence is verified before writing, so a patch never silently applies to the
wrong firmware version.
"""
import sys
from pathlib import Path

BASE = 0x08008000


def apply(image: bytearray, path: Path):
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split("#")[0].split()
        if not line:
            continue
        if len(line) != 3:
            sys.exit(f"{path}:{lineno}: expected '<addr> <orig|-> <new>'")
        addr, orig, new = int(line[0], 16), line[1], bytes.fromhex(line[2])
        off = addr - BASE
        if off < 0:
            sys.exit(f"{path}:{lineno}: address 0x{addr:08x} is below image base")
        if off + len(new) > len(image):
            image.extend(b"\xFF" * (off + len(new) - len(image)))
        cur = bytes(image[off:off + len(new)])
        if orig == "-":
            if cur != b"\xFF" * len(new):
                sys.exit(f"{path}:{lineno}: 0x{addr:08x} is not free (found {cur.hex()})")
        elif cur != bytes.fromhex(orig):
            sys.exit(f"{path}:{lineno}: 0x{addr:08x} expected {orig}, found {cur.hex()}")
        image[off:off + len(new)] = new
        print(f"  {path.name}:{lineno}  0x{addr:08x}  {cur.hex()} -> {new.hex()}")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    image = bytearray(Path(sys.argv[1]).read_bytes())
    for p in sys.argv[3:]:
        apply(image, Path(p))
    Path(sys.argv[2]).write_bytes(image)
    print(f"{sys.argv[2]}: {len(image)} bytes")


if __name__ == "__main__":
    main()
