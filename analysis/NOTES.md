# Analysis notes – Launchkey Mini MK4 fw 1563

## Confirmed

- SysEx container decoded; see CLAUDE.md. `make verify` and the CRC check both pass.
- Load address 0x08008000 confirmed: VTOR write in Reset_Handler, and the most Thumb pointers resolve to function prologues at this base.
- 56 external IRQ vectors populated → STM32F4 family (F401/F411-class likely, not confirmed).
- Hopper (via REA) finds 2096 procedures.

## Open questions

- [ ] Exact MCU part / flash size (look for the DBGMCU IDCODE read at 0xE0042000 and the flash-size register).
- [x] Where settings are stored: in an **external SPI NOR flash** (`serstor_hw.c` @~0x08011B24 issues 0x02 page
      program, 0x03 read, 0x20/0x52/0xD8 erase, 0x05 status). The app never unlocks internal flash (no FLASH_KEYR
      keys in the image; FLASH regs only touched for ACR latency/caches), so updates are staged in SPI flash and
      installed by the bootloader. -> 0x080372B3..0x0803FFFF of the 0x38000 app slot is free for code
      (36 173 bytes). Exact SPI address map of the settings / update staging area still unknown.
- [x] Bootloader installs a grown image: Phase 0 cfw build (193 332 bytes, code at 0x080372B4, hook replacing
      `bl feat_scale_init`) flashed 2026-11-10, device boots and works normally -> code past the old end runs.
- [x] ID bytes `01 03 0c 1b 0d 35` = firmware version: 1.3, build 1563 (0x0c<<7|0x1b), 1717 (0x0d<<7|0x35).
      Same values as u32s at 0x080081F0 (`g_fw_version`). Still unknown whether the bootloader checks them.
      App side: boot screen prints all 4 fields; inquiry reply (0x080115B6) sends major, minor, build only;
      0x7C header version is just stored in the update state (0x0801B92C), not compared. Patched builds put the
      patch count in field 4 (see apply_patches.py).
- [x] Does the bootloader accept an image larger than the original (code-cave growth)? Yes (see above).
- [ ] MIDI input dispatch / SysEx handler (entry point for new functions).
- [ ] Recovery: bootloader mode button combo, in case a patched image doesn't boot.

## Memory budget (fw 1563)

- Flash: app slot 0x08008000..0x0803FFFF = 0x38000 (updater limit), image 193 203 bytes (84 %), **35.3 KiB free**.
- RAM (64 KiB): RW data 0x20000000..0x200015D0 (decompressed from the image end), zero-init
  0x200015D0..0x2000F518 (scatter table @0x08033544), leaves **2 792 bytes** up to 0x20010000 for the main stack
  (initial SP 0x2000FFF8). Any new feature must find RAM inside existing buffers or share that headroom.

## Modes and layouts (fw 1563)

- Layout handle = `{u32 index; u8 type}` (type 1 = pads, 2 = encoders, 3 = faders; `layout_type` @0x08029CFC).
- `layout_select(handle, kind)` @0x080159C0 stores the index in a feature param: **0x1D pad mode, 0x1E encoder
  mode, 0x1F fader mode** (`confsys_set_param(0xD0, id, {index, 0}, 2)`), so mode changes reach feature listeners.
- `ui_layout_name` @0x0802AE30: pad layouts 5..8 (or 0) = "Custom 1..4"; encoder layouts 6..9 (or 0) =
  "Custom 1..4" -> **encoder Custom 4 = 0x1E value 9**.
- Feature listener slots: 6 of 8 used (`feat_add_listener`), filters 5 of 8.
- Relative input (encoders, inputrelative.cpp): three listener lists of max 4, notified with (id, value, delta):
  `relin_notify_pos` @0x08020E48 (list 0x2000F210, 2 users), `relin_notify_hires` @0x08020E56 (0x2000F224, 1 user),
  `relin_notify_step` @0x08020E8A (0x2000F238, 2 users). Registered at 0x08015FB6.. and 0x08016636...
- **Control ids (probe 2 on hardware, 2026-11-11):** pads 0..7 = top row (left->right), 8..15 = bottom row;
  encoders 1..8 = 56..63 (0x38..0x3F); Shift = 25 (0x19), mirrored in feature param 0x3F (127/0).
  io lists: A = press (value = 14-bit velocity; encoders: touch), C = release, B/D = pressure / value stream.
  relin pos list = every tick with delta, step list = per detent (~2 ticks).
- Encoder mode select = Shift + top-row pad: pad 7 -> 0x1E = 9 (Custom 4), pad 6 -> 8 (Custom 3). The feature
  listener sees it (also 0x45 = 0 each time, meaning unknown). Custom 4 enc 1 sends CC 51 ch 1 by default.
- Mode select with Shift: **top-row pads 0..7 -> encoder mode** (pad n -> 0x1E = n + 2, pad 7 = Custom 4 = 9),
  **bottom-row pads 8..15 -> pad mode** (pad 15 -> 0x1D = 8 = pad Custom 4, pad 12 -> 5 = Custom 1).
  0x1E/0x1D only *report* the mode: vetoing 0x1E with a feature filter does not stop the layout switch.
- `ctl_block` is not usable to own controls across Shift: the Shift overlay captures/releases controls and
  clears the block flags on release. cfw instead replaces the input dispatchers (io lists A..D 0x0801B530/
  B54C/B574/B592, encoder lists 0x08020E48/E56/E8A, all start with a 4-byte push -> `b.w`) and decides per
  event (press owns a control until its release). Verified on hardware (probe 3.3).
- Port mapping: g_app_ports[0] = DAW port, g_app_ports[1] = MIDI port.
- **Keys Channel = feature param 0x64** (0-based). Encoder relin events pass value and delta as u16
  (delta -1 = 0xFFFF) -> sign-extend. Raw encoder io events (A/C/D) must still reach inputrelative's io
  listeners (0x08020E97 / EBB / EDB) or no relin events are generated.
- Feature flags at runtime (pad Custom 4 selected, 2026-11-11): defined 02-07 1b-1f 3c-3f 44-47 49-4f 53-59 5c
  64-77 79-7d; persistent 3c 3d 3e 4a 4f 64-77 79; free 00 01 08-1a 20-3b 40-43 48 50-52 5a 5b 5d-63 78 7e 7f.
- **Clock:** `clock_poll` @0x08013C60 (main loop, before input) keeps state at 0x20000104: +0 running, +2 external
  sync, +8 pending external F8s (`clock_ext_tick` @0x08013C50 adds one per received F8), +0xC accumulator,
  +0x10 tick period in us (clamped 1000..1000000), **+0x20 = 24 PPQN tick counter `g_clock_ticks` 0x20000124**.
  Each ms the accumulator gains 1000 us; reaching the period increments the counter (internal), or each
  pending F8 does (external). Free-running. MIDI Clock Out (`clock_out_poll` @0x08014492, main loop) sends
  one F8 per counter step -> cfw steps from the same counter in cfw_poll.
- Stack: >= 1472 bytes of the painted area (0x2000F620..) never touched -> peak stack use ~1 KB.
- `ctl_block(id)` @0x08016038 / `ctl_unblock` @0x08016048 / `ctl_is_blocked` @0x08016058: per-control flag
  (table 0x2000C410) that makes the app listeners ignore the control; ui_menumgr uses it to capture controls
  (`0x08025712` capture = block + 0x0801BD28(id, 1), `0x08025726` release = unblock + 0x0801BC92(id, 3)).
- MIDI out: `midi_put(port, byte)` @0x08013F8E (USB-MIDI packetiser, SysEx of any length), `midi_send3(port,
  status, d1, d2)` @0x08013F6E. App ports (objects on main's stack) at 0x2000EE40 / 0x2000EE44.
- **Settings menu:** items via `settings_entry(i)` 0x0802B44C: 12-byte entries {u8 string id, u8 feature id,
  u16 0, u32 type (8 = channel, 1 = vel curve, 0 = on/off, 9 = power-on modes, ...), u8 max, u8 steps, u16 0} in
  table 0x08031AB0 (>= 49 keys, 20 items) / +0xF0 (Minis, 17 items); `device_key_count` 0x0801D8CC < 0x31 =
  Mini. Pads select items 0..15 (`settings_count_pads` 0x0802B43A = 16/19); the arrows (34/35) step through
  `settings_count_nav` 0x0802B428 = 17/20 items. cfw (settings.c) adds item 17 "Turing Channel" (feature 0x61,
  type 8) by hooking the accessor and count_nav, and `str_get` 0x08015B0C (id 0xE2 = new name; stock returns
  "" for ids >= 0xE2; strings = lang base table 0x08036A68[lang @0x200015CC] + u16 offsets @0x2000B984).
- **Overview popup template 4:** field 0 title, fields 1..8 = 2 rows x 4 fields of 32 px (encoder labels).
- **Layout names:** `layout_name_text_a` 0x0802AEB8 (mode popups, called at 0x08022548/A32/A58/E8C) and
  `layout_name_text_b` 0x0802AECA (home screen, 0x08024230) write a layout's name into a display field, handle
  {index, type} in r2/r3. cfw hooks those calls so pad Custom 4 reads "Turing Machine".
- **MIDI realtime handler** 0x08030958 (via 0x0803098E): F8 -> `clock_ext_tick`, FA -> `clock_midi_start`
  0x08013C12, FB -> `clock_midi_continue` 0x08013C24, FC -> `clock_midi_stop` 0x08013C1A, F2 accepted (no
  action here). cfw hooks the three transport calls (cfw_midi_*) -> Turing Machine Start = rewind to the first
  loop step (tm_rewind: rotate back by the steps since the loop start) played on the next tick, Continue = resume, Stop = pause (recorded in the hook, applied in tm_mode_poll).
- Main loop @0x0801041A (calls io poll 0x08014502, confstore_poll x5); all input listeners run there.

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
- **Stock feature IDs (static scan, all confsys 0xD0 calls + define thunks 0x0801594A, 0x0801F112, 0x0801F51C,
  0x0801F584, 0x0801F660):** 02-07 (14-bit, written as 0x4002..0x4007, LSBs at 22-27), 1B-1F, 3C-3F, 44-47,
  49-4F, 53-59, 5C, 64-77, 79-7D; persistent 3C 3D 3E 4A 4F 64-77 79. Computed IDs only in layout_select
  (1D-1F), `feat_on_store_loaded` (defined IDs) and the CC handler below. 14-bit aliasing exists only below
  0x40 (`feat_set_param` clears the flag for IDs >= 0x40). New cfw IDs: pick >= 0x40 outside this list.
- The persistent blob has one byte per ID; its confstore record (key 0xFEA70BE5, 0x84 bytes) keeps its size when
  cfw uses more IDs, so the SPI layout and stock values are unaffected. Slots of never-persistent IDs read as
  whatever the record holds (0 on old records) -> cfw uses a marker ID.
- **Feature CCs:** `midi_cc_feature_set` 0x0803050C (MIDI input chain 0x08030304): an incoming CC on MIDI
  **channel 7** sets feature param id = CC number if the ID is defined (`confsys_param_flags` 0x08016D8C),
  CC 0..0x3F as 14-bit MSB/LSB pairs. So CC 80/81/82/90/91/93/94/95/96 on channel 7 *to* the device change
  the Turing Machine parameters, just like CC 100.. change the stock settings.
- **No data in internal flash after the image:** the only literal pointers outside the image are 0x080001F0
  (bootloader read) and the FLASH regs (ACR latency 0x0801057E, caches 0x08010E3E); no FLASH_KEYR. The app slot
  = sectors 2-5 and updates may fill all 0x38000 bytes, so anything stored there would not survive an update.
- RAM 0x200015D0..0x2000F518 is zero-initialised at startup (scatter table @0x08033544).

## Scale (fw 1563)

- Feature params: 0x4A scale on/off (0 / 0x7F), 0x3E scale type (1..0x1E, 12-byte semitone masks @0x08031DE0, max 8 notes),
  0x3D root note (0..11), 0x3C scale mode (0..2: snap / filter / easy).
- `feat_scale_init` @0x0801F750 defines them (flags=1, default off / Major / C / 0); `feat_scale_filter` @0x0801F6B0
  pushes values into the scale engine `g_scale` @0x2000ECE4 (scale.cpp: +0 enabled, +4 12-byte map,
  +0x10 mode, +0x14 root, +0x18 listeners).

## Scale and Arp screens (fw 1563)

- Button presses/releases go through `ui_press_dispatch` 0x080163E0 / `ui_release_dispatch` 0x0801645E, which
  call every view's handler with the control id; each handler checks `ui_menu_owns(id, view)` 0x080257A6
  (table of the capturing view per control). Views capture their controls in their activate callback.
- Control ids: Scale 0x12, **Shift+Scale 0x42**, Arp 0x1E, **Shift+Arp 0x60** (own LED = latch state),
  Chord Map button 0x13 / 0x43, encoder arrows **up 0x25 / down 0x24** (DAW CC 0x33 / 0x34; Shift overlay:
  0x25 = previous page). Ids derived statically and from the user's description of stock behaviour.
- Scale view = menu 0xF, state `g_scale_view` 0x2000F488 {u8 item, u8 shown, u8 pending_off, u8, u32 hold ms}.
  Press of 0x12 **or 0x42**: scale off -> `scale_view_open` (scale on + view); on -> pending_off, and (view not
  shown) hold timer 333 ms; release with pending_off -> view closed + scale off; timer expiry -> view shown.
- Arp view = menu 1, state `g_arp_view` 0x20001094 (.data) {u32 prev pad layout, u8 type, ..., +8 shown,
  +9 pending_off, +0xC hold ms, +0x10 layout ms}. 0x1E like Scale (hold also switches the pads to the arp
  pattern layout after 333 ms); **0x60 toggles feature 0x58 (latch, 0/0x7F, volatile)** + popup "Latch"/"On|Off"
  (`arp_popup_title(0xFF, 0x70)`, `popup_field_text(0xFF, 1, str)`). While shown, the view captures encoders
  0x38..0x3F and both arrows (arrows otherwise unused there, LEDs off). Encoders: tempo (feat 3), swing (2),
  0x56 rate, 0x59 gate, 0x55 type, 0x57 octave, 0x5C, rhythm (4). `arp_leds` 0x08023778 is also the arp
  feature listener.
- View open (stock pattern): `ui_close_views` (all close callbacks), shown = 1, `ui_view_changed`,
  `ui_popup_begin` (activate callbacks re-capture controls and redraw LEDs). Close: `*_view_close` + popup_begin.
- A cfw override of Shift + Scale / Shift + Arp (open the screen without switching off, arrows set the latch;
  `cfw/shift_views.c`, hooks at 0x0801640E, 0x08016444, 0x08023778) was removed again: the stock handlers
  run unchanged. Restore it from git history if needed.
- cfw (`cfw/arp_view.c`): hooks the arp press call in the dispatcher and the entry of `arp_leds`. In the Arp
  view up/down set the latch with the stock popup, and the arrow of the current setting is lit. Stock 0x1E
  press with the arp on sets pending_off; only the hold timer (+0xC, view not shown) or the layout timer (+0x10,
  pad layout != 0xD) clear it on expiry. With the view shown and the pads on layout 0xD neither runs, so a long
  press switched the arp off on release. cfw sets the hold timer whenever pending_off is set and it is 0
  (expiry: pending_off = 0 + `arp_view_open`, which simply re-shows the view).
- Chord Map performance-modifier view = menu 7 (state 0x200010AC): pads 5-7 / 13-15 pick Arp Up/Down,
  Inversion Up/Down, Bass + Chord, Chord Split; control 0x10 toggles their latch ("Latch ON/OFF").

## Turing Machine mode (cfw, pad mode Custom 4)

- `cfw/tm_mode.c` + `cfw/tm_core.c`. Top pads = register bits 0..7 (bit 0 newest = pad 1, green; press flips); yellow = the bit of the first step played after a rewind (Start/Play reset): `tm_start_bit` = (len - 1 + pos) % len; the Function view shows step k of the loop on pad k (`tm_loop_bit` = start bit - k, wrapping),
  bottom pads = scale notes (white; press toggles), encoders 1..8 = probability, length, rate (4/1..1/32,
  CW = faster), range, gate, trigger heads, velocity depth, velocity head; octave on the up/down arrows. Trigger modes (tm_heads, dense -> sparse): Every, OR(0,1/3,2/3), OR(0,1/2), 0,
  XOR(0,1/2), AND(0,1/2), AND(0,1/2)&OR(1/4,3/4), AND(0,1/3,2/3); a head at fraction f reads bit f*len (min 1
  after the first), i.e. bit 0 delayed - fixed bits collapsed on short loops (len 3: bit 3 = bit 0). The note lasts gate % of
  the step (5..95 %) and is always released before the next step (no ties). Encoder 7 = velocity depth: velocity = 100 blended with a 7-bit register window (tm_velocity). Encoder 8 =
  velocity head 0..9 moves that window: lowest bit `tm_vel_shift` = (4 + head) % 10, so head 0 = bits 4..10 (the
  window before the head existed; old records read 0). Encoder 7 was Rotate until 2026-11-12 (removed).
- Parameters are table-driven (`params[]` in tm_mode.c: feature id, range, step, per-tick vs per-detent, popup
  label); `enc_param[]` maps encoders 1..8 to them, so remapping an encoder is one table entry. Every change
  (encoders, octave arrows) goes through `param_change`; `param_apply` clamps and holds the side effects
  (length: tile + pos mod len), so host CC writes behave like the encoders and redraw the LEDs.
  `param_save` fires our own feature listener too; it redraws only when the value really changed (no double draw).
  Probability reacts per tick (finer), the rest per detent.
- Pitch: register value (bits 0..7) picks a pitch 0..range semitones above the root (range 1..48, 12 = one
  octave), quantised to the closest enabled scale note in pitch (tie -> lower, inside 0..range when one exists).
  All off = rest. (Until 2026-11-11 range counted scale notes; stored values carry over as semitones.)
- **Button ids** (DAW-mode CC map 0x0801C180, ctl -> CC: 0x22 -> 106 up, 0x23 -> 107 down, 0x10 -> 104
  scene launch ">", 0x11 -> 105 Function): **">" 16 = enable/disable** (feature 0x41 = disabled, saved; disabled = notes muted
  only (note_on skipped, sounding note released); clock, transport and Play keep running), **Play 0x48 = start/stop** (see below), **Function 17 =
  hold**, **up 34 / down 35 = octave** (stored 0..8, shown -4..+4, 4 = 0 = base MIDI 48 like the keyboard default); loop start = power-on or last Start. Their LEDs are driven through the same override layer (white).
  While Function is held both rows show the loop in play order from its first step (pad 1, yellow) and pad presses flip them.
  Flips, Start rewind and length changes tile the loop over all 16 bits (`tm_tile`: bit k = bit k - len,
  the locked state): pitch reads bits 0..7 and velocity up to bit 15, beyond short loops, so stale copies there made
  edits fade in over several steps.
  The keyboard-octave popup captures the arrows (ctl_is_blocked 34/35), so the four mode buttons only yield
  to a menu that has captured pad 0 too (Settings etc.).
- Bottom pads 8..15 = scale notes 1..8 (pad 15 only for 8-note scales; note 8 stored inverted in 0x62). Hold: while
  held both rows show the live register in loop order: pad k = step k after a rewind = bit `tm_loop_bit(pos, k)`
  (play order is bit len-1 down to 0), so the first step is always pad 1 and a locked loop stands still;
  playhead = the pad whose bit is 0 (step playing now, = step pos - 1), mint (amber on pad 1); a pad press flips that bit. Function + up/down: tm_rewind(1) / tm_rewind(len-1) rotates the loop (step k -> k+1 / k-1, pos unchanged). Replaces the earlier frozen snapshot (frozen_reg / frozen_steps).
- **Background playback:** transport (Play, MIDI Start/Continue/Stop) and stepping run in every pad mode
  (also while disabled = muted); leaving the mode keeps them, `led()` and popups only act while the mode is
  shown. Stopped: tm_mode_poll keeps `last_clock` current.
- **Play button (control 0x48):** stock `play_press` 0x08022710 (press dispatcher, no menu check) sends FA/FC on
  `g_transport` 0x2000F480 {u8 standalone, u8 running, u32 port} when standalone (= no DAW connection, set by
  `transport_daw_changed` 0x08022752 from 0x08026858 == 0; it then captures 0x48/0x49) and toggles running.
  cfw_play_press sets `running` = TM playing before calling it, so FA/FC always match the TM toggle
  (`tm_play_button`: start = rewind like MIDI Start). With a DAW connected only the TM toggles.
- **Shift overlay LEDs:** `shift_overlay_leds` 0x08022A88 (menu 0xE, via 0x08022CD8) draws through `shift_led`
  0x08022A5C (only if `ui_menu_owns(id, 0xE)`, then `led_set` 0x0802A612 = led_override(id, 0) + colour):
  top row = encoder modes, bottom row = pad modes (current 0x0086FF, available 0x001642), 0x28.. other buttons.
  Its last call `shift_leds_tail` 0x0802B0D2 is hooked (cfw_shift_leds): pad 15 (Custom 4) = green
  0x00FF00 enabled / 0x000C00 disabled, in every pad mode. The ">" LED (mode shown) is white bright/dim.
- `ui_popup_begin` 0x08024F5E synchronously runs two callback lists (0x2000EDF0 / 0x2000ED88, max 12 each); they
  repaint button LEDs, e.g. ">" went dark after the enable popup -> cfw redraws its button LEDs after it.
- Shift + encoder: the mode keeps the encoders (io events -> TM_ENC_ONLY, relin consumed) and only shows the value.
- Menus: controls captured by a menu (ctl_is_blocked) go to the menu - except after (re)selecting pad Custom 4:
  Scale/Arp screens keep encoders captured after leaving them, so selecting the mode sets a priority flag
  that wins over captures until any other button (not Shift/pads/encoders/the four mode buttons) is pressed.
- Notes: MIDI port (g_app_ports[1]) on the Turing Channel (feature 0x61, Settings item 17). **DIN/TRS out:** main() inits a
  third port (stack sp+0x50, raw-byte mode, serial ring -> USART6 @31250 baud via DMA) and links it to the MIDI
  port (`port+0x18`, `0x08013EE8`); `midi_send3` walks that chain, so MIDI-port notes also go out on DIN.
- **Persistence:** feature params 0x50 prob, 0x51 length, 0x52 rate, 0x5A octave, 0x5B range, 0x5D gate,
  0x5E trigger heads, 0x5F note mask (7 bits), 0x61 MIDI channel, 0x62 note 8 off, 0x63 velocity depth, 0x42 velocity head, 0x41 disabled
  (inverted: records from before it read 0 = enabled), 0x60 marker (0x5A), defined with flags 3 in cfw_init. Marker
  missing (first boot / older settings record) -> defaults are stored. Ids 0x20..0x3F avoided: they alias the
  LSBs of 14-bit params 0x00..0x1F.

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
