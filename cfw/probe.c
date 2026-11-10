/* Phase 1 probe: reports input events and feature-param changes as SysEx
 *     F0 00 20 29 7D <type> <data...> F7
 * on both app MIDI ports, decoded on the host by tools/probe_listen.py. Temporary
 * (removed once tm_mode.c uses the findings).
 *
 *   type 1 io      list(0..3 = A..D), id, value lo7, value hi7
 *   type 2 enc     list(0 = pos, 2 = step; 1 = hires not reported), id, delta + 64, value lo7, value hi7
 *   type 3 feat    id, value lo7, value hi7
 *   type 4 stack   free bytes lo7, hi7
 *   type 5 mode    1 = took over pads + encoders 1..5 (pad mode Custom 4), 0 = released
 *
 * Pad events reach the stock listeners through cfw_io_dispatch_* (hooks replace the
 * io_listener dispatchers). While the takeover is active and Shift is up, a pad's press
 * and everything until its release go only to the probe.
 */
#include <stdint.h>
#include "fw_api.h"

uint32_t cfw_stack_free(void);

static uint8_t registered;

static void report(const uint8_t *data, int n)
{
    static const uint8_t hdr[] = {0xF0, 0x00, 0x20, 0x29, 0x7D};
    for (int p = 0; p < 2; p++) {
        midi_port_t *port = g_app_ports[p];
        if (!port)
            continue;
        for (unsigned i = 0; i < sizeof hdr; i++)
            midi_put(port, hdr[i]);
        for (int i = 0; i < n; i++)
            midi_put(port, data[i] & 0x7F);
        midi_put(port, 0xF7);
    }
}

/* --- probe 3: take over pads + encoders 1..5 while pad mode is Custom 4 --- */
#define FEAT_PAD_MODE     0x1D
#define FEAT_SHIFT        0x3F
#define PAD_CUSTOM4       8
#define PAD_COUNT         16
#define ENC_FIRST         56
#define ENC_COUNT         5

static uint8_t active, shift;
static uint16_t pad_lit;
static uint32_t owned;          /* controls whose current press belongs to the probe (slot bits) */
static uint16_t note_on;        /* pads with a sounding test note */

/* bit slot for controls the takeover may own: pads 0..15, encoders 1..5 -> 16..20 */
static int ctl_slot(uint32_t id)
{
    if (id < PAD_COUNT)
        return (int)id;
    if (id >= ENC_FIRST && id < ENC_FIRST + ENC_COUNT)
        return (int)(16 + id - ENC_FIRST);
    return -1;
}

static void draw_pads(void)
{
    for (uint32_t id = 0; id < PAD_COUNT; id++) {
        int bright = pad_lit >> id & 1;
        uint32_t rgb = id < 8 ? (bright ? 0x0000FF : 0x000018) : (bright ? 0x00FF00 : 0x001800);
        led_override_color(id, rgb);
        led_override(id, LED_STATIC);
    }
}

static void test_note(uint32_t pad, int on)
{
    for (int p = 0; p < 2; p++)
        if (g_app_ports[p])
            midi_send3(g_app_ports[p], (on ? 0x90 : 0x80) | p, 60 + pad, on ? 100 : 0);
}

static void set_active(int on)
{
    if (on == active)
        return;
    active = (uint8_t)on;
    if (on) {
        draw_pads();
    } else {
        for (uint32_t id = 0; id < PAD_COUNT; id++)
            led_override(id, LED_RELEASE);
        for (int p = 0; p < 2; p++)
            if (g_app_ports[p])
                midi_send3(g_app_ports[p], 0xB0 | p, 123, 0);   /* all notes off */
        note_on = 0;
    }
    uint8_t m[] = {5, (uint8_t)on};
    report(m, sizeof m);
}

/* Returns 1 if the event is consumed (stock listeners must not see it). */
static int io_event(int list, uint32_t id, uint32_t value)
{
    uint8_t m[] = {1, (uint8_t)list, (uint8_t)id, (uint8_t)value, (uint8_t)(value >> 7)};
    report(m, sizeof m);
    int slot = ctl_slot(id);
    if (slot < 0)
        return 0;
    uint32_t bit = 1u << slot;
    if (list == 0) {                       /* press decides who owns the control until release */
        int take = active && !shift;
        owned = take ? owned | bit : owned & ~bit;
        if (take && !shift && id < PAD_COUNT) {
            pad_lit ^= (uint16_t)bit;
            draw_pads();
            test_note(id, 1);
            note_on |= (uint16_t)bit;
        }
    }
    if (!(owned & bit))
        return 0;
    if (list == 2) {                       /* release */
        if (id < PAD_COUNT && (note_on & bit)) {
            test_note(id, 0);
            note_on &= (uint16_t)~bit;
        }
        owned &= ~bit;
    }
    return 1;
}

typedef void (*io2_fn)(uint32_t, uint32_t);
typedef void (*io1_fn)(uint32_t);

static void call_list2(const io_list_t *l, uint32_t id, uint32_t v)
{
    for (unsigned i = 0; i < l->count; i++)
        ((io2_fn)l->fn[i])(id, v);
}

/* Hooks: replace the stock io_listener dispatchers (see cfw/hooks.txt). */
void cfw_io_dispatch_a(uint32_t id, uint32_t v) { if (!io_event(0, id, v)) call_list2(&g_io_list_a, id, v); }
void cfw_io_dispatch_b(uint32_t id, uint32_t v) { if (!io_event(1, id, v)) call_list2(&g_io_list_b, id, v); }
void cfw_io_dispatch_d(uint32_t id, uint32_t v) { if (!io_event(3, id, v)) call_list2(&g_io_list_d, id, v); }
void cfw_io_dispatch_c(uint32_t id)
{
    if (io_event(2, id, 0))
        return;
    for (unsigned i = 0; i < g_io_list_c.count; i++)
        ((io1_fn)g_io_list_c.fn[i])(id);
}

/* Returns 1 if the encoder event is consumed. */
static int enc_event(int list, uint32_t id, uint32_t value, int32_t delta)
{
    uint8_t m[] = {2, (uint8_t)list, (uint8_t)id, (uint8_t)(delta + 64), (uint8_t)value, (uint8_t)(value >> 7)};
    if (list != 1)          /* hires events only matter for the decision, keep the log readable */
        report(m, sizeof m);
    return active && !shift && id >= ENC_FIRST && id < ENC_FIRST + ENC_COUNT;
}

static void call_list3(const io_list_t *l, uint32_t id, uint32_t v, int32_t d)
{
    for (unsigned i = 0; i < l->count; i++)
        ((relin_listener_fn)l->fn[i])(id, v, d);
}

/* Hooks: replace the stock encoder dispatchers (see cfw/hooks.txt). */
void cfw_relin_dispatch_pos(uint32_t id, uint32_t v, int32_t d)
{
    if (!enc_event(0, id, v, d))
        call_list3(&g_relin_list_pos, id, v, d);
}

void cfw_relin_dispatch_hires(uint32_t id, uint32_t v, int32_t d)
{
    if (!enc_event(1, id, v, d))
        call_list3(&g_relin_list_hires, id, v, d);
}

void cfw_relin_dispatch_step(uint32_t id, uint32_t v, int32_t d)
{
    if (!enc_event(2, id, v, d))
        call_list3(&g_relin_list_step, id, v, d);
}

static void stack_report(void)
{
    uint32_t f = cfw_stack_free();
    uint8_t m[] = {4, (uint8_t)f, (uint8_t)(f >> 7)};
    report(m, sizeof m);
}

static void feat_changed(uint32_t id, uint32_t value)
{
    uint8_t m[] = {3, (uint8_t)id, (uint8_t)value, (uint8_t)(value >> 7)};
    report(m, sizeof m);
    stack_report();
    if (id == FEAT_PAD_MODE)
        set_active(value == PAD_CUSTOM4);
    else if (id == FEAT_SHIFT) {
        shift = value != 0;
        if (!shift && active)
            draw_pads();    /* the Shift overlay may have used the LED override layer */
    }
}

void probe_poll(void)
{
    if (registered)
        return;
    registered = 1;
    feat_add_listener(feat_changed);
    stack_report();

    fw_param_t p;
    confsys_get_param(FW_GROUP_FEATURE, FEAT_PAD_MODE, &p);
    set_active(p.value == PAD_CUSTOM4);
}
