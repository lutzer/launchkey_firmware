# Launchkey Mini MK4 firmware workbench
#
#   make            unpack original + build analysis ELF
#   make verify     prove the syx codec round-trips byte-for-byte
#   make patched    apply patches/*.patch and repack -> build/patched.syx
#   make disasm     full Thumb disassembly -> build/lkmini4-1563.s
#   make inquiry    ask the connected Launchkey for model + firmware version
#   make upload     build + flash build/patched.syx (asks for confirmation)
#   make restore    flash the untouched original firmware

VERSION  := 1563
ORIG     := firmware/original/launchkeyminimk4-firmware-$(VERSION).syx
BIN      := build/lkmini4-$(VERSION).bin
ELF      := build/lkmini4-$(VERSION).elf
SYMS     := analysis/symbols.txt
PATCHES  := $(sort $(wildcard patches/*.patch))
PY       := uv run python
OBJDUMP  := xcrun llvm-objdump

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
	$(PY) tools/bitmap.py patch $(BIN) 0x27f $< -o $@

build/patched.bin: $(BIN) $(PATCHES) tools/apply_patches.py
	$(PY) tools/apply_patches.py $(BIN) $@ $(PATCHES)

patched: build/patched.bin
	$(PY) tools/lkfw.py pack $< build/patched.syx --meta $(BIN).json
	$(PY) tools/lkfw.py verify build/patched.syx

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

.PHONY: all verify disasm patched inquiry upload restore clean
