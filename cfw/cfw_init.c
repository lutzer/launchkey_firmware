/* cfw entry points called from hooks in the stock firmware (see cfw/hooks.txt). */
#include <stdint.h>
#include "fw_api.h"
#include "tm_mode.h"

extern uint32_t __cfw_bss_start[], __cfw_bss_end[];

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
}

/* Hook: replaces `bl fmt_version` in ui_show_boot_screen (0x08016E42). Visible proof that cave code runs. */
uint32_t cfw_boot_version(char *buf, uint32_t pos, uint32_t size, const uint32_t *ver)
{
    pos = fmt_version(buf, pos, size, ver);
    return str_append(buf, pos, size, "+cfw");
}

/* Hooks: layout-name text (mode popups, home screen). Pad Custom 4 is Turing Machine mode. */
#define LAYOUT_PADS    1
#define PAD_CUSTOM4    8

static int is_tm_layout(uint32_t index, uint32_t type)
{
    return (type & 0xFF) == LAYOUT_PADS && index == PAD_CUSTOM4;
}

void cfw_layout_name_a(uint32_t screen, uint32_t field, uint32_t index, uint32_t type)
{
    if (is_tm_layout(index, type))
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
    int r = tm_io_event(0, id, v);
    if (r == TM_PASS)
        call_list2(&g_io_list_a, id, v);
    else if (r == TM_ENC_ONLY)
        relin_io_listener_a(id, v);
}

void cfw_io_dispatch_b(uint32_t id, uint32_t v)
{
    if (tm_io_event(1, id, v) == TM_PASS)
        call_list2(&g_io_list_b, id, v);
}

void cfw_io_dispatch_d(uint32_t id, uint32_t v)
{
    int r = tm_io_event(3, id, v);
    if (r == TM_PASS)
        call_list2(&g_io_list_d, id, v);
    else if (r == TM_ENC_ONLY)
        relin_io_listener_d(id, v);
}

void cfw_io_dispatch_c(uint32_t id)
{
    int r = tm_io_event(2, id, 0);
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
    if (!tm_enc_event(0, id, d))
        call_list3(&g_relin_list_pos, id, v, d);
}

void cfw_relin_dispatch_hires(uint32_t id, uint32_t v, int32_t d)
{
    if (!tm_enc_event(1, id, d))
        call_list3(&g_relin_list_hires, id, v, d);
}

void cfw_relin_dispatch_step(uint32_t id, uint32_t v, int32_t d)
{
    if (!tm_enc_event(2, id, d))
        call_list3(&g_relin_list_step, id, v, d);
}
