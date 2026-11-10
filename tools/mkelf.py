#!/usr/bin/env python3
"""Wrap a raw Cortex-M flash image in a minimal ARM ELF so disassemblers
(Hopper via REA, objdump, Ghidra, ...) load it at the right address with
Thumb entry points and named vector-table handlers.

Usage: mkelf.py image.bin image.elf [--base 0x08008000] [--syms syms.txt]

syms.txt (optional, one per line):  <hex address> <name> [func|data]
"""
import argparse
import struct
from pathlib import Path

VECTORS = ["__initial_sp", "Reset_Handler", "NMI_Handler", "HardFault_Handler",
           "MemManage_Handler", "BusFault_Handler", "UsageFault_Handler", None, None, None, None,
           "SVC_Handler", "DebugMon_Handler", None, "PendSV_Handler", "SysTick_Handler"]


def collect_symbols(image: bytes, base: int, symfile):
    syms = {}  # name -> (addr, is_func)
    end = base + len(image)
    # Vector table: 16 core entries + external IRQs until the first value that is not a flash pointer.
    n = 0
    while 4 * (n + 1) <= len(image):
        (v,) = struct.unpack_from("<I", image, 4 * n)
        if n >= 16 and not (v == 0 or (base <= v < end and v & 1)):
            break
        if n > 0 and v and base <= v < end:
            name = VECTORS[n] if n < 16 and VECTORS[n] else f"IRQ{n - 16}_Handler"
            if v & ~1 not in [a & ~1 for a, _ in syms.values()]:
                syms[name] = (v | 1, True)
        n += 1
    syms["__vector_table"] = (base, False)
    if symfile and Path(symfile).exists():
        for line in Path(symfile).read_text().splitlines():
            line = line.split("#")[0].split()
            if len(line) >= 2:
                addr, name = int(line[0], 16), line[1]
                is_func = len(line) < 3 or line[2] == "func"
                syms[name] = ((addr | 1) if is_func else addr, is_func)
    return syms


def build_elf(image: bytes, base: int, syms: dict) -> bytes:
    # Layout: ehdr | phdr | .text data | .strtab/.symtab/.shstrtab | shdrs
    ehsize, phsize, shsize = 52, 32, 40
    text_off = 0x1000
    shstr = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
    strtab = b"\0"
    symtab = b"\0" * 16
    # $t mapping symbol so tools treat the whole section as Thumb, then real symbols
    for name, (addr, is_func) in [("$t", (base, False))] + sorted(syms.items(), key=lambda kv: kv[1][0]):
        off = len(strtab)
        strtab += name.encode() + b"\0"
        # LOCAL NOTYPE for $t; GLOBAL FUNC, or GLOBAL NOTYPE for data (STT_OBJECT has no size here and
        # would make objdump dump all following code as raw bytes)
        info = 0 if name == "$t" else (1 << 4) | (2 if is_func else 0)
        symtab += struct.pack("<IIIBBH", off, addr, 0, info, 0, 1)
    nlocal = 2  # null + $t
    body = bytearray(text_off) + image
    def align(b, a=4):
        b += b"\0" * (-len(b) % a)
    align(body); strtab_off = len(body); body += strtab
    align(body); symtab_off = len(body); body += symtab
    align(body); shstr_off = len(body); body += shstr
    align(body); shoff = len(body)
    sh = [b"\0" * shsize,
          struct.pack("<10I", 1, 1, 0x6, base, text_off, len(image), 0, 0, 4, 0),             # .text  PROGBITS AX
          struct.pack("<10I", 7, 2, 0, 0, symtab_off, len(symtab), 3, nlocal, 4, 16),          # .symtab -> link .strtab
          struct.pack("<10I", 15, 3, 0, 0, strtab_off, len(strtab), 0, 0, 1, 0),               # .strtab
          struct.pack("<10I", 23, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)]                 # .shstrtab
    body += b"".join(sh)
    entry = syms.get("Reset_Handler", (base | 1, True))[0]
    ehdr = (b"\x7fELF\x01\x01\x01" + b"\0" * 9 +
            struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, entry, ehsize, shoff, 0x05000400,
                        ehsize, phsize, 1, shsize, len(sh), 4))
    phdr = struct.pack("<8I", 1, text_off, base, base, len(image), len(image), 5, 0x1000)  # PT_LOAD R+X
    body[0:ehsize] = ehdr
    body[ehsize:ehsize + phsize] = phdr
    return bytes(body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin"); ap.add_argument("elf")
    ap.add_argument("--base", default="0x08008000")
    ap.add_argument("--syms")
    a = ap.parse_args()
    base = int(a.base, 16)
    image = Path(a.bin).read_bytes()
    syms = collect_symbols(image, base, a.syms)
    Path(a.elf).write_bytes(build_elf(image, base, syms))
    print(f"{a.elf}: {len(image)} bytes @ 0x{base:08x}, {len(syms)} symbols")


if __name__ == "__main__":
    main()
