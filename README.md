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
| [uv](https://docs.astral.sh/uv/) | Python + dependencies for all tools | `brew install uv` |
| `make` | build pipeline | Xcode command line tools |
| `xcrun llvm-objdump` | disassembly (`make disasm`) | Xcode |
| [SysEx Librarian](https://www.snoize.com/SysExLibrarian/) | uploading from a GUI (optional fallback) | already installed in `/Applications` |
| ARM linker (`arm-none-eabi-gcc`) | compiling **new C code** into the firmware (later) | `brew install --cask gcc-arm-embedded` |
| Ghidra + Java (optional) | better ARM decompilation in REA | `brew install --cask ghidra` |
| REA MCP server | AI-assisted reverse engineering in Claude Code | `npm install` (already in `package.json`) |

---

## Python environment (uv)

This project uses [uv](https://docs.astral.sh/uv/) to manage Python and its
dependencies. Everything is set up in `pyproject.toml`, `uv.lock` (exact
versions) and `.python-version` (Python 3.12).

```sh
uv sync                                   # one-time setup: creates .venv/ with mido + python-rtmidi
uv run python tools/lkfw.py info firmware/original/launchkeyminimk4-firmware-1563.syx
uv run tools/send_syx.py --inquiry        # any tool runs the same way
```

- `uv run …` uses the project environment automatically. You don't need to
  activate anything, and it runs `uv sync` first if needed.
- The Makefile already calls every tool through `uv run python`.
- To add a dependency, run `uv add <package>`. This updates `pyproject.toml` and
  `uv.lock`; commit both.
- `.venv/` is gitignored. Delete it at any time and run `uv sync` to recreate it.
- The tools are plain scripts under `tools/`, so no package is built or installed
  (`[tool.uv] package = false`).

---

## Quick start

```sh
uv sync         # once: Python environment
make            # unpack the original .syx -> build/lkmini4-1563.bin (+ .json, + .elf)
make verify     # prove the SysEx codec round-trips the original byte-for-byte
make disasm     # full Thumb disassembly -> build/lkmini4-1563.s
make patched    # apply patches/*.patch, repack -> build/patched.syx (CRC recalculated)
make inquiry    # ask the connected Launchkey for its model + firmware version
make upload     # build + flash build/patched.syx (asks before flashing)
make restore    # flash the untouched original firmware
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
tools/send_syx.py       uploads a .syx to the device over MIDI (with acknowledgement handshake)
tools/bitmap.py         lists / exports / patches the OLED graphics
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

### Editing screen graphics (example: boot logo)

All OLED graphics are stored in the firmware's image table, so you can change
them without writing any code:

```sh
python3 tools/bitmap.py list   build/lkmini4-1563.bin              # all images: id, address, size
python3 tools/bitmap.py export build/lkmini4-1563.bin 0x27f x.txt  # dump as ASCII art
```

Edit the ASCII art (`#` = pixel on, `.` = off) without changing its size.
`assets/boot_logo.txt` is the boot logo (image `0x27F`, 128×48). `make patched`
regenerates `patches/010-boot-logo.patch` from it automatically. The original
logo is saved in `assets/boot_logo.orig.txt`.

---

## Uploading firmware to the Launchkey Mini MK4

> **Status:** the update protocol below was reverse engineered from firmware
> 1563. Model/version queries have been tested on a real Launchkey Mini MK4 25.
> **No modified firmware has been flashed yet.** Update this section once the
> first upload has been done.

### How updating works

You don't need a special boot mode. The **running firmware** receives the update
over its **"MIDI"** USB port (not the "DAW" port):

1. The computer sends a start message (`0x71`) and a header (`0x7C`) with size,
   CRC and version.
2. The device replies **"ready"**. It may show *Press to Upgrade* first and
   wait for you to confirm on the keyboard.
3. The computer sends 6,037 data blocks of 32 bytes (`0x72`). The device
   **acknowledges every block**, and the display shows *Receiving App…*.
4. The computer sends the last block (`0x73`), which contains the start-up
   vectors. The device checks the CRC and restarts with the new firmware. If the
   check fails, it shows *Upgrade Failed*.

The same firmware file works for the 25- and 37-key models. The firmware
detects which model it's running on at power-up. The device rejects images
larger than 224 KB (`0x38000` bytes).

### Before you start

1. Connect the Launchkey **directly** to the Mac (no hub) with a good cable, and
   make sure the laptop won't go to sleep.
2. **Quit every app that uses the Launchkey**: DAWs, Novation Components, browser
   tabs with Web MIDI. Only one program should talk to the device.
3. Check that the computer can see the device:
   ```sh
   make inquiry
   # Launchkey Mini MK4 25, firmware 1.3 build 1563
   ```
4. Build and check the firmware:
   ```sh
   make patched                                   # -> build/patched.syx
   uv run tools/send_syx.py build/patched.syx --dry-run
   ```

### Upload the patched firmware (recommended: command line)

```sh
make upload
```

This is the same as `uv run tools/send_syx.py build/patched.syx`. The uploader:

1. checks the file's size and CRC, then finds the Launchkey "MIDI" port
   automatically,
2. asks **"Flash this firmware now?"**; answer `y`,
3. sends start + header and **waits up to 120 s for the device's "ready"**. If
   the display shows *Press to Upgrade*, confirm on the keyboard,
4. sends each block and waits for its acknowledgement, showing progress,
5. sends the last block and prints the device's final status reply.

**Don't unplug the keyboard or close the terminal during the upload.** When it
has finished, run `make inquiry` again. The version stays `1.3 build 1563`
because only the image changed, not the version number. To check that the patch
is active, look at the device: for the boot logo patch, unplug and reconnect
it and watch the start screen for the **CFW** badge.

Useful options:

| Option | Effect |
|---|---|
| `--port "MK4 37"` | choose a different port (substring of the name) |
| `--yes` | don't ask for confirmation |
| `--confirm-timeout 300` | wait longer for the "ready" / button press |
| `--ack-timeout 5` | wait longer for each block acknowledgement |
| `--no-ack --delay-ms 20` | blind mode: ignore acknowledgements and pause between messages |
| `--list` | list all MIDI ports |

If the uploader reports **no "ready"**, nothing has been written yet. Check that
no other app is using the port and try again. If it stops **in the middle** of
the upload, don't unplug the device. Run `make upload` (or `make restore`)
again right away.

### Go back to the original firmware

```sh
make restore      # flashes firmware/original/launchkeyminimk4-firmware-1563.syx
```

You can also use [Novation Components](https://components.novationmusic.com),
which installs the official firmware.

### Alternative: SysEx Librarian (GUI)

This option sends without waiting for acknowledgements, so use it only if the
command line doesn't work.

1. Open **SysEx Librarian** and select the Launchkey's **MIDI** port as the
   destination (not "DAW").
2. In *Preferences → Transmitting*, set a **delay between messages of at least 20 ms**.
3. Drag `build/patched.syx` into the window, select it and click **Play**.
4. If the display shows *Press to Upgrade*, confirm on the device. Messages sent
   before you confirm may be ignored. If the upload then fails, send the file
   again after confirming.

---

## Safety & recovery

- The update only rewrites the application area (from `0x08008000`). The
  bootloader at `0x08000000` stays the same, so the device should still accept
  a new upload even when a patched image fails to start.
- The uploader only starts sending data after the device has replied "ready", and
  it stops at the first missing acknowledgement.
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
- The confirmed button combination for bootloader mode.
