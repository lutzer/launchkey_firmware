# Analysis notes – Launchkey Mini MK4 fw 1563

## Confirmed

- SysEx container decoded; see CLAUDE.md. `make verify` and the CRC check both pass.
- Load address 0x08008000 confirmed: VTOR write in Reset_Handler, and the most Thumb pointers resolve to function prologues at this base.
- 56 external IRQ vectors populated → STM32F4 family (F401/F411-class likely, not confirmed).
- Hopper (via REA) finds 2096 procedures.

## Open questions

- [ ] Exact MCU part / flash size (look for the DBGMCU IDCODE read at 0xE0042000 and the flash-size register).
- [ ] Where settings are stored (`serstor_hw.c`, `confsys.c`): which flash sector? Is 0x080372B4–0x0803FFFF free for code?
- [x] ID bytes `01 03 0c 1b 0d 35` = firmware version: 1.3, build 1563 (0x0c<<7|0x1b), 1717 (0x0d<<7|0x35).
      Same values as u32s at 0x080081F0 (`g_fw_version`). Still unknown whether the bootloader checks them.
- [ ] Does the bootloader accept an image larger than the original (code-cave growth)?
- [ ] MIDI input dispatch / SysEx handler (entry point for new functions).
- [ ] Recovery: bootloader mode button combo, in case a patched image doesn't boot.

## Display (fw 1563)

- 128x64 OLED, SSD1306-style page format (byte = 8 vertical px, LSB top).
- UI layer: screens built from templates (`g_screen_templates` @0x08032994, 8 bytes/entry: nfields, ?, ptr to
  12-byte field descriptors: x u16?, width, page, pages, ..., buf offset @9, buf len @10).
  `disp_set_screen(screen, template)`, `disp_set_field_text(screen, field, str)`, `disp_set_field_num(screen, field, val)`.
  Field type 1 = text, 2 = image id, 3 = 3-word struct.
- `disp_render_template` turns fields into a display-list byte stream (ring buffer);
  `dl_interpret` @0x0801244C decodes it locally (opcode 0x7x = image) and pulls bitmaps from flash.
- Image table `g_image_table` @0x0802DF94, 640 entries: `off:19 | pages:5 | width:8`, data base 0x0800E600.
  `tools/bitmap.py list/export/patch`.
- Boot screen `ui_show_boot_screen` @0x08016E04 (called from 0x080103E6): template 11 =
  image 0x27F (128x48 Novation logo) + text line "Ver: " + version at page 6.
- Strings: `str_get(id)` @0x08015B0C, 226 English strings from 0x0802E994 (0xA7 "Launchkey", 0xA8 "Ver: ").

## Patches

- `010-boot-logo.patch` – generated from `assets/boot_logo.txt`: adds an inverted "CFW" badge right of the
  Novation logo (160 bytes, data only, image size unchanged). **Status: built, not yet tested on hardware.**

## Update protocol (app side, handler 0x08030E00)

- 0x71 (needs `02 13`) -> state 1; 0x7C header -> state 2, stores size/CRC/version; then (immediately or after
  confirmation, "Press to Upgrade") state 3 and a ready ack `00 20 29 00 72 n0 n1 n2` (n = next offset >> 5).
- Every 0x72 block is written and acked with n+1; 0x73 (vector head) finishes; status reply `... 00 73 st 7F 7F`.
- Max image 0x38000. UI strings: "Entering Upgrade", "Press to Upgrade", "Receiving App...", "Upgrade Failed".
- [ ] Which button confirms "Press to Upgrade", and when is the confirmation skipped (flag checked in 0x7C path)?
- [ ] Meaning of the 0x73 status byte.
