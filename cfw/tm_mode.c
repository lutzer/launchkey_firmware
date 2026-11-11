/* Turing Machine mode (replaces pad mode Custom 4).
 *
 * Active while feature param 0x1D (pad mode) = 8 and Shift is up:
 *   top pads 0..7      register bits (newest at pad 1), green; dark beyond the loop length;
 *                      press flips the bit
 *   bottom pads 8..15  scale notes (pad 9 = root, ...), white; press toggles (Scale on only)
 *   encoders 1..7      probability, length, rate, octave, range, gate length, trigger heads
 * A step plays a note when any trigger head (register positions 0, s, 2s, ... within the loop;
 * Full = every position, 1/16 = first only) reads a 1; every note lasts the gate length and
 * is released before the next step (no ties).
 * Steps on the 24 PPQN clock counter (g_clock_ticks), notes go out on the USB MIDI port.
 * The input dispatchers in cfw_init.c ask tm_io_event / tm_enc_event whether an event
 * belongs to this mode.
 */
#include <stdint.h>
#include "fw_api.h"
#include "tm_core.h"
#include "tm_mode.h"

#define FEAT_SCALE_MODE   0x3C
#define FEAT_SCALE_ROOT   0x3D
#define FEAT_SCALE_TYPE   0x3E
#define FEAT_SCALE_ON     0x4A
#define FEAT_PAD_MODE     0x1D
#define FEAT_SHIFT        0x3F
#define FEAT_KEYS_CHANNEL 0x64        /* Settings > Keys Channel, 0-based */
#define PAD_CUSTOM4       8

#define PAD_COUNT         16
#define PAD_ROW           8
#define ENC_FIRST         56          /* encoder 1 */
#define ENC_COUNT         7

#define MIDI_PORT         1           /* g_app_ports[1] = USB MIDI port */
#define VELOCITY          100

#define GREEN_ON          0x00FF00
#define GREEN_DIM         0x001800
#define WHITE_ON          0xFFFFFF
#define WHITE_DIM         0x181818
#define LED_OFF           0x000000

enum { ENC_PROB, ENC_LEN, ENC_RATE, ENC_OCT, ENC_RANGE, ENC_GATE, ENC_HEADS };

static tm_t tm;
static struct {
    uint8_t active, shift;
    uint16_t ticks;          /* clock ticks since the current step started */
    int8_t  note;            /* sounding note or -1 */
    uint8_t channel;         /* channel of the sounding note */
    uint16_t menu_pads;      /* pads currently captured by a menu (not drawn) */
    uint32_t last_clock;
    uint32_t owned;          /* controls whose current press belongs to this mode (slot bits) */
} st;

static const char *const rate_names[TM_NUM_DIVS] = {
    "4/1", "2/1", "1/1", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32",
};

/* ---- helpers ---- */

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static char *put_num(char *p, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    return p;
}

static uint32_t feat_get(uint32_t id)
{
    fw_param_t p;
    confsys_get_param(FW_GROUP_FEATURE, id, &p);
    return p.value;
}

static const uint8_t *scale_semis(void)
{
    uint32_t type = feat_get(FEAT_SCALE_TYPE);
    if (type < 1 || type > 30)
        type = 1;
    return g_scale_intervals[type];
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---- output ---- */

static void note_off(void)
{
    if (st.note >= 0 && g_app_ports[MIDI_PORT])
        midi_send3(g_app_ports[MIDI_PORT], 0x80 | st.channel, (uint32_t)st.note, 0);
    st.note = -1;
}

static void note_on(int note)
{
    if (note < 0 || !g_app_ports[MIDI_PORT])
        return;
    st.channel = (uint8_t)(feat_get(FEAT_KEYS_CHANNEL) & 0x0F);
    midi_send3(g_app_ports[MIDI_PORT], 0x90 | st.channel, (uint32_t)note, VELOCITY);
    st.note = (int8_t)note;
}

/* Pads captured by a menu (e.g. Settings) are left to the menu; tm_mode_poll redraws
 * them once the menu lets go. */
static void led(uint32_t id, uint32_t rgb)
{
    if (st.menu_pads >> id & 1)
        return;
    led_override_color(id, rgb);
    led_override(id, LED_STATIC);
}

/* Pad drawing pauses while Shift is held (the Shift overlay owns the LEDs then);
 * releasing Shift redraws both rows. */
static void draw_register(void)
{
    if (st.shift)
        return;
    for (uint32_t i = 0; i < PAD_ROW; i++)
        led(i, i >= tm.len ? LED_OFF : (tm.reg >> i & 1) ? GREEN_ON : GREEN_DIM);
}

static void draw_notes(void)
{
    if (st.shift)
        return;
    int scale_on = feat_get(FEAT_SCALE_ON) != 0;
    int size = tm_scale_size(scale_semis());
    for (uint32_t i = 0; i < PAD_ROW; i++) {
        uint32_t rgb;
        if (!scale_on)
            rgb = WHITE_ON;
        else if ((int)i >= size)
            rgb = LED_OFF;
        else
            rgb = (tm.mask >> i & 1) ? WHITE_ON : WHITE_DIM;
        led(PAD_ROW + i, rgb);
    }
}

static void popup(const char *title, const char *value)
{
    ui_popup_begin();
    disp_set_screen(1, 0);
    disp_text(1, 0, title);
    disp_text(1, 1, value);
}

static void show_param(int enc)
{
    char v[20], *p = v;
    const char *title = "";
    switch (enc) {
    case ENC_PROB: {
        title = "Probability";
        uint32_t flip = (uint32_t)(TM_PROB_MAX - tm.prob) * 100 / TM_PROB_MAX;
        if (tm.prob == TM_PROB_MAX)
            p = put_str(p, "Locked");
        else if (tm.prob == 0)
            p = put_str(p, "Locked inv 2x");
        else
            p = put_str(put_num(put_str(p, "Flip "), flip), "%");
        break;
    }
    case ENC_LEN:
        title = "Length";
        p = put_str(put_num(p, tm.len), " steps");
        break;
    case ENC_RATE:
        title = "Rate";
        p = put_str(p, rate_names[tm.div]);
        break;
    case ENC_OCT:
        title = "Octave";
        p = put_num(p, tm.octave);
        break;
    case ENC_RANGE:
        title = "Range";
        p = put_str(put_num(p, tm.range), " notes");
        break;
    case ENC_GATE:
        title = "Gate";
        p = put_str(put_num(p, tm.gate), "%");
        break;
    case ENC_HEADS:
        title = "Trig Heads";
        if (tm.heads == 0)
            p = put_str(p, "Full");
        else
            p = put_num(put_str(p, "1/"), tm_head_spacing[tm.heads]);
        break;
    }
    *p = 0;
    popup(title, v);
}

/* ---- sequencing ---- */

static void step(void)
{
    note_off();
    tm_step(&tm);
    if (tm_gate(&tm))
        note_on(tm_note(&tm, (int)feat_get(FEAT_SCALE_ROOT) % 12, scale_semis(), feat_get(FEAT_SCALE_ON) != 0));
    draw_register();
}

static void advance_clock(void)
{
    uint32_t now = g_clock_ticks;
    uint32_t elapsed = now - st.last_clock;
    st.last_clock = now;
    if (elapsed > 384)          /* clock jumped (restart / long stall): resync */
        elapsed = 1;
    while (elapsed--) {
        uint16_t len = tm_div_ticks[tm.div];
        if (++st.ticks >= len) {
            st.ticks = 0;
            step();
        } else if (st.ticks == tm_gate_ticks(&tm)) {
            note_off();         /* always before the next step: no ties */
        }
    }
}

/* ---- mode switching ---- */

static void set_active(int on)
{
    if (on == st.active)
        return;
    st.active = (uint8_t)on;
    if (on) {
        st.ticks = 0;
        st.last_clock = g_clock_ticks;
        draw_register();
        draw_notes();
        popup("Turing Machine", "");
    } else {
        note_off();
        for (uint32_t id = 0; id < PAD_COUNT; id++)
            led_override(id, LED_RELEASE);
    }
}

/* ---- input (called from the dispatchers in cfw_init.c) ---- */

static int ctl_slot(uint32_t id)
{
    if (id < PAD_COUNT)
        return (int)id;
    if (id >= ENC_FIRST && id < ENC_FIRST + ENC_COUNT)
        return (int)(PAD_COUNT + id - ENC_FIRST);
    return -1;
}

static void pad_pressed(uint32_t id)
{
    if (id < PAD_ROW) {                         /* register pad: flip that bit */
        if (tm_flip(&tm, (int)id))
            draw_register();
        return;
    }
    if (!feat_get(FEAT_SCALE_ON))
        return;
    uint32_t i = id - PAD_ROW;
    if ((int)i >= tm_scale_size(scale_semis()))
        return;
    tm.mask ^= (uint8_t)(1u << i);
    draw_notes();
}

/* list: 0 = press (A), 1 = B, 2 = release (C), 3 = D. Returns TM_PASS / TM_CONSUME / TM_ENC_ONLY. */
int tm_io_event(int list, uint32_t id, uint32_t value)
{
    (void)value;
    int slot = ctl_slot(id);
    if (slot < 0)
        return TM_PASS;
    uint32_t bit = 1u << slot;
    if (list == 0) {            /* the press decides who owns the control until its release */
        int take = st.active && !st.shift && !ctl_is_blocked(id);   /* blocked = a menu has it */
        st.owned = take ? st.owned | bit : st.owned & ~bit;
        if (take && id < PAD_COUNT)
            pad_pressed(id);
    }
    if (!(st.owned & bit))
        return TM_PASS;
    if (list == 2)
        st.owned &= ~bit;
    return id < PAD_COUNT ? TM_CONSUME : TM_ENC_ONLY;
}

/* list: 0 = pos (every tick), 1 = hires, 2 = step (per detent). Returns 1 if consumed. */
int tm_enc_event(int list, uint32_t id, int32_t delta)
{
    delta = (int16_t)delta;     /* the encoder module passes deltas as u16 (-1 = 0xFFFF) */
    if (!st.active || st.shift || id < ENC_FIRST || id >= ENC_FIRST + ENC_COUNT || ctl_is_blocked(id))
        return 0;
    int enc = (int)(id - ENC_FIRST);
    if (enc == ENC_PROB) {
        if (list != 0)
            return 1;
        tm.prob = (uint8_t)clamp(tm.prob + delta, 0, TM_PROB_MAX);
    } else {
        if (list != 2)
            return 1;
        switch (enc) {
        case ENC_LEN:   tm.len = (uint8_t)clamp(tm.len + delta, TM_LEN_MIN, TM_LEN_MAX); draw_register(); break;
        case ENC_RATE:  tm.div = (uint8_t)clamp(tm.div + delta, 0, TM_NUM_DIVS - 1); break;
        case ENC_OCT:   tm.octave = (uint8_t)clamp(tm.octave + delta, 0, TM_OCT_MAX); break;
        case ENC_RANGE: tm.range = (uint8_t)clamp(tm.range + delta, TM_RANGE_MIN, TM_RANGE_MAX); break;
        case ENC_GATE:  tm.gate = (uint8_t)clamp(tm.gate + delta * TM_GATE_STEP, TM_GATE_MIN, TM_GATE_MAX); break;
        case ENC_HEADS: tm.heads = (uint8_t)clamp(tm.heads + delta, 0, TM_NUM_HEADS - 1); break;
        }
    }
    show_param(enc);
    return 1;
}

void tm_feat_changed(uint32_t id, uint32_t value)
{
    switch (id) {
    case FEAT_PAD_MODE:
        set_active(value == PAD_CUSTOM4);
        break;
    case FEAT_SHIFT:
        st.shift = value != 0;
        if (!st.shift && st.active) {   /* the Shift overlay uses the LED override layer too */
            draw_register();
            draw_notes();
        }
        break;
    case FEAT_SCALE_ON:
    case FEAT_SCALE_TYPE:
    case FEAT_SCALE_ROOT:
        if (st.active)
            draw_notes();
        break;
    }
}

void tm_mode_init(void)
{
    volatile uint32_t *systick_val = (volatile uint32_t *)0xE000E018;
    tm_init(&tm, *systick_val ^ g_clock_ticks);
    st.note = -1;
}

/* Main loop (after input handling). */
void tm_mode_poll(void)
{
    static uint8_t started;
    if (!started) {
        started = 1;
        set_active(feat_get(FEAT_PAD_MODE) == PAD_CUSTOM4);
    }
    if (!st.active)
        return;
    uint16_t menu = 0;
    for (uint32_t id = 0; id < PAD_COUNT; id++)
        if (ctl_is_blocked(id))
            menu |= (uint16_t)(1u << id);
    if (menu != st.menu_pads) {
        uint16_t released = st.menu_pads & (uint16_t)~menu;
        st.menu_pads = menu;
        if (released) {
            draw_register();
            draw_notes();
        }
    }
    advance_clock();
}
