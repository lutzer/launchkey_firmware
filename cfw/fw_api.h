/* Stock firmware (fw 1563) API used by cfw code. Addresses: cfw/fw_api.ld. */
#ifndef FW_API_H
#define FW_API_H

#include <stdint.h>

/* Feature parameter value as passed to confsys_{get,set}_param. flags on set:
 * bit0 = definition (first one wins, no listeners), bit1 = persistent. */
typedef struct {
    uint32_t value;
    uint32_t flags;
} fw_param_t;

#define FW_GROUP_FEATURE 0xD0

/* group 0xD0 = feature params; ids: 0x4A scale on/off, 0x3E scale type, 0x3D root, 0x3C scale mode */
void confsys_get_param(uint32_t group, uint32_t id, fw_param_t *out);
void confsys_set_param(uint32_t group, uint32_t id, const fw_param_t *in, uint32_t r3);

/* filter(id, old, new) runs before a (non-definition) feature param change is stored and
 * returns the value to store; returning old vetoes the change. 5 of 8 slots used. */
void feat_add_filter(uint32_t (*filter)(uint32_t id, uint32_t old, uint32_t value));

/* listener(id, value) called after a feature param changed */
void feat_add_listener(void (*listener)(uint32_t id, uint32_t value));

void feat_scale_init(void);

/* Appends "a.b.c.d" from ver[4] to buf at pos (bounded by size); returns the new pos. */
uint32_t fmt_version(char *buf, uint32_t pos, uint32_t size, const uint32_t *ver);

/* Appends str to the NUL-terminated buf (bounded by size, always terminated); returns the new length. */
uint32_t str_append(char *buf, uint32_t pos, uint32_t size, const char *str);

/* 30 x 12 bytes, index = scale type (1..30); non-zero = semitone in scale */
extern const uint8_t g_scale_intervals[][12];

/* Main loop: called several times per pass; cfw_poll hooks one of these calls. */
void confstore_poll(void);

/* Display: screen 1 + template 0 = two-line popup (field 0 title, field 1 value), as used by
 * the encoder-mode switch. disp_text sets a field and redraws. ui_popup_begin notifies the UI
 * that a popup starts (called by the stock popups before drawing). */
void disp_set_screen(uint32_t screen, uint32_t template_id);
void disp_text(uint32_t screen, uint32_t field, const char *str);
/* Write a layout's name into a display field; the layout handle {index, type} comes in r2/r3
 * (type 1 = pads). _a is used by the mode popups, _b by the home screen. */
void layout_name_text_a(uint32_t screen, uint32_t field, uint32_t index, uint32_t type);
void layout_name_text_b(uint32_t screen, uint32_t field, uint32_t index, uint32_t type);
/* Select a layout like the Shift overlay's mode pads do (layout_select + mode popup); the handle
 * {index, type} is passed by value in r0/r1. Pad mode index = feature param 0x1D value. */
void ui_select_layout(uint32_t index, uint32_t type);
void ui_popup_begin(void);

/* Input listeners (all called from the main loop). io lists: control id < 0x78. */
typedef void (*io_listener_fn)(uint32_t id, uint32_t value);
void io_add_listener_a(io_listener_fn fn);   /* 7 of 8 slots used by stock fw */
void io_add_listener_b(io_listener_fn fn);   /* 1 of 8 */
void io_add_listener_c(void (*fn)(uint32_t id));  /* 5 of 8 */
void io_add_listener_d(io_listener_fn fn);   /* 7 of 8 */

/* The io lists themselves (count + listeners); cfw replaces the stock dispatchers
 * (0x0801B530 A, 0x0801B54C B, 0x0801B574 C, 0x0801B592 D) to filter events. */
typedef struct {
    uint8_t count;
    void *fn[8];
} io_list_t;
extern io_list_t g_io_list_a, g_io_list_b, g_io_list_c, g_io_list_d;

/* The encoder module (inputrelative.cpp) derives relin events from raw encoder io events
 * through these io listeners (registered on lists A, C, D). */
void relin_io_listener_a(uint32_t id, uint32_t value);
void relin_io_listener_c(uint32_t id);
void relin_io_listener_d(uint32_t id, uint32_t value);

/* Encoders: (encoder id, accumulated value, delta). Lists use io_list_t (max 4 entries);
 * cfw replaces the dispatchers (0x08020E48 pos, 0x08020E56 hires, 0x08020E8A step). */
typedef void (*relin_listener_fn)(uint32_t id, uint32_t value, int32_t delta);
extern io_list_t g_relin_list_pos, g_relin_list_hires, g_relin_list_step;
void relin_add_pos_listener(relin_listener_fn fn);   /* 2 of 4 */
void relin_add_step_listener(relin_listener_fn fn);  /* 2 of 4 */

/* MIDI out. A port is a USB-MIDI packetiser object living on main()'s stack; the two
 * app ports are stored at g_app_ports[0..1]. midi_put streams bytes (SysEx F0..F7 of
 * any length, packed into USB-MIDI packets); midi_send3 sends one 3-byte message. */
typedef struct midi_port midi_port_t;
extern midi_port_t *g_app_ports[2];   /* written by main() at init */
void midi_put(midi_port_t *port, uint32_t byte);
void midi_send3(midi_port_t *port, uint32_t status, uint32_t d1, uint32_t d2);

/* Controls: ids 0..15 = pads (0..7 top row), 56..63 = encoders 1..8, 25 = Shift.
 * ctl_block makes the stock app ignore a control (no notes / CCs); the input listeners
 * still see it. */
void ctl_block(uint32_t id);
void ctl_unblock(uint32_t id);
/* Also set while a menu (Settings, Shift overlay, ...) has captured the control
 * (ui_menumgr capture @0x08025712). */
uint32_t ctl_is_blocked(uint32_t id);

/* LED override layer (drawn instead of the pad mode's colours while active):
 * led_override(id, 0 static / 1 flash / 2 pulse) turns it on, led_override(id, 3) off.
 * Colours are 0xRRGGBB. */
#define LED_STATIC  0
#define LED_FLASH   1
#define LED_PULSE   2
#define LED_RELEASE 3
void led_override_color(uint32_t id, uint32_t rgb);
void led_override(uint32_t id, uint32_t mode);

/* Clock: 24 PPQN tick counter, advanced by clock_poll (0x08013C60) in the main loop from the
 * internal tempo (period in us at 0x20000114) or counted external MIDI clocks. Poll it from
 * the main loop and step on the difference, like the MIDI Clock Out sender (0x08014492). */
extern volatile uint32_t g_clock_ticks;
/* Model: number of keys (25 / 37; >= 49 selects the big-model settings table). */
uint32_t device_key_count(void);
/* String table behind str_get: per-language base pointers, current language, u16 offsets. */
extern const char *const g_str_lang_base[];
extern volatile uint8_t g_str_lang;
extern const uint16_t g_str_offsets[];

/* Received MIDI transport (realtime handler 0x08030958 calls these for FA / FB / FC). */
void clock_midi_start(void);
void clock_midi_continue(void);
void clock_midi_stop(void);

/* Feature param value get / set (group 0xD0; the volatile setter does not mark the value persistent). */
uint32_t feat_get_value(uint32_t id);
void feat_set_value_volatile(uint32_t id, uint32_t value);

/* UI views ("menus", ui_menumgr): a view owns the controls it captured. ui_close_views runs every
 * view's close callback, ui_view_changed tells the display a view opened (stock open path:
 * close_views, set shown, view_changed, ui_popup_begin). Popup: arp_popup_title(key, string id)
 * opens the popup with a title, popup_field_text(key, field, str) sets its value line. */
#define UI_MENU_ARP    1
#define FEAT_SHIFT     0x3F  /* feature param mirroring Shift (127 held / 0) */
#define UI_MENU_SHIFT  0xE   /* Shift overlay: mode-select pads (bottom row = pad modes) */
#define UI_MENU_SCALE  0xF
uint32_t ui_menu_owns(uint32_t id, uint32_t menu);
void ui_close_views(void);
void ui_view_changed(void);
void popup_field_text(uint32_t key, uint32_t field, const char *str);

/* Scale view (menu 0xF, buttons Scale 0x12 / Shift+Scale 0x42): press handler, close, state. */
typedef struct {
    uint8_t  item;          /* highlighted encoder item */
    uint8_t  shown;
    uint8_t  pending_off;   /* release turns the scale off */
    uint8_t  pad;
    uint32_t hold_timer;    /* ms until a held press opens the view */
} scale_view_t;
extern volatile scale_view_t g_scale_view;
void scale_press(uint32_t id);
void scale_view_close(void);
void scale_view_open(void);     /* scale on + view shown (stock open path, incl. ui_popup_begin) */

/* Arp view (menu 1, buttons Arp 0x1E / Shift+Arp 0x60 = latch toggle). */
typedef struct {
    uint32_t prev_layout;   /* pad layout before the hold switched to the arp pattern layout */
    uint8_t  prev_type, pad[3];
    uint8_t  shown;
    uint8_t  pending_off;   /* release turns the arp off */
    uint8_t  pad2[2];
    uint32_t hold_timer;    /* ms until a held press opens the view */
    uint32_t layout_timer;  /* ms until a held press switches the pads to the arp pattern layout */
} arp_view_t;
extern volatile arp_view_t g_arp_view;
void arp_press(uint32_t id);
void arp_view_close(void);
void arp_view_open(void);       /* arp on + view shown (stock open path, incl. ui_popup_begin) */
void arp_led_set(uint32_t id, uint32_t rgb);     /* only if the arp view owns the control */
void arp_popup_title(uint32_t key, uint32_t str_id);

/* Arp engine (main loop, once per pass): updates its elapsed-ms / elapsed-tick references, then returns
 * early while the arp is off (enabled == 0); otherwise steps the pattern and ages the note queue. */
typedef struct {
    uint8_t enabled;        /* arp on */
} arp_state_t;
extern volatile arp_state_t g_arp_state;
void arp_poll(void);
/* Note queue of the arp (g_arp + 0x90): flush removes every queued note with a note-off. */
typedef struct arp_notes arp_notes_t;
extern arp_notes_t g_arp_notes;
void arp_notes_flush(arp_notes_t *notes);

/* Play button (control 0x48): in standalone mode (no DAW connection) the stock press handler sends
 * MIDI Start (running 0) or Stop (running 1) on its port and toggles running; it ignores other ids. */
typedef struct {
    uint8_t  standalone;    /* no DAW connection: Play (and 0x49) captured */
    uint8_t  running;       /* last sent: 1 = Start, 0 = Stop */
    uint8_t  pad[2];
    midi_port_t *port;
} transport_t;
extern volatile transport_t g_transport;
void play_press(uint32_t id);

/* Shift overlay (menu 0xE) LED refresh 0x08022A88: bottom-row pads = pad modes (current 0x0086FF,
 * others 0x001642); its last call is shift_leds_tail (no args). */
void shift_leds_tail(void);

#endif
