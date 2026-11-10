# Analysis notes – Launchkey Mini MK4 fw 1563

## Confirmed

- SysEx container decoded; see CLAUDE.md. `make verify` and the CRC check both pass.
- Load address 0x08008000 confirmed: VTOR write in Reset_Handler, and the most Thumb pointers resolve to function prologues at this base.
- 56 external IRQ vectors populated → STM32F4 family (F401/F411-class likely, not confirmed).
- Hopper (via REA) finds 2096 procedures.

## Open questions

- [ ] Exact MCU part / flash size (look for the DBGMCU IDCODE read at 0xE0042000 and the flash-size register).
- [ ] Where settings are stored (`serstor_hw.c`, `confsys.c`): which flash sector? Is 0x080372B4–0x0803FFFF free for code?
      (RAM side and save mechanism are understood, see "Settings storage"; the flash sector is still unknown.)
- [x] ID bytes `01 03 0c 1b 0d 35` = firmware version: 1.3, build 1563 (0x0c<<7|0x1b), 1717 (0x0d<<7|0x35).
      Same values as u32s at 0x080081F0 (`g_fw_version`). Still unknown whether the bootloader checks them.
      App side: boot screen prints all 4 fields; inquiry reply (0x080115B6) sends major, minor, build only;
      0x7C header version is just stored in the update state (0x0801B92C), not compared. Patched builds put the
      patch count in field 4 (see apply_patches.py).
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

## Settings storage / feature params (fw 1563)

- `confsys_set_param(group, id, {value, flags}*, r3)` @0x08016C88 / `confsys_get_param` @0x08016C50 dispatch on
  `(group >> 4) & 0xF`; group 0xD0 = "feature" params (midiobj_feature.cpp) -> `feat_set_param` @0x08026D64.
- 128 feature params, flag byte per id in `g_feat_flags` @0x2000E094: bit0 = defined, bit1 = **persistent**,
  bit2 = 14-bit (low 7 bits at +0x20). A set call with flags bit0 is a *definition* (first one wins, no filters/
  listeners); persistent defs are skipped once the store has loaded.
- Persistent values live in `g_feat_persist` @0x2000DD8C (0x84 bytes, magic 0xAA at +0x80), registered with
  `confstore_register(key 0xFEA70BE5, ptr, 0x84, 3000 ms)`. Volatile ones live in @0x2000E014.
  A changed persistent value calls `confstore_mark_dirty` -> written to flash ~3 s later.
- On boot `confstore_poll` loads the blob, then `feat_on_store_loaded` @0x08026CAC re-sets every defined param
  (non-definition set), so filters/listeners see the restored values.
- Settings menu params are defined with flags = 3 (feature_settings.cpp, e.g. 0x0801EFF8).
- RAM 0x200015D0..0x2000F518 is zero-initialised at startup (scatter table @0x08033544).

## Scale (fw 1563)

- Feature params: 0x4A scale on/off (0 / 0x7F), 0x3E scale type (1..0x1E, 12-byte interval masks @0x0801F7C0),
  0x3D root note (0..11), 0x3C scale mode (0..2: snap / filter / easy).
- `feat_scale_init` @0x0801F750 defines them (flags=1, default off / Major / C / 0); `feat_scale_filter` @0x0801F6B0
  pushes values into the scale engine `g_scale` @0x2000ECE4 (scale.cpp: +0 enabled, +4 12-byte map,
  +0x10 mode, +0x14 root, +0x18 listeners).

## Patches

- `010-boot-logo.patch` – generated from `assets/boot_logo.txt`: adds an inverted "CFW" badge right of the
  Novation logo (160 bytes, data only, image size unchanged). **Status: built, not yet tested on hardware.**
- `020-persist-scale.patch` – 1 byte at 0x0801F75E: scale param definitions use flags 3 instead of 1, so on/off,
  type, root and mode are stored in the persistent feature blob and restored at power-on (same mechanism as the
  Settings menu). Devices with an existing settings record read 0 there on first boot = factory defaults.
  **Status: built, not yet tested on hardware.**

## Update protocol (app side, handler 0x08030E00)

- 0x71 (needs `02 13`) -> state 1; 0x7C header -> state 2, stores size/CRC/version; then (immediately or after
  confirmation, "Press to Upgrade") state 3 and a ready ack `00 20 29 00 72 n0 n1 n2` (n = next offset >> 5).
- Every 0x72 block is written and acked with n+1; 0x73 (vector head) finishes; status reply `... 00 73 st 7F 7F`.
- Max image 0x38000. UI strings: "Entering Upgrade", "Press to Upgrade", "Receiving App...", "Upgrade Failed".
- [ ] Which button confirms "Press to Upgrade", and when is the confirmation skipped (flag checked in 0x7C path)?
- [ ] Meaning of the 0x73 status byte.
