# Launchkey Mini MK4 firmware workbench
#
#   make            unpack original + build analysis ELF
#   make verify     prove the syx codec round-trips byte-for-byte
#   make patched    apply patches/*.patch and repack -> build/patched.syx
#   make disasm     full Thumb disassembly -> build/lkmini4-1563.s

VERSION  := 1563
ORIG     := firmware/original/launchkeyminimk4-firmware-$(VERSION).syx
BIN      := build/lkmini4-$(VERSION).bin
ELF      := build/lkmini4-$(VERSION).elf
SYMS     := analysis/symbols.txt
PATCHES  := $(sort $(wildcard patches/*.patch))
PY       := python3
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

build/patched.bin: $(BIN) $(PATCHES) tools/apply_patches.py
	$(PY) tools/apply_patches.py $(BIN) $@ $(PATCHES)

patched: build/patched.bin
	$(PY) tools/lkfw.py pack $< build/patched.syx --meta $(BIN).json
	$(PY) tools/lkfw.py verify build/patched.syx

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all verify disasm patched clean
