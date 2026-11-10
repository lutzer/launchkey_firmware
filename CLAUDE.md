# Launchkey Mini MK4 firmware workbench

Goal: analyse Novation Launchkey Mini MK4 firmware (v1563) and add functions by patching the binary.

## Layout

- `firmware/original/` – untouched vendor `.syx` files (never edit)
- `tools/lkfw.py` – SysEx codec: `unpack`, `pack`, `info`, `verify` (pack recomputes size + CRC)
- `tools/mkelf.py` – wraps the raw image in an ARM ELF (base 0x08008000, Thumb, vector + `analysis/symbols.txt` names)
- `tools/apply_patches.py` – applies `patches/*.patch` (`<addr> <orig hex|-> <new hex>`, orig bytes are verified)
- `analysis/symbols.txt` – names we discover; rebuilt into the ELF by `make`
- `analysis/NOTES.md` – findings log (keep updated)
- `build/` – generated (gitignored): `.bin`, `.bin.json`, `.elf`, `.s`, `patched.syx`
- `make` / `make verify` / `make disasm` / `make patched`

## Container format (verified: round-trip byte-identical, CRC matches)

- Messages `F0 00 20 29 00 <cmd> … F7` (Novation ID).
- `0x71` start: `02 13` + 6 id bytes `01 03 0c 1b 0d 35` (meaning unknown, copied verbatim on pack).
- `0x7C` info: `00` + same 6 bytes + size (8 nibbles) + CRC (8 nibbles).
- `0x72` data: 37 7-bit bytes = MSB-first bitstream → 32 image bytes (+3 zero bits). Carries image[0x20:], last chunk 0xFF-padded.
- `0x73` final data: image[0x00:0x20] (vector-table head sent last).
- CRC = CRC-32/MPEG-2 (poly 04C11DB7, init FFFFFFFF, no reflect/xorout) over image[:size].

## Target facts

- ARM Cortex-M4 (Thumb-2), STM32F4-family (FLASH regs @0x40023C00, RCC @0x40023800, USB OTG_FS @0x50000000).
- App linked at **0x08008000** (Reset_Handler writes VTOR=0x08008000); bootloader occupies 0x08000000–0x08007FFF and is NOT part of the update.
- Initial SP 0x2000FFF8 (64 KiB SRAM). Image ends 0x080372B3 (193 203 bytes). Reset_Handler 0x08033481.
- Source-file strings: confsys.c, confsys_init.c, extdata_*.c, io_listener.c, muxio_hw_data.c, serstor_hw.c.

## Analysis tooling

- REA MCP: Hopper provider works on `build/lkmini4-1563.elf` (`open_binary` with `provider_id: "hopper"`).
  Raw-binary mode needs Ghidra (not installed). Re-run `make` and reopen after editing `symbols.txt`.
- `xcrun llvm-objdump` (Xcode) disassembles; `xcrun clang -target thumbv7em-none-eabi -mcpu=cortex-m4` compiles.
  No ARM linker is installed yet (needed for C code caves).

## Rules

- Never flash anything that hasn't passed `make patched` (CRC + round-trip verify).
- Before using space after 0x080372B3 as a code cave, confirm it's not used for settings storage (see NOTES open questions).
