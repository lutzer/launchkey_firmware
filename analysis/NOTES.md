# Analysis notes – Launchkey Mini MK4 fw 1563

## Confirmed

- SysEx container decoded; see CLAUDE.md. `make verify` and the CRC check both pass.
- Load address 0x08008000 confirmed: VTOR write in Reset_Handler, and the most Thumb pointers resolve to function prologues at this base.
- 56 external IRQ vectors populated → STM32F4 family (F401/F411-class likely, not confirmed).
- Hopper (via REA) finds 2096 procedures.

## Open questions

- [ ] Exact MCU part / flash size (look for the DBGMCU IDCODE read at 0xE0042000 and the flash-size register).
- [ ] Where settings are stored (`serstor_hw.c`, `confsys.c`): which flash sector? Is 0x080372B4–0x0803FFFF free for code?
- [ ] Meaning of the id bytes `01 03 0c 1b 0d 35` in 0x71/0x7C (does the bootloader check them?).
- [ ] Does the bootloader accept an image larger than the original (code-cave growth)?
- [ ] MIDI input dispatch / SysEx handler (entry point for new functions).
- [ ] Recovery: bootloader mode button combo, in case a patched image doesn't boot.
