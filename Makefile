# Launchkey Mini MK4 firmware workbench
#
#   make            unpack original + build analysis ELF
#   make verify     prove the syx codec round-trips byte-for-byte
#   make patched    apply patches/*.patch, stamp version 1.3.1563.<VERSION file>, repack -> build/patched.syx
#                   (one-off: make patched REV=7)
#   make release    build/launchkeyminimk4-cfw-<version>.zip: uploader.html + the patched .syx
#   make disasm     full Thumb disassembly -> build/lkmini4-1563.s
#   make inquiry    ask the connected Launchkey for model + firmware version
#   make upload     build + flash build/patched.syx (asks for confirmation)
#   make restore    flash the untouched original firmware
#   make cfw        compile cfw/*.c into the code cave -> patches/100-cfw-code.patch, 101-cfw-hooks.patch
#   make cfw-test   run the host unit tests of the cfw logic

VERSION  := 1563
ORIG     := firmware/original/launchkeyminimk4-firmware-$(VERSION).syx
BIN      := build/lkmini4-$(VERSION).bin
ELF      := build/lkmini4-$(VERSION).elf
SYMS     := analysis/symbols.txt
CFW_PATCHES := patches/100-cfw-code.patch patches/101-cfw-hooks.patch
PATCHES  := $(sort $(wildcard patches/*.patch) $(CFW_PATCHES))
REV      ?= $(shell cat VERSION)
PY       := uv run python
OBJDUMP  := xcrun llvm-objdump

# cfw: C code linked into the free flash after the stock image (cfw/cfw.ld)
ARMCC    := xcrun clang -target thumbv7em-none-eabi -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
ARMFLAGS := -std=c11 -Os -fomit-frame-pointer -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Ibuild/cfw
LLD      := ld.lld
CFW_SRC  := $(wildcard cfw/*.c)
CFW_OBJ  := $(patsubst cfw/%.c,build/cfw/%.o,$(CFW_SRC))
CFW_HOOK_SYMS := $(shell awk '!/^[[:space:]]*(\#|$$)/ {print "-u " $$3}' cfw/hooks.txt)

all: $(ELF)

$(BIN): $(ORIG) tools/lkfw.py | build
	$(PY) tools/lkfw.py unpack $< $@

$(ELF): $(BIN) $(SYMS) tools/mkelf.py
	$(PY) tools/mkelf.py $< $@ --syms $(SYMS)

verify: $(ORIG)
	$(PY) tools/lkfw.py verify $<

disasm: $(ELF)
	$(OBJDUMP) -d --triple=thumbv7em-none-eabi --mcpu=cortex-m4 $< > build/lkmini4-$(VERSION).s
	@echo "-> build/lkmini4-$(VERSION).s"

# Bitmap patches are generated from editable ASCII art in assets/
patches/010-boot-logo.patch: assets/boot_logo.txt $(BIN) tools/bitmap.py
	$(PY) tools/bitmap.py patch $(BIN) 0x27f $< -o $@ --title "Boot logo: small CF badge over the Novation logo"

build/cfw/%.o: cfw/%.c $(wildcard cfw/*.h) | build
	@mkdir -p build/cfw
	$(ARMCC) $(ARMFLAGS) -c $< -o $@

# CFW_REV = REV for the boot screen; the header is rewritten only when REV changes
build/cfw/cfw_rev.h: FORCE | build
	@mkdir -p build/cfw
	@echo '#define CFW_REV $(REV)' | cmp -s - $@ || echo '#define CFW_REV $(REV)' > $@

build/cfw/cfw_init.o: build/cfw/cfw_rev.h

build/cfw.elf: $(CFW_OBJ) cfw/cfw.ld cfw/fw_api.ld
	$(LLD) -T cfw/cfw.ld -L cfw --gc-sections -e cfw_init $(CFW_HOOK_SYMS) -Map build/cfw.map -o $@ $(CFW_OBJ)

# one mkcave run writes both files (make 3.81 has no grouped targets)
patches/100-cfw-code.patch: build/cfw.elf cfw/hooks.txt $(BIN) tools/mkcave.py
	$(PY) tools/mkcave.py build/cfw.elf cfw/hooks.txt $(BIN) $(CFW_PATCHES)
patches/101-cfw-hooks.patch: patches/100-cfw-code.patch

cfw: $(CFW_PATCHES)

build/test_tm_core: cfw/test/test_tm_core.c cfw/tm_core.c cfw/tm_core.h | build
	clang -std=c11 -Wall -Wextra -Werror -O1 -o $@ cfw/test/test_tm_core.c cfw/tm_core.c

cfw-test: build/test_tm_core
	$<

# Version field 4 = REV (VERSION file): stock 1.3.1563.1717 -> 1.3.1563.<REV> in the SysEx header and on the
# boot screen (cfw); the image keeps 1717 so Novation Components sees stock. Always re-applied (FORCE,
# ~1 s), so a changed VERSION or a REV= override can never be skipped; `patched` repacks every time anyway.
build/patched.bin: $(BIN) $(PATCHES) tools/apply_patches.py FORCE
	$(PY) tools/apply_patches.py $(BIN) $@ --meta $(BIN).json --level $(REV) $(PATCHES)

patched: build/patched.bin
	$(PY) tools/lkfw.py pack $< build/patched.syx --meta $<.json
	$(PY) tools/lkfw.py verify build/patched.syx

# Release bundle: the patched firmware as launchkeyminimk4-cfw-<version>.syx + the Web MIDI uploader
RELEASE  := launchkeyminimk4-cfw-1.3.$(VERSION).$(REV)

release: patched
	rm -rf build/release build/$(RELEASE).zip
	mkdir -p build/release
	cp build/patched.syx build/release/$(RELEASE).syx
	cp tools/uploader.html build/release/uploader.html
	cd build/release && zip -q -X ../$(RELEASE).zip $(RELEASE).syx uploader.html
	@echo "-> build/$(RELEASE).zip"

build:
	mkdir -p build

clean:
	rm -rf build

inquiry:
	$(PY) tools/send_syx.py --inquiry

upload: patched
	$(PY) tools/send_syx.py build/patched.syx

restore: $(ORIG)
	$(PY) tools/send_syx.py $(ORIG)

FORCE:

.PHONY: all verify disasm patched release inquiry upload restore clean cfw cfw-test
