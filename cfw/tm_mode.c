/* Turing Machine mode (replaces pad mode Custom 4).
 *
 * Active while feature param 0x1D (pad mode) = 8 and Shift is up:
 *   top pads 0..7      register bits (newest at pad 1), green; dark beyond the loop length.
 *                      While Function is held both rows show bits 1..16 and a press flips the bit.
 *   bottom pads 8..15  scale notes (pad 9 = root, ...), white; press toggles (Scale on only);
 *                      pads beyond the scale size are dark, all eight lit with Scale off
 *   Function (17)      hold: while held the top row is frozen on screen (the register keeps
 *                      running) and top-pad presses flip the bit they showed
 *   > (16)             play/pause (above Function); the mode starts paused
 *   up/down (34/35)    octave
 *   encoders 1..8      probability, length, rate, range, gate length, trigger heads, rotate,
 *                      velocity depth (velocity from register bits 4..10, see tm_velocity)
 *                      (rotate shifts the loop without flipping bits, i.e. moves the step that
 *                      MIDI Start returns to)
 * MIDI transport: Start = back to the first step, which plays on the next clock tick (the
 * downbeat), Continue = resume, Stop = pause.
 * Parameters and the note mask are saved as persistent feature params (see PARAM_*).
 * A step plays a note depending on the trigger mode (tm_heads: every step, OR / AND / XOR of
 * delayed register bits, ~88 % .. ~12 % dense); every note lasts the gate length and
 * is released before the next step (no ties).
 * Steps on the 24 PPQN clock counter (g_clock_ticks), notes go out on the MIDI port, which the
 * stock firmware chains to the DIN/TRS output (port link +0x18, set in main).
 * The input dispatchers in cfw_init.c ask tm_io_event / tm_enc_event whether an event
 * belongs to this mode. Controls captured by a menu (ctl_is_blocked) go to the menu, unless
 * pad Custom 4 was (re)selected since the last other button press (st.priority).
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
#define CTL_SHIFT         25
#define PAD_CUSTOM4       8

#define PAD_COUNT         16
#define PAD_ROW           8
#define NOTE8_BIT         0x80        /* 8th scale note (bottom pad 8, 8-note scales only) */
#define ENC_FIRST         56          /* encoder 1 */
#define ENC_COUNT         8
#define ENC_ROTATE        6           /* encoder 7; encoders 1..6 = params P_PROB..P_HEADS */
#define ENC_VEL           7           /* encoder 8 */
/* Buttons next to the pads (ids from the DAW-mode CC map at 0x0801C180: CC 104..107). */
#define BTN_PLAY          16          /* ">" (scene launch), above Function */
#define BTN_HOLD          17          /* Function */
#define BTN_OCT_UP        34
#define BTN_OCT_DOWN      35

#define MIDI_PORT         1           /* g_app_ports[1] = USB MIDI port (+ DIN via link) */

#define GREEN_ON          0x00FF00
#define GREEN_DIM         0x001800
#define WHITE_ON          0xFFFFFF
#define WHITE_DIM         0x181818
#define LED_OFF           0x000000

/* Persistent feature params (free ids >= 0x40: ids 0x20..0x3F double as 14-bit LSBs). */
/* Feature values are 7 bits: P_MASK holds notes 1..7, P_NOTE8_OFF the 8th note inverted (records
 * from before it existed read 0 = on, as the 8th note used to be always on). */
enum { P_PROB, P_LEN, P_RATE, P_RANGE, P_GATE, P_HEADS, P_OCT, P_MASK, P_CHAN, P_NOTE8_OFF, P_VEL, P_COUNT };
static const uint8_t param_id[P_COUNT] = {0x50, 0x51, 0x52, 0x5B, 0x5D, 0x5E, 0x5A, 0x5F, TM_CHANNEL_PARAM, 0x62, 0x63};
#define PARAM_MAGIC_ID    0x60        /* != PARAM_MAGIC: stored values are not ours -> defaults */
#define PARAM_MAGIC       0x5A
#define PARAM_PERSISTENT  3           /* definition flags: defined | persistent */


static tm_t tm;
static uint8_t midi_channel;   /* Settings > Turing Channel, 0-based */
static struct {
    uint8_t active, shift;
    uint16_t ticks;          /* clock ticks since the current step started */
    int8_t  note;            /* sounding note or -1 */
    uint8_t channel;         /* channel of the sounding note */
    uint16_t menu_pads;      /* pads currently captured by a menu (not drawn) */
    uint32_t last_clock;
    uint32_t owned;          /* controls whose current press belongs to this mode (slot bits) */
    uint8_t hold;            /* Function down: top row frozen, bits can be flipped */
    uint8_t playing;         /* play/pause (">"); off when the mode is entered */
    uint8_t priority;        /* set by (re)selecting pad Custom 4: our controls win over a menu
                              * (Scale/Arp screens keep them captured); cleared by any other
                              * button press, i.e. whenever a menu may be opened */
    uint16_t frozen_reg;     /* register as shown while frozen */
    uint16_t frozen_steps;   /* steps since freezing */
    uint8_t pos;             /* steps since the loop start, mod length (for rewinding) */
    uint8_t rot;             /* rotation since the loop start, mod length (display only) */
    uint8_t popup_pending;   /* show the mode popup from the main loop, after the stock one */
} st;

/* MIDI transport recorded by tm_transport, applied in tm_mode_poll */
static volatile uint8_t transport_event;
static volatile uint32_t transport_clock;

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

static void feat_set(uint32_t id, uint32_t value, uint32_t flags)
{
    fw_param_t p = {value, flags};
    confsys_set_param(FW_GROUP_FEATURE, id, &p, 0);
}

static uint32_t param_value(int p)
{
    switch (p) {
    case P_PROB:  return tm.prob;
    case P_LEN:   return tm.len;
    case P_RATE:  return tm.div;
    case P_OCT:   return tm.octave;
    case P_RANGE: return tm.range;
    case P_GATE:  return tm.gate;
    case P_HEADS: return tm.heads;
    case P_CHAN:  return midi_channel;
    case P_VEL:   return tm.vel;
    case P_MASK:  return tm.mask & 0x7F;
    default:      return !(tm.mask & NOTE8_BIT);
    }
}

static int clamp(int v, int lo, int hi);

static void param_apply(int p, uint32_t v)
{
    switch (p) {
    case P_PROB:  tm.prob = (uint8_t)clamp((int)v, 0, TM_PROB_MAX); break;
    case P_LEN:   tm.len = (uint8_t)clamp((int)v, TM_LEN_MIN, TM_LEN_MAX); break;
    case P_RATE:  tm.div = (uint8_t)clamp((int)v, 0, TM_NUM_DIVS - 1); break;
    case P_OCT:   tm.octave = (uint8_t)clamp((int)v, 0, TM_OCT_MAX); break;
    case P_RANGE: tm.range = (uint8_t)clamp((int)v, TM_RANGE_MIN, TM_RANGE_MAX); break;
    case P_GATE:  tm.gate = (uint8_t)clamp((int)v, TM_GATE_MIN, TM_GATE_MAX); break;
    case P_HEADS: tm.heads = (uint8_t)clamp((int)v, 0, TM_NUM_HEADS - 1); break;
    case P_MASK:  tm.mask = (uint8_t)((v & 0x7F) | (tm.mask & NOTE8_BIT)); break;
    case P_NOTE8_OFF: tm.mask = (uint8_t)((tm.mask & 0x7F) | (v ? 0 : NOTE8_BIT)); break;
    case P_CHAN:  midi_channel = (uint8_t)(v & 0x0F); break;
    case P_VEL:   tm.vel = (uint8_t)clamp((int)v, 0, TM_VEL_MAX); break;
    }
}

static void param_save(int p)
{
    feat_set(param_id[p], param_value(p), 0);
}

/* Stored values valid -> load them; otherwise (first boot, old settings record) store ours. */
static void param_sync(void)
{
    if (feat_get(PARAM_MAGIC_ID) == PARAM_MAGIC) {
        for (int p = 0; p < P_COUNT; p++)
            param_apply(p, feat_get(param_id[p]));
    } else {
        for (int p = 0; p < P_COUNT; p++)
            param_save(p);
        feat_set(PARAM_MAGIC_ID, PARAM_MAGIC, 0);    /* last: its listener call re-syncs */
    }
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
    st.channel = midi_channel;
    midi_send3(g_app_ports[MIDI_PORT], 0x90 | st.channel, (uint32_t)note, (uint32_t)tm_velocity(&tm));
    st.note = (int8_t)note;
}

/* Pads captured by a menu (e.g. Settings) are left to the menu; tm_mode_poll redraws
 * them once the menu lets go. */
static void led(uint32_t id, uint32_t rgb)
{
    if (id < PAD_COUNT && (st.menu_pads >> id & 1))
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
    uint16_t reg = st.hold ? st.frozen_reg : tm.reg;
    uint32_t pads = st.hold ? PAD_COUNT : PAD_ROW;  /* held: bits 9..16 on the bottom row */
    for (uint32_t i = 0; i < pads; i++) {
        int bit = reg >> i & 1;
        uint32_t rgb;
        if (i >= tm.len)
            rgb = LED_OFF;
        else
            rgb = bit ? GREEN_ON : GREEN_DIM;
        led(i, rgb);
    }
}

static void draw_notes(void)
{
    if (st.shift || st.hold)    /* held: the bottom row shows register bits */
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

static void draw_buttons(void)
{
    if (st.shift)
        return;
    led(BTN_PLAY, st.playing ? WHITE_ON : WHITE_DIM);
    led(BTN_HOLD, st.hold ? WHITE_ON : WHITE_DIM);
    led(BTN_OCT_UP, tm.octave < TM_OCT_MAX ? WHITE_ON : WHITE_DIM);
    led(BTN_OCT_DOWN, tm.octave > 0 ? WHITE_ON : WHITE_DIM);
}

static void draw_all(void)
{
    draw_register();
    draw_notes();
    draw_buttons();
}

static const uint8_t buttons[] = {BTN_PLAY, BTN_HOLD, BTN_OCT_UP, BTN_OCT_DOWN};
#define BTN_COUNT (sizeof buttons)

static void popup(const char *title, const char *value)
{
    ui_popup_begin();
    disp_set_screen(1, 0);
    disp_text(1, 0, title);
    disp_text(1, 1, value);
}

/* p = P_PROB..P_OCT (encoders 1..6 = P_PROB..P_HEADS) */
/* Mode popup: title + short encoder labels in the stock 8-field overview layout (template 4:
 * field 0 = title, fields 1..4 / 5..8 = encoders 1..4 / 5..8, 32 px each). */
#define TEMPLATE_OVERVIEW 4
static const char *const enc_labels[8] = {"Prob", "Len", "Rate", "Rng", "Gte", "Hds", "Rot", "Vel"};

static void popup_overview(void)
{
    ui_popup_begin();
    disp_set_screen(1, TEMPLATE_OVERVIEW);
    disp_text(1, 0, "Turing Machine");
    for (uint32_t i = 0; i < 8; i++)
        disp_text(1, 1 + i, enc_labels[i]);
}

static void show_param(int param)
{
    char v[20], *p = v;
    const char *title = "";
    switch (param) {
    case P_PROB: {
        title = "Probability";
        uint32_t flip = (uint32_t)(TM_PROB_MAX - tm.prob) * 100 / TM_PROB_MAX;
        if (tm.prob == TM_PROB_MAX)
            p = put_str(p, "Locked");
        else if (tm.prob == 0)
            p = put_str(p, "Invert");
        else
            p = put_str(put_num(put_str(p, "Flip "), flip), "%");
        break;
    }
    case P_LEN:
        title = "Length";
        p = put_str(put_num(p, tm.len), " steps");
        break;
    case P_RATE:
        title = "Rate";
        p = put_str(p, rate_names[tm.div]);
        break;
    case P_OCT:
        title = "Turing Octave";
        if (tm.octave < TM_OCT_ZERO)
            p = put_num(put_str(p, "-"), TM_OCT_ZERO - tm.octave);
        else
            p = put_num(put_str(p, tm.octave > TM_OCT_ZERO ? "+" : ""), tm.octave - TM_OCT_ZERO);
        break;
    case P_RANGE:
        title = "Range";
        p = put_str(put_num(p, tm.range), " notes");
        break;
    case P_GATE:
        title = "Gate";
        p = put_str(put_num(p, tm.gate), "%");
        break;
    case P_VEL:
        title = "Velocity";
        if (tm.vel == 0)
            p = put_str(p, "Fixed");
        else
            p = put_str(put_num(put_str(p, "Depth "), tm.vel), "%");
        break;
    case P_HEADS:
        title = "Trig Heads";
        p = put_str(p, tm_heads[tm.heads].name);
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
    st.frozen_steps++;
    st.pos = (uint8_t)((st.pos + 1) % tm.len);
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

static void set_playing(int on)
{
    st.playing = (uint8_t)on;
    if (on) {
        st.last_clock = g_clock_ticks;
        st.ticks = 0;
        step();                 /* start right away */
    } else {
        note_off();
    }
    draw_buttons();
    popup("Turing Machine", on ? "Playing" : "Paused");
}

/* Back to the first step of the loop; it plays on the next clock tick once playing. */
static void rewind(void)
{
    tm_rewind(&tm, st.pos);
    st.pos = 0;
    note_off();
    st.ticks = (uint16_t)(tm_div_ticks[tm.div] - 1);
    draw_register();
}

void tm_transport(int event)
{
    transport_clock = g_clock_ticks;
    transport_event = (uint8_t)event;
}

/* Start: the first step falls on the first clock tick after the message, like a sequencer
 * restarting from the top. Continue keeps the step phase. */
static void apply_transport(int event, uint32_t clock)
{
    if (event == TM_STOP) {
        if (st.playing) {
            st.playing = 0;
            note_off();
            draw_buttons();
        }
        return;
    }
    if (event == TM_START) {
        st.playing = 0;
        rewind();
    } else if (st.playing) {
        return;                 /* Continue while running: nothing to do */
    }
    st.playing = 1;
    st.last_clock = clock;
    draw_buttons();
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
        st.playing = 0;         /* entering the mode never starts playback */
        st.pos = 0;             /* the loop starts here */
        st.rot = 0;
        draw_all();
        st.popup_pending = 1;   /* the stock "Custom 4" popup comes after this listener call */
    } else {
        note_off();
        st.hold = 0;
        st.playing = 0;
        for (uint32_t id = 0; id < PAD_COUNT; id++)
            led_override(id, LED_RELEASE);
        for (uint32_t i = 0; i < BTN_COUNT; i++)
            led_override(buttons[i], LED_RELEASE);
    }
}

/* ---- input (called from the dispatchers in cfw_init.c) ---- */

/* A menu (Settings, Scale, Arp, ...) owns this control right now. */
static int menu_owns(uint32_t id)
{
    return !st.priority && ctl_is_blocked(id);
}

/* The keyboard-octave popup captures the arrows too; the mode buttons only go to a menu
 * that has also captured pad 1 (a full menu such as Settings). */
static int menu_owns_button(uint32_t id)
{
    return menu_owns(id) && menu_owns(0);
}

static int ctl_slot(uint32_t id)
{
    if (id < PAD_COUNT)
        return (int)id;
    if (id >= ENC_FIRST && id < ENC_FIRST + ENC_COUNT)
        return (int)(PAD_COUNT + id - ENC_FIRST);
    for (uint32_t i = 0; i < BTN_COUNT; i++)
        if (id == buttons[i])
            return (int)(PAD_COUNT + ENC_COUNT + i);
    return -1;
}

static void pad_pressed(uint32_t id)
{
    if (st.hold) {                              /* flips need Function held */
        if ((int)id >= tm.len)
            return;
        /* the bit shown has moved on by the steps since freezing */
        int pos = ((int)id + st.frozen_steps) % tm.len;
        st.frozen_reg ^= (uint16_t)(1u << id);
        if (tm_flip(&tm, pos))
            draw_register();
        return;
    }
    if (id < PAD_ROW)
        return;
    if (!feat_get(FEAT_SCALE_ON))
        return;
    uint32_t i = id - PAD_ROW;
    if ((int)i >= tm_scale_size(scale_semis()))
        return;
    tm.mask ^= (uint8_t)(1u << i);
    param_save(i == 7 ? P_NOTE8_OFF : P_MASK);
    draw_notes();
}

static void button_pressed(uint32_t id)
{
    switch (id) {
    case BTN_PLAY:
        set_playing(!st.playing);
        break;
    case BTN_HOLD:
        st.hold = 1;
        st.frozen_reg = tm.reg;
        st.frozen_steps = 0;
        draw_register();
        draw_buttons();
        break;
    case BTN_OCT_UP:
    case BTN_OCT_DOWN:
        tm.octave = (uint8_t)clamp(tm.octave + (id == BTN_OCT_UP ? 1 : -1), 0, TM_OCT_MAX);
        param_save(P_OCT);
        draw_buttons();
        show_param(P_OCT);
        break;
    }
}

static void button_released(uint32_t id)
{
    if (id == BTN_HOLD && st.hold) {
        st.hold = 0;
        draw_register();
        draw_notes();
        draw_buttons();
    }
}

/* list: 0 = press (A), 1 = B, 2 = release (C), 3 = D. Returns TM_PASS / TM_CONSUME / TM_ENC_ONLY. */
int tm_io_event(int list, uint32_t id, uint32_t value)
{
    (void)value;
    int slot = ctl_slot(id);
    if (slot < 0) {
        if (list == 0 && id != CTL_SHIFT)
            st.priority = 0;    /* any other button may open a menu: menus win again */
        return TM_PASS;
    }
    uint32_t bit = 1u << slot;
    if (list == 0) {            /* the press decides who owns the control until its release */
        int is_enc = id >= ENC_FIRST && id < ENC_FIRST + ENC_COUNT;
        int take;
        if (st.shift)           /* Shift + encoder = show value; the Shift overlay's capture doesn't count */
            take = st.active && is_enc;
        else
            take = st.active &&
                   !(slot >= PAD_COUNT + ENC_COUNT ? menu_owns_button(id) : menu_owns(id));
        st.owned = take ? st.owned | bit : st.owned & ~bit;
        if (take && id < PAD_COUNT)
            pad_pressed(id);
        else if (take && slot >= PAD_COUNT + ENC_COUNT)
            button_pressed(id);
    }
    if (!(st.owned & bit))
        return TM_PASS;
    if (list == 2) {
        st.owned &= ~bit;
        if (slot >= PAD_COUNT + ENC_COUNT)
            button_released(id);
    }
    return id >= ENC_FIRST && id < ENC_FIRST + ENC_COUNT ? TM_ENC_ONLY : TM_CONSUME;
}

/* list: 0 = pos (every tick), 1 = hires, 2 = step (per detent). Returns 1 if consumed. */
/* Popup with the value of encoder enc (0..7). */
static void show_enc(int enc)
{
    if (enc == ENC_ROTATE) {
        char v[20], *p = put_num(put_str(v, "Step "), st.rot + 1u);
        *p = 0;
        popup("Rotate", v);
    } else {
        show_param(enc == ENC_VEL ? P_VEL : enc);
    }
}

int tm_enc_event(int list, uint32_t id, int32_t delta)
{
    delta = (int16_t)delta;     /* the encoder module passes deltas as u16 (-1 = 0xFFFF) */
    if (!st.active || id < ENC_FIRST || id >= ENC_FIRST + ENC_COUNT || (!st.shift && menu_owns(id)))
        return 0;
    int enc = (int)(id - ENC_FIRST);    /* == param index P_PROB..P_HEADS */
    if (st.shift) {             /* Shift + turn: show the value without changing it */
        if (list == 2)
            show_enc(enc);
        return 1;
    }
    if (enc == ENC_VEL) {
        if (list != 2)
            return 1;
        tm.vel = (uint8_t)clamp(tm.vel + delta * TM_VEL_STEP, 0, TM_VEL_MAX);
        param_save(P_VEL);
        show_param(P_VEL);
        return 1;
    }
    if (enc == ENC_ROTATE) {
        if (list != 2)
            return 1;
        /* rotating the register while keeping pos moves the step MIDI Start rewinds to */
        int n = ((-delta) % tm.len + tm.len) % tm.len;      /* CW = forward = rewind by len - 1 */
        tm_rewind(&tm, n);
        st.rot = (uint8_t)(((st.rot + delta) % tm.len + tm.len) % tm.len);
        draw_register();
        show_enc(ENC_ROTATE);
        return 1;
    }
    if (enc == P_PROB) {
        if (list != 0)
            return 1;
        tm.prob = (uint8_t)clamp(tm.prob + delta, 0, TM_PROB_MAX);
        param_save(P_PROB);
    } else {
        if (list != 2)
            return 1;
        switch (enc) {
        case P_LEN:
            tm.len = (uint8_t)clamp(tm.len + delta, TM_LEN_MIN, TM_LEN_MAX);
            st.pos = (uint8_t)(st.pos % tm.len);
            st.rot = (uint8_t)(st.rot % tm.len);
            draw_register();
            break;
        case P_RATE:  tm.div = (uint8_t)clamp(tm.div + delta, 0, TM_NUM_DIVS - 1); break;
        case P_RANGE: tm.range = (uint8_t)clamp(tm.range + delta, TM_RANGE_MIN, TM_RANGE_MAX); break;
        case P_GATE:  tm.gate = (uint8_t)clamp(tm.gate + delta * TM_GATE_STEP, TM_GATE_MIN, TM_GATE_MAX); break;
        case P_HEADS: tm.heads = (uint8_t)clamp(tm.heads + delta, 0, TM_NUM_HEADS - 1); break;
        }
        param_save(enc);
    }
    show_param(enc);
    return 1;
}

void tm_feat_changed(uint32_t id, uint32_t value)
{
    switch (id) {
    case FEAT_PAD_MODE:
        set_active(value == PAD_CUSTOM4);
        if (value == PAD_CUSTOM4) {     /* (re)selected: take the controls back from any menu */
            st.priority = 1;
            st.menu_pads = 0;
            st.popup_pending = 1;
            draw_all();
        }
        break;
    case FEAT_SHIFT:
        st.shift = value != 0;
        if (!st.shift && st.active)     /* the Shift overlay uses the LED override layer too */
            draw_all();
        break;
    case FEAT_SCALE_ON:
    case FEAT_SCALE_TYPE:
    case FEAT_SCALE_ROOT:
        if (st.active)
            draw_notes();
        break;
    case PARAM_MAGIC_ID:            /* settings record loaded after start-up */
        param_sync();
        break;
    default:
        if (feat_get(PARAM_MAGIC_ID) != PARAM_MAGIC)
            break;                  /* not ours yet: param_sync stores the defaults */
        for (int p = 0; p < P_COUNT; p++)
            if (id == param_id[p])
                param_apply(p, value);
        if (id == param_id[P_OCT] && st.active)
            draw_buttons();
        break;
    }
}

void tm_mode_init(void)
{
    volatile uint32_t *systick_val = (volatile uint32_t *)0xE000E018;
    tm_init(&tm, *systick_val ^ g_clock_ticks);
    st.note = -1;
    /* Define the persistent params at feature-init time like the stock features do, so
     * confstore restores them (and re-applies them through the listener) after loading. */
    for (int p = 0; p < P_COUNT; p++)
        feat_set(param_id[p], param_value(p), PARAM_PERSISTENT);
    feat_set(PARAM_MAGIC_ID, PARAM_MAGIC, PARAM_PERSISTENT);
}

/* Main loop (after input handling). */
void tm_mode_poll(void)
{
    static uint8_t started;
    if (!started) {
        started = 1;
        param_sync();
        set_active(feat_get(FEAT_PAD_MODE) == PAD_CUSTOM4);
    }
    uint8_t event = transport_event;
    if (event) {
        transport_event = 0;
        if (st.active)
            apply_transport(event, transport_clock);
    }
    if (!st.active)
        return;
    if (st.popup_pending) {
        st.popup_pending = 0;
        popup_overview();
    }
    uint16_t menu = 0;
    for (uint32_t id = 0; id < PAD_COUNT; id++)
        if (menu_owns(id))
            menu |= (uint16_t)(1u << id);
    if (menu != st.menu_pads) {
        uint16_t released = st.menu_pads & (uint16_t)~menu;
        st.menu_pads = menu;
        if (released) {
            draw_register();
            draw_notes();
        }
    }
    if (st.playing)
        advance_clock();
}
