# Launchkey Mini MK4 Firmware Workbench

Tools for unpacking, analysing, patching and repacking the firmware of the
**Novation Launchkey Mini MK4** (version 1563), plus instructions for uploading
a modified build to the device.

> ⚠️ **Use at your own risk.** Flashing modified firmware can leave the device
> unusable and may void your warranty. Read [Safety & recovery](#safety--recovery)
> before your first upload.

---

## Requirements

| Tool | Needed for | Install |
|---|---|---|
| Python 3.10+ | everything | already installed |
| `make` | build pipeline | Xcode command line tools |
| `xcrun llvm-objdump` | disassembly (`make disasm`) | Xcode |
| `mido` + `python-rtmidi` | uploading from the command line (optional) | `pip3 install -r requirements.txt` |
| [SysEx Librarian](https://www.snoize.com/SysExLibrarian/) | uploading from a GUI (optional) | already installed in `/Applications` |
| ARM linker (`arm-none-eabi-gcc`) | compiling **new C code** into the firmware (later) | `brew install --cask gcc-arm-embedded` |
| Ghidra + Java (optional) | better ARM decompilation in REA | `brew install --cask ghidra` |
| REA MCP server | AI-assisted reverse engineering in Claude Code | `npm install` (already in `package.json`) |

---

## Quick start

```sh
make            # unpack the original .syx -> build/lkmini4-1563.bin (+ .json, + .elf)
make verify     # prove the SysEx codec round-trips the original byte-for-byte
make disasm     # full Thumb disassembly -> build/lkmini4-1563.s
make patched    # apply patches/*.patch, repack -> build/patched.syx (CRC recalculated)
make clean      # delete build/
```

With no patches in `patches/`, `build/patched.syx` is **byte-identical** to the
original file. This makes it a safe file for a first test upload.

---

## Project layout

```
firmware/original/      vendor .syx files – never edit these
tools/lkfw.py           SysEx codec: unpack / pack / info / verify
tools/mkelf.py          wraps the raw image in an ARM ELF for disassemblers
tools/apply_patches.py  applies patches/*.patch with original-byte checks
tools/send_syx.py       uploads a .syx to the device over MIDI
analysis/symbols.txt    function/data names you discover (fed into the ELF)
analysis/NOTES.md       findings log and open questions
patches/                your patches (*.patch), applied in alphabetical order
build/                  generated files (gitignored)
CLAUDE.md               project context for Claude Code
```

---

## How the firmware file works

The `.syx` file is a sequence of Novation SysEx messages (`F0 00 20 29 00 <cmd> … F7`):

| cmd | Role | Contents |
|---|---|---|
| `0x71` | start | `02 13` + 6 ID bytes (`01 03 0c 1b 0d 35`, meaning unknown) |
| `0x7C` | header | `00` + the same 6 ID bytes + image size (8 nibbles) + CRC (8 nibbles) |
| `0x72` ×6037 | data | 37 7-bit bytes = MSB-first bitstream carrying 32 image bytes |
| `0x73` | final data | image bytes `0x00–0x1F` (the vector table is sent **last**) |

- CRC: **CRC-32/MPEG-2** (poly `0x04C11DB7`, init `0xFFFFFFFF`, no reflection) over the image.
- The image is ARM Cortex-M4 Thumb code for an STM32F4-family MCU, linked at
  **`0x08008000`**. A 32 KiB bootloader at `0x08000000` is **not** part of the
  update and is never overwritten by it.

`lkfw.py` manual use:

```sh
python3 tools/lkfw.py info   firmware/original/launchkeyminimk4-firmware-1563.syx
python3 tools/lkfw.py unpack firmware/original/launchkeyminimk4-firmware-1563.syx out.bin
python3 tools/lkfw.py pack   out.bin out.syx --meta out.bin.json
python3 tools/lkfw.py verify out.syx
```

---

## Analysing the firmware

### With Claude Code + REA

The REA MCP server can open `build/lkmini4-1563.elf` with the **Hopper**
provider. In Claude Code, ask things like:

- "Open build/lkmini4-1563.elf in REA and find the SysEx input handler"
- "Decompile the function that references the string `confsys.c`"

Whenever you identify a function, add it to `analysis/symbols.txt`:

```
0x08012340  midi_sysex_dispatch
0x20001000  g_config            data
```

Then run `make` again. The rebuilt ELF and disassembly will show the names.
Write your findings in `analysis/NOTES.md`.

### With other tools

The ELF loads in Ghidra, IDA, Hopper and Binary Ninja, and `llvm-objdump` and
`arm-none-eabi-objdump` can read it too. If a tool wants a raw binary instead,
use `build/lkmini4-1563.bin` with these settings: ARM Cortex-M4,
little endian, Thumb, base `0x08008000`.

---

## Writing patches

A patch file in `patches/` contains one edit per line:

```
# <address>   <original bytes>   <new bytes>
0x08012344    0b4b               00bf        # replace ldr with nop
0x080372b4    -                  70470000    # '-' = write into free (0xFF) space
```

- Addresses are absolute (flash addresses starting at `0x08008000`).
- The original bytes are checked before anything is written. If they don't
  match (for example, after a firmware update), the build stops with an error.
- Thumb instructions are little-endian. `00bf` = `nop`, `7047` = `bx lr`.
- To get the bytes for a few instructions, assemble them with clang:
  ```sh
  echo 'movs r0, #1
  bx lr' | xcrun clang -target thumbv7em-none-eabi -mcpu=cortex-m4 -c -x assembler - -o /tmp/p.o
  xcrun llvm-objdump -d /tmp/p.o
  ```
- ⚠️ We haven't yet checked whether the flash after `0x080372B3` is free or
  used for settings. Don't put code there until `analysis/NOTES.md` confirms it.

`make patched` → `build/patched.syx`, ready to upload.

---

## Uploading firmware to the Launchkey Mini MK4

> These upload steps are **not yet tested on a real device**. Update this
> section once a method has been confirmed to work.

### Before you start

1. Keep a copy of the original `.syx` (it's in `firmware/original/`).
2. Connect the Launchkey **directly** to the Mac with a good USB cable. Avoid
   hubs.
3. Close every app that uses the Launchkey's MIDI ports: DAWs, Novation
   Components, browsers with Web MIDI tabs.
4. Always build with `make patched`. It recalculates and checks the CRC.

### Recommended first test: upload the unmodified firmware

Before you flash anything modified, run a full upload with **no patches** in
`patches/`. That `build/patched.syx` is byte-identical to the vendor file, so
this test checks only the upload process and leaves the firmware itself
unchanged.

### Option A: SysEx Librarian (GUI)

1. Open **SysEx Librarian**.
2. Under *Destination*, select the Launchkey's **MIDI** port (not the "DAW" port).
3. Drag `build/patched.syx` into the window.
4. Open *Preferences → Transmitting* and set a **delay between messages of
   about 20 ms** or more. Firmware messages that are sent too fast can be dropped.
5. Select the file and click **Play**. Don't unplug the device until it finishes.
6. The device should restart with the new firmware.

### Option B: command line

```sh
pip3 install -r requirements.txt
python3 tools/send_syx.py --list                                  # find the port name
python3 tools/send_syx.py build/patched.syx --dry-run             # validate only
python3 tools/send_syx.py build/patched.syx --port "Launchkey Mini MK4 MIDI" --delay-ms 20
```

The script checks size and CRC before sending, then sends one message at a time.
A full upload takes about 2 minutes at 20 ms per message.

### Option C: Novation Components (official)

[Novation Components](https://components.novationmusic.com) installs official
firmware only. Use it to return to stock firmware.

### Checking the result

- Watch the device's response with SysEx Librarian's *Record* function or with a
  MIDI monitor. This can show whether the device acknowledges messages. Note
  your findings in `analysis/NOTES.md`.
- Novation devices report their firmware version on request (Universal Device
  Inquiry `F0 7E 7F 06 01 F7`).

---

## Safety & recovery

- The update only rewrites the application area (from `0x08008000`). The
  bootloader at `0x08000000` stays the same, so the device should still accept
  a new upload even when a patched image fails to start.
- If the device doesn't start after an upload:
  1. Unplug it and plug it back in.
  2. Put it into **bootloader / update mode**. Novation devices enter this
     mode when you hold certain buttons while connecting USB. Check Novation's
     support pages for the Launchkey Mini MK4 combination, then add it here.
  3. Re-upload the original file (`firmware/original/…syx`) or use Novation
     Components.
- Change only one thing per upload so you always know what broke.
- Never upload a file that fails `python3 tools/lkfw.py verify`.

---

## Status / open questions

See [`analysis/NOTES.md`](analysis/NOTES.md). The most important ones:

- The exact MCU part number and flash size, and where settings are stored. These
  decide whether free flash can be used for new code.
- What the 6 ID bytes in the header mean, and whether the bootloader checks them.
- The confirmed button combination for bootloader mode.
