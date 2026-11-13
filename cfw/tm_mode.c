/* Turing Machine mode (replaces pad mode Custom 4).
 *
 * Active while feature param 0x1D (pad mode) = 8 and Shift is up:
 *   top pads 0..7      register bits (newest at pad 1), green, the loop's first step yellow; dark
 *                      beyond the loop length.
 *                      While Function is held both rows show the loop in play order, its first
 *                      step (yellow) on pad 1, the playhead mint (amber on the first step),
 *                      and a press flips that step's bit.
 *   bottom pads 8..15  scale notes (pad 9 = root, ...), white; press toggles (Scale on only);
 *                      pads beyond the scale size are dark, all eight lit with Scale off
 *   Function (17)      hold: while held the pads show the loop anchored at its first step (live,
 *                      so a locked loop stands still) and pad presses flip the step they show
 *   > (16)             enable/disable (above Function, saved): disabled mutes the notes; the
 *                      sequencer keeps following clock, transport and Play (pads keep moving)
 *   Play (0x48)        start from the first step / stop, in every mode, without a DAW
 *                      connection only (cfw_play_press); the sequencer starts stopped
 *   Shift + Play       arm a one-shot run: the next start plays run_steps x 1/16, then stops;
 *                      while armed encoder 1 sets run_steps (1..32, saved), in every pad mode
 *   up/down (34/35)    octave; with Function held: rotate the loop one step right / left (the
 *                      pattern moves under the start and the playhead, heard from the next step)
 *   encoders 1..8      probability, length, rate, range, gate length, trigger heads, velocity
 *                      depth, velocity head (moves the register window velocity is read from,
 *                      see tm_velocity); mapping and ranges: params[] / enc_param[]
 * MIDI transport (also while another pad mode is shown or disabled): Start = back to the first
 * step, which plays on the next clock tick (the downbeat), Continue = resume, Stop = pause.
 * Outside the mode the LEDs and popups are left to the stock firmware.
 * Parameters and the note mask are saved as persistent feature params (see PARAM_*).
 * A step plays a note depending on the trigger mode (tm_heads: every step, OR / AND / XOR of
 * delayed register bits, ~88 % .. ~12 % dense); every note lasts the gate length and
 * is released before the next step. Gate Tie (after 95 %): 95 %, but a note followed by another
 * one is held into it (same pitch: one long note, else legato: new note on, then old note off).
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
#define CTL_SHIFT         25
#define PAD_SELECT        15          /* Shift + this pad selects pad Custom 4 */

#define PAD_COUNT         16
#define PAD_ROW           TM_PADS
#define NOTE8_BIT         0x80        /* 8th scale note (bottom pad 8, 8-note scales only) */
#define ENC_FIRST         56          /* encoder 1 */
#define ENC_COUNT         8
/* Buttons next to the pads (ids from the DAW-mode CC map at 0x0801C180: CC 104..107). */
#define BTN_ENABLE        16          /* ">" (scene launch), above Function */
#define BTN_HOLD          17          /* Function */
#define BTN_OCT_UP        34
#define BTN_OCT_DOWN      35

#define MIDI_PORT         1           /* g_app_ports[1] = USB MIDI port (+ DIN via link) */

#define GREEN_ON          0x00FF00
#define GREEN_DIM         0x001800
#define GREEN_DARK        0x000C00    /* Shift overlay: the mode pad while disabled */
#define YELLOW_ON         0xFFFF00    /* the first step played after a rewind (Start / Play reset) */
#define YELLOW_DIM        0x181800
#define MINT_ON           0x00FF80    /* Function view: the playhead (step in bit 0, just played) */
#define MINT_DIM          0x00180C
#define AMBER_ON          0xFF8000    /* Function view: the playhead on the first step */
#define AMBER_DIM         0x180C00
#define WHITE_ON          0xFFFFFF
#define WHITE_DIM         0x181818
#define LED_OFF           0x000000

/* Parameters: every encoder and the octave arrows change one through param_change.
 * Persistent ones are feature params (free ids >= 0x40: ids 0x20..0x3F double as 14-bit LSBs).
 * Feature values are 7 bits: P_MASK holds notes 1..7, P_NOTE8_OFF the 8th note inverted (records
 * from before it existed read 0 = on, as the 8th note used to be always on). P_ENABLE is stored
 * inverted too (0 = enabled), for the same reason. P_VEL_HEAD 0 = the window before it existed.
 * P_RUN (one-shot run length, Shift + Play) is stored XOR RUN_STEPS_DEFAULT, so 0 = the default. */
enum { P_PROB, P_LEN, P_RATE, P_RANGE, P_GATE, P_HEADS, P_OCT, P_MASK, P_CHAN, P_NOTE8_OFF, P_VEL, P_ENABLE,
       P_VEL_HEAD, P_RUN, P_COUNT };
typedef struct {
    uint8_t id;             /* feature param id, 0 = not stored */
    uint8_t min, max;       /* stored value range (feature values are 7 bits, so max <= 127) */
    uint8_t step;           /* change per encoder detent / tick */
    uint8_t fine;           /* encoder reacts to every tick, else per detent */
    const char *label;      /* short name in the mode popup (encoders only) */
} param_def_t;
static const param_def_t params[P_COUNT] = {
    [P_PROB]      = {0x50, 0, TM_PROB_MAX, 1, 1, "Prob"},
    [P_LEN]       = {0x51, TM_LEN_MIN, TM_LEN_MAX, 1, 0, "Len"},
    [P_RATE]      = {0x52, 0, TM_NUM_DIVS - 1, 1, 0, "Rate"},
    [P_RANGE]     = {0x5B, TM_RANGE_MIN, TM_RANGE_MAX, 1, 0, "Rng"},
    [P_GATE]      = {0x5D, TM_GATE_MIN, TM_GATE_TIE, TM_GATE_STEP, 0, "Gate"},
    [P_HEADS]     = {0x5E, 0, TM_NUM_HEADS - 1, 1, 0, "Head"},
    [P_OCT]       = {0x5A, 0, TM_OCT_MAX, 1, 0, 0},
    [P_MASK]      = {0x5F, 0, 0x7F, 1, 0, 0},
    [P_CHAN]      = {TM_CHANNEL_PARAM, 0, 15, 1, 0, 0},
    [P_NOTE8_OFF] = {0x62, 0, 1, 1, 0, 0},
    [P_VEL]       = {0x63, 0, TM_VEL_MAX, TM_VEL_STEP, 0, "Vel"},
    [P_ENABLE]    = {0x41, 0, 1, 1, 0, 0},
    [P_VEL_HEAD]  = {0x42, 0, TM_VEL_HEADS - 1, 1, 0, "VHd"},
    [P_RUN]       = {0x43, 0, 40, 1, 0, 0},
};
#define RUN_STEPS_DEFAULT 8
#define RUN_STEPS_MAX     32
/* encoders 1..8 */
static const uint8_t enc_param[ENC_COUNT] = {P_PROB, P_LEN, P_RATE, P_RANGE, P_GATE, P_HEADS, P_VEL, P_VEL_HEAD};
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
    uint8_t hold;            /* Function down: loop shown from its first step, bits can be flipped */
    uint8_t playing;         /* transport: Play button / MIDI Start..Stop, in every pad mode */
    uint8_t enabled;         /* ">": notes out; disabled = muted, everything else keeps running */
    uint8_t priority;        /* set by (re)selecting pad Custom 4: our controls win over a menu
                              * (Scale/Arp screens keep them captured); cleared by any other
                              * button press, i.e. whenever a menu may be opened */
    uint8_t pos;             /* steps since the loop start, mod length (for rewinding) */
    uint8_t popup_pending;   /* show the mode popup from the main loop, after the stock one */
    uint8_t armed;           /* Shift + Play: the next start is a one-shot run */
    uint8_t run;             /* one-shot run in progress: stops after run_steps 1/16 steps */
    uint8_t run_steps;       /* one-shot run length 1..RUN_STEPS_MAX (encoder 1 while armed) */
    uint8_t run_done;        /* a run ended by itself (tm_run_done) */
    uint32_t run_start;      /* clock tick the run started on */
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

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
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
    case P_ENABLE: return !st.enabled;
    case P_MASK:  return tm.mask & 0x7F;
    case P_NOTE8_OFF: return !(tm.mask & NOTE8_BIT);
    case P_VEL_HEAD: return tm.vel_head;
    case P_RUN:   return st.run_steps ^ RUN_STEPS_DEFAULT;
    default:      return 0;
    }
}

/* Set a stored parameter (from an encoder, a button or the settings store / host), clamped to its range. */
static void param_apply(int p, int value)
{
    uint8_t v = (uint8_t)clamp(value, params[p].min, params[p].max);
    switch (p) {
    case P_PROB:  tm.prob = v; break;
    case P_LEN:
        if (v == tm.len)
            break;
        tm.len = v;
        tm_tile(&tm);
        st.pos = (uint8_t)(st.pos % v);
        break;
    case P_RATE:  tm.div = v; break;
    case P_OCT:   tm.octave = v; break;
    case P_RANGE: tm.range = v; break;
    case P_GATE:  tm.gate = v; break;
    case P_HEADS: tm.heads = v; break;
    case P_MASK:  tm.mask = (uint8_t)(v | (tm.mask & NOTE8_BIT)); break;
    case P_NOTE8_OFF: tm.mask = (uint8_t)((tm.mask & 0x7F) | (v ? 0 : NOTE8_BIT)); break;
    case P_CHAN:  midi_channel = v; break;
    case P_VEL:   tm.vel = v; break;
    case P_VEL_HEAD: tm.vel_head = v; break;
    case P_ENABLE: st.enabled = v == 0; break;     /* tm_mode_poll releases a muted note */
    case P_RUN:   st.run_steps = (uint8_t)clamp(v ^ RUN_STEPS_DEFAULT, 1, RUN_STEPS_MAX); break;
    }
}

static void param_save(int p)
{
    if (params[p].id)
        feat_set(params[p].id, param_value(p), 0);
}

/* Stored values valid -> load them; otherwise (first boot, old settings record) store ours. */
static void param_sync(void)
{
    if (feat_get(PARAM_MAGIC_ID) == PARAM_MAGIC) {
        for (int p = 0; p < P_COUNT; p++)
            if (params[p].id)
                param_apply(p, (int)feat_get(params[p].id));
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

/* ---- output ---- */

static void send_note_off(uint32_t channel, int note)
{
    if (note >= 0 && g_app_ports[MIDI_PORT])
        midi_send3(g_app_ports[MIDI_PORT], 0x80 | channel, (uint32_t)note, 0);
}

static void note_off(void)
{
    send_note_off(st.channel, st.note);
    st.note = -1;
}

static void note_on(int note)
{
    if (note < 0 || !st.enabled || !g_app_ports[MIDI_PORT])     /* disabled = muted */
        return;
    st.channel = midi_channel;
    midi_send3(g_app_ports[MIDI_PORT], 0x90 | st.channel, (uint32_t)note, (uint32_t)tm_velocity(&tm));
    st.note = (int8_t)note;
}

/* Only while the mode is shown (it keeps running in the background). Pads captured by a menu
 * (e.g. Settings) are left to the menu; tm_mode_poll redraws them once the menu lets go. */
static void led(uint32_t id, uint32_t rgb)
{
    if (!st.active || (id < PAD_COUNT && (st.menu_pads >> id & 1)))
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
    uint32_t pads = st.hold ? PAD_COUNT : PAD_ROW;  /* held: steps 9..16 on the bottom row */
    /* held: pad i = step i of the loop, so the first step is always pad 1 */
    uint32_t start = st.hold ? 0 : (uint32_t)tm_start_bit(&tm, st.pos);
    for (uint32_t i = 0; i < pads; i++) {
        int b = st.hold && i < tm.len ? tm_loop_bit(&tm, st.pos, (int)i) : (int)i;
        int bit = tm.reg >> b & 1;
        uint32_t rgb;
        if (i >= tm.len)
            rgb = LED_OFF;
        else if (st.hold && b == 0)     /* bit 0 = the step playing now */
            rgb = i == start ? (bit ? AMBER_ON : AMBER_DIM) : (bit ? MINT_ON : MINT_DIM);
        else if (i == start)
            rgb = bit ? YELLOW_ON : YELLOW_DIM;
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
    led(BTN_ENABLE, st.enabled ? WHITE_ON : WHITE_DIM);
    led(BTN_HOLD, st.hold ? WHITE_ON : WHITE_DIM);
    led(BTN_OCT_UP, st.hold || tm.octave < TM_OCT_MAX ? WHITE_ON : WHITE_DIM);
    led(BTN_OCT_DOWN, st.hold || tm.octave > 0 ? WHITE_ON : WHITE_DIM);
}

static void draw_all(void)
{
    draw_register();
    draw_notes();
    draw_buttons();
}

static const uint8_t buttons[] = {BTN_ENABLE, BTN_HOLD, BTN_OCT_UP, BTN_OCT_DOWN};
#define BTN_COUNT (sizeof buttons)

/* ui_popup_begin runs the stock views' callbacks, which repaint button LEDs: redraw ours after it. */
static void popup(const char *title, const char *value)
{
    ui_popup_begin();
    draw_buttons();
    disp_set_screen(1, 0);
    disp_text(1, 0, title);
    disp_text(1, 1, value);
}

/* Mode popup: title + short encoder labels in the stock 8-field overview layout (template 4:
 * field 0 = title, fields 1..4 / 5..8 = encoders 1..4 / 5..8, 32 px each). */
#define TEMPLATE_OVERVIEW 4

void tm_overview_text(void)
{
    disp_set_screen(1, TEMPLATE_OVERVIEW);
    disp_text(1, 0, "Turing Machine");
    for (uint32_t i = 0; i < ENC_COUNT; i++)
        disp_text(1, 1 + i, params[enc_param[i]].label);
}

static void popup_overview(void)
{
    ui_popup_begin();
    draw_buttons();
    tm_overview_text();
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
        p = put_num(put_str(p, "+"), tm.range);
        if (tm.range % 12)
            p = put_str(p, " semitones");
        else            /* "+24 = 2 Oct" */
            p = put_str(put_num(put_str(p, " = "), tm.range / 12u), " Oct");
        break;
    case P_GATE:
        title = "Gate";
        p = tm.gate == TM_GATE_TIE ? put_str(p, "Tie") : put_str(put_num(p, tm.gate), "%");
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
    case P_VEL_HEAD: {
        uint32_t lo = (uint32_t)tm_vel_shift(&tm) + 1u;     /* "Bits 5-11" */
        title = "Vel Head";
        p = put_num(put_str(put_num(put_str(p, "Bits "), lo), "-"), lo + 6u);
        break;
    }
    }
    *p = 0;
    popup(title, v);
}

/* An encoder or button changes parameter p by delta steps. */
static void param_change(int p, int delta)
{
    param_apply(p, (int)param_value(p) + delta * params[p].step);
    param_save(p);
    draw_all();
    show_param(p);
}

/* ---- sequencing ---- */

/* A note still sounding here was tied over (Gate Tie): the same pitch keeps sounding, another one
 * starts before the held one stops (legato). */
static void step(void)
{
    int held = st.note;
    uint32_t held_channel = st.channel;
    tm_step(&tm);
    st.pos = (uint8_t)((st.pos + 1) % tm.len);
    int note = -1;
    if (tm_gate(&tm))
        note = tm_note(&tm, (int)feat_get(FEAT_SCALE_ROOT) % 12, scale_semis(), feat_get(FEAT_SCALE_ON) != 0);
    if (held < 0 || note != held || !st.enabled || midi_channel != held_channel) {
        st.note = -1;
        note_on(note);
        send_note_off(held_channel, held);
    }
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
            if (tm.gate != TM_GATE_TIE || !tm_next_gate(&tm))
                note_off();     /* before the next step, unless tied into it */
        }
    }
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
        st.playing = 0;
        st.run = 0;
        note_off();
        return;
    }
    if (event == TM_START) {
        st.playing = 0;
        rewind();
    } else if (st.playing) {
        return;                 /* Continue while running: nothing to do */
    }
    if (st.armed || (event == TM_START && st.run)) {   /* a restart during a run restarts it */
        st.armed = 0;
        st.run = 1;
        st.run_start = clock;
    }
    st.playing = 1;
    st.last_clock = clock;
}

int tm_playing(void)
{
    return st.playing;
}

void tm_arm(int on)
{
    st.armed = (uint8_t)(on != 0);
}

int tm_armed(void)
{
    return st.armed;
}

void tm_run_popup(void)
{
    char v[20];
    *put_str(put_num(put_str(v, "Armed "), st.run_steps), " Steps") = 0;    /* put_* don't terminate */
    popup("Transport", v);
}

int tm_run_done(void)
{
    int done = st.run_done;
    st.run_done = 0;
    return done;
}

/* Shift held: the pad that selects the mode is green, bright = enabled (stock: blue when selected). */
void tm_shift_leds(void)
{
    if (!ui_menu_owns(PAD_SELECT, UI_MENU_SHIFT))
        return;
    led_override_color(PAD_SELECT, st.enabled ? GREEN_ON : GREEN_DARK);
    led_override(PAD_SELECT, LED_STATIC);
}

/* Play button (standalone): like MIDI Start / Stop. */
void tm_play_button(int start)
{
    apply_transport(start ? TM_START : TM_STOP, g_clock_ticks);
}

void tm_popup(const char *title, const char *value)
{
    popup(title, value);
}

/* ---- mode switching ---- */

static void set_active(int on)
{
    if (on == st.active)
        return;
    st.active = (uint8_t)on;
    if (on) {                   /* playback state carries over: it runs in every mode */
        draw_all();
        st.popup_pending = 1;   /* the stock "Custom 4" popup comes after this listener call */
    } else {
        st.hold = 0;
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

static int is_enc(uint32_t id)
{
    return id >= ENC_FIRST && id < ENC_FIRST + ENC_COUNT;
}

/* Armed (Shift + Play): encoder 1 sets the one-shot run length, in every pad mode. */
static int run_enc(uint32_t id)
{
    return st.armed && id == ENC_FIRST;
}

/* Bit index in st.owned: pads, then encoders, then the mode buttons. */
#define SLOT_ENC  PAD_COUNT
#define SLOT_BTN  (SLOT_ENC + ENC_COUNT)

static int ctl_slot(uint32_t id)
{
    if (id < PAD_COUNT)
        return (int)id;
    if (is_enc(id))
        return (int)(SLOT_ENC + id - ENC_FIRST);
    for (uint32_t i = 0; i < BTN_COUNT; i++)
        if (id == buttons[i])
            return (int)(SLOT_BTN + i);
    return -1;
}

static void pad_pressed(uint32_t id)
{
    if (st.hold) {                              /* flips need Function held */
        if ((int)id >= tm.len)
            return;
        if (tm_flip(&tm, tm_loop_bit(&tm, st.pos, (int)id)))
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
    case BTN_ENABLE:
        st.enabled = !st.enabled;
        param_save(P_ENABLE);
        draw_buttons();
        popup("Turing Machine", st.enabled ? "Enabled" : "Disabled");
        break;
    case BTN_HOLD:
        st.hold = 1;
        draw_register();
        draw_buttons();
        break;
    case BTN_OCT_UP:
    case BTN_OCT_DOWN:
        if (st.hold) {
            /* rotating the loop back one step moves every step one pad right in the Function view */
            tm_rewind(&tm, id == BTN_OCT_UP ? 1 : tm.len - 1);
            draw_register();
            break;
        }
        param_change(P_OCT, id == BTN_OCT_UP ? 1 : -1);
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

/* list = TM_IO_*. Returns TM_PASS / TM_CONSUME / TM_ENC_ONLY. */
int tm_io_event(int list, uint32_t id, uint32_t value)
{
    (void)value;
    int slot = ctl_slot(id);
    if (slot < 0) {
        if (list == TM_IO_PRESS && id != CTL_SHIFT)
            st.priority = 0;    /* any other button may open a menu: menus win again */
        return TM_PASS;
    }
    uint32_t bit = 1u << slot;
    if (list == TM_IO_PRESS) {  /* the press decides who owns the control until its release */
        int take;
        if (run_enc(id))
            take = 1;
        else if (st.shift)      /* Shift + encoder = show value; the Shift overlay's capture doesn't count */
            take = st.active && is_enc(id);
        else
            take = st.active && !(slot >= SLOT_BTN ? menu_owns_button(id) : menu_owns(id));
        st.owned = take ? st.owned | bit : st.owned & ~bit;
        if (take && slot < SLOT_ENC)
            pad_pressed(id);
        else if (take && slot >= SLOT_BTN)
            button_pressed(id);
    }
    if (!(st.owned & bit))
        return TM_PASS;
    if (list == TM_IO_RELEASE) {
        st.owned &= ~bit;
        if (slot >= SLOT_BTN)
            button_released(id);
    }
    return is_enc(id) ? TM_ENC_ONLY : TM_CONSUME;
}

/* list = TM_REL_*. Each encoder changes its parameter (enc_param), per tick or per detent. */
int tm_enc_event(int list, uint32_t id, int32_t delta)
{
    delta = (int16_t)delta;     /* the encoder module passes deltas as u16 (-1 = 0xFFFF) */
    if (run_enc(id)) {
        if (list == TM_REL_STEP) {
            st.run_steps = (uint8_t)clamp(st.run_steps + delta, 1, RUN_STEPS_MAX);
            param_save(P_RUN);
            tm_run_popup();
        }
        return 1;
    }
    if (!st.active || !is_enc(id) || (!st.shift && menu_owns(id)))
        return 0;
    int p = enc_param[id - ENC_FIRST];
    if (list != (params[p].fine ? TM_REL_POS : TM_REL_STEP))
        return 1;
    if (st.shift)               /* Shift + turn: show the value without changing it */
        show_param(p);
    else
        param_change(p, delta);
    return 1;
}

void tm_feat_changed(uint32_t id, uint32_t value)
{
    switch (id) {
    case FEAT_PAD_MODE:
        set_active(value == TM_PAD_MODE);
        if (value == TM_PAD_MODE) {     /* (re)selected: take the controls back from any menu */
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
        for (int p = 0; p < P_COUNT; p++) {     /* also set by the host (channel-7 CCs) */
            if (params[p].id && id == params[p].id) {
                uint32_t old = param_value(p);
                param_apply(p, (int)value);
                if (st.active && param_value(p) != old)    /* not the echo of our own param_save */
                    draw_all();
                break;
            }
        }
        break;
    }
}

void tm_mode_init(void)
{
    volatile uint32_t *systick_val = (volatile uint32_t *)0xE000E018;
    tm_init(&tm, *systick_val ^ g_clock_ticks);
    st.note = -1;
    st.enabled = 1;
    st.run_steps = RUN_STEPS_DEFAULT;
    /* Define the persistent params at feature-init time like the stock features do, so
     * confstore restores them (and re-applies them through the listener) after loading. */
    for (int p = 0; p < P_COUNT; p++)
        if (params[p].id)
            feat_set(params[p].id, param_value(p), PARAM_PERSISTENT);
    feat_set(PARAM_MAGIC_ID, PARAM_MAGIC, PARAM_PERSISTENT);
}

/* Main loop (after input handling). */
void tm_mode_poll(void)
{
    static uint8_t started;
    if (!started) {
        started = 1;
        param_sync();
        set_active(feat_get(FEAT_PAD_MODE) == TM_PAD_MODE);
    }
    uint8_t event = transport_event;
    if (event) {
        transport_event = 0;
        apply_transport(event, transport_clock);
    }
    /* end of a one-shot run: checked before stepping, so the step after it never plays */
    if (st.playing && st.run && g_clock_ticks - st.run_start >= (uint32_t)st.run_steps * TM_RUN_STEP_TICKS) {
        apply_transport(TM_STOP, g_clock_ticks);
        st.run_done = 1;
    }
    if (st.playing) {
        advance_clock();
    } else {                    /* stopped: the clock passes by */
        st.last_clock = g_clock_ticks;
        note_off();
    }
    if (!st.enabled)            /* muted: release a note sounding when it was disabled */
        note_off();
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
}
