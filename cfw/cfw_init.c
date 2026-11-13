/* cfw entry points called from hooks in the stock firmware (see cfw/hooks.txt). */
#include <stdint.h>
#include "fw_api.h"
#include "tm_mode.h"

extern uint32_t __cfw_bss_start[], __cfw_bss_end[];

static void play_led_poll(void);
static void run_end_poll(void);

/* Hook: replaces `bl feat_scale_init` at the end of feature init (0x0801592C). */
void cfw_init(void)
{
    feat_scale_init();
    for (uint32_t *p = __cfw_bss_start; p < __cfw_bss_end; p++)
        *p = 0;
    tm_mode_init();
}

/* Hook: replaces one `bl confstore_poll` in the main loop (0x0801048A). */
void cfw_poll(void)
{
    static uint8_t started;
    confstore_poll();
    if (!started) {             /* first pass: all stock init is done */
        started = 1;
        feat_add_listener(tm_feat_changed);
    }
    tm_mode_poll();
    run_end_poll();
    play_led_poll();
}

/* Hook: replaces `bl fmt_version` in ui_show_boot_screen (0x08016E42). Visible proof that cave code runs. */
uint32_t cfw_boot_version(char *buf, uint32_t pos, uint32_t size, const uint32_t *ver)
{
    pos = fmt_version(buf, pos, size, ver);
    return str_append(buf, pos, size, "+cfw");
}

/* Hooks: layout-name text (mode popups, home screen). Pad Custom 4 is Turing Machine mode. */
#define LAYOUT_PADS    1

static int is_tm_layout(uint32_t index, uint32_t type)
{
    return (type & 0xFF) == LAYOUT_PADS && index == TM_PAD_MODE;
}

/* The pad-mode popup (0x08022E8C: screen 1, field 1) becomes the encoder overview right away, so the
 * stock popup doesn't flash up before tm_mode_poll shows it. */
void cfw_layout_name_a(uint32_t screen, uint32_t field, uint32_t index, uint32_t type)
{
    if (is_tm_layout(index, type) && screen == 1 && field == 1)
        tm_overview_text();
    else if (is_tm_layout(index, type))
        disp_text(screen, field, "Turing Machine");
    else
        layout_name_text_a(screen, field, index, type);
}

void cfw_layout_name_b(uint32_t screen, uint32_t field, uint32_t index, uint32_t type)
{
    if (is_tm_layout(index, type))
        disp_text(screen, field, "Turing Machine");
    else
        layout_name_text_b(screen, field, index, type);
}

/* Hooks: the MIDI realtime handler's calls for FA / FB / FC. */
void cfw_midi_start(void)
{
    clock_midi_start();
    tm_transport(TM_START);
}

void cfw_midi_continue(void)
{
    clock_midi_continue();
    tm_transport(TM_CONTINUE);
}

void cfw_midi_stop(void)
{
    clock_midi_stop();
    tm_transport(TM_STOP);
}

/* Hook: the Play button handler in the press dispatcher. Without a DAW connection Play is the transport
 * of Turing Machine mode and the arp (start / stop); the stock MIDI Start / Stop output is synced to it
 * first, so they agree. Shift + Play arms a one-shot run (tm_arm). With a DAW connected Play belongs to
 * the DAW only (stock).
 */
#define CTL_PLAY       0x48
#define FEAT_SHIFT     0x3F
#define PLAY_GREEN     0x00FF00

/* Play LED: green while playing, flashing while armed (standalone); with a DAW connected the DAW drives it. */
static void play_led(void)
{
    if (g_transport.standalone) {
        int armed = tm_armed() && !tm_playing();
        led_override_color(CTL_PLAY, tm_playing() || armed ? PLAY_GREEN : 0);
        led_override(CTL_PLAY, armed ? LED_FLASH : LED_STATIC);
    } else {
        led_override(CTL_PLAY, LED_RELEASE);
    }
}

/* Redraw the Play LED when the transport or the DAW connection changed (MIDI Start / Stop included). */
static void play_led_poll(void)
{
    static uint8_t drawn;       /* bit 0 playing, bit 1 standalone, bit 2 armed, bit 7 valid */
    uint8_t now = (uint8_t)(0x80 | (tm_playing() ? 1 : 0) | (g_transport.standalone ? 2 : 0) |
                            (tm_armed() ? 4 : 0));
    if (now != drawn) {
        drawn = now;
        play_led();
    }
}

/* A one-shot run stopped the transport: MIDI Stop, if our Play sent the Start (running = 1); a run
 * started by an external MIDI Start leaves running 0, so nothing is echoed back. */
static void run_end_poll(void)
{
    if (tm_run_done() && g_transport.standalone && g_transport.running)
        play_press(CTL_PLAY);
}

static void shift_play(void)
{
    if (tm_armed()) {
        tm_arm(0);
        tm_popup("Transport", "Not Playing");
    } else {
        if (tm_playing()) {     /* stop like Play does */
            g_transport.running = 1;
            play_press(CTL_PLAY);
            tm_play_button(0);
        }
        tm_arm(1);
        tm_popup("Transport", "Armed 8 Steps");
    }
    play_led();
}

void cfw_play_press(uint32_t id)
{
    if (id != CTL_PLAY || !g_transport.standalone) {
        play_press(id);
        return;
    }
    if (feat_get_value(FEAT_SHIFT)) {   /* Shift + Play arrives as 0x48 with Shift held */
        shift_play();
        return;
    }
    int start = !tm_playing();
    g_transport.running = (uint8_t)!start;
    play_press(id);
    tm_play_button(start);
    tm_popup("Transport", start ? "Playing" : "Not Playing");
    play_led();
}

/* Hook: last call of the Shift overlay LED refresh, after it drew the mode pads. */
void cfw_shift_leds(void)
{
    shift_leds_tail();
    tm_shift_leds();
    if (g_transport.standalone)
        play_led();
}

/* ---- input dispatchers: replace the stock ones (see cfw/hooks.txt) ---- */

typedef void (*io2_fn)(uint32_t, uint32_t);
typedef void (*io1_fn)(uint32_t);

static void call_list2(const io_list_t *l, uint32_t id, uint32_t v)
{
    for (unsigned i = 0; i < l->count; i++)
        ((io2_fn)l->fn[i])(id, v);
}

void cfw_io_dispatch_a(uint32_t id, uint32_t v)
{
    int r = tm_io_event(TM_IO_PRESS, id, v);
    if (r == TM_PASS)
        call_list2(&g_io_list_a, id, v);
    else if (r == TM_ENC_ONLY)
        relin_io_listener_a(id, v);
}

void cfw_io_dispatch_b(uint32_t id, uint32_t v)
{
    if (tm_io_event(TM_IO_B, id, v) == TM_PASS)
        call_list2(&g_io_list_b, id, v);
}

void cfw_io_dispatch_d(uint32_t id, uint32_t v)
{
    int r = tm_io_event(TM_IO_D, id, v);
    if (r == TM_PASS)
        call_list2(&g_io_list_d, id, v);
    else if (r == TM_ENC_ONLY)
        relin_io_listener_d(id, v);
}

void cfw_io_dispatch_c(uint32_t id)
{
    int r = tm_io_event(TM_IO_RELEASE, id, 0);
    if (r == TM_ENC_ONLY) {
        relin_io_listener_c(id);
    } else if (r == TM_PASS) {
        for (unsigned i = 0; i < g_io_list_c.count; i++)
            ((io1_fn)g_io_list_c.fn[i])(id);
    }
}

static void call_list3(const io_list_t *l, uint32_t id, uint32_t v, int32_t d)
{
    for (unsigned i = 0; i < l->count; i++)
        ((relin_listener_fn)l->fn[i])(id, v, d);
}

void cfw_relin_dispatch_pos(uint32_t id, uint32_t v, int32_t d)
{
    if (!tm_enc_event(TM_REL_POS, id, d))
        call_list3(&g_relin_list_pos, id, v, d);
}

void cfw_relin_dispatch_hires(uint32_t id, uint32_t v, int32_t d)
{
    if (!tm_enc_event(TM_REL_HIRES, id, d))
        call_list3(&g_relin_list_hires, id, v, d);
}

void cfw_relin_dispatch_step(uint32_t id, uint32_t v, int32_t d)
{
    if (!tm_enc_event(TM_REL_STEP, id, d))
        call_list3(&g_relin_list_step, id, v, d);
}
