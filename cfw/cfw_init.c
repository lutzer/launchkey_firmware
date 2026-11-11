/* cfw entry points called from hooks in the stock firmware (see cfw/hooks.txt). */
#include <stdint.h>
#include "fw_api.h"
#include "probe.h"
#include "tm_mode.h"

#define STACK_PAINT      0xA5A5A5A5u
#define STACK_PAINT_LO   0x2000F620u   /* above CFW RAM (0x2000F520..0x2000F620) */
#define STACK_PAINT_GAP  512u          /* keep clear of the live stack / interrupt frames */

extern uint32_t __cfw_bss_start[], __cfw_bss_end[];

static void paint_stack(void)
{
    uint32_t sp, primask;
    __asm volatile("mov %0, sp" : "=r"(sp));
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");
    for (volatile uint32_t *p = (uint32_t *)STACK_PAINT_LO; (uint32_t)p < sp - STACK_PAINT_GAP; p++)
        *p = STACK_PAINT;
    __asm volatile("msr primask, %0" :: "r"(primask) : "memory");
}

/* Bytes of painted stack that were never touched (stack headroom left). */
uint32_t cfw_stack_free(void)
{
    const uint32_t *p = (const uint32_t *)STACK_PAINT_LO;
    while (*p == STACK_PAINT)
        p++;
    return (uint32_t)p - STACK_PAINT_LO;
}

/* Hook: replaces `bl feat_scale_init` at the end of feature init (0x0801592C). */
void cfw_init(void)
{
    feat_scale_init();
    for (uint32_t *p = __cfw_bss_start; p < __cfw_bss_end; p++)
        *p = 0;
    paint_stack();
    tm_mode_init();
}

/* One feature listener for all cfw code (only two stock slots were free). */
static void feat_changed(uint32_t id, uint32_t value)
{
    probe_feat(id, value);
    if (id == 0x1D && value == 8)   /* entering pad Custom 4: dump feature flags (dev) */
        probe_flags();
    tm_feat_changed(id, value);
}

/* Hook: replaces one `bl confstore_poll` in the main loop (0x0801048A). */
void cfw_poll(void)
{
    static uint8_t started;
    confstore_poll();
    if (!started) {             /* first pass: all stock init is done */
        started = 1;
        feat_add_listener(feat_changed);
        probe_stack();
    }
    tm_mode_poll();
}

/* Hook: replaces `bl fmt_version` in ui_show_boot_screen (0x08016E42). Visible proof that cave code runs. */
uint32_t cfw_boot_version(char *buf, uint32_t pos, uint32_t size, const uint32_t *ver)
{
    pos = fmt_version(buf, pos, size, ver);
    return str_append(buf, pos, size, "+cfw");
}

/* ---- input dispatchers: replace the stock ones (see cfw/hooks.txt) ---- */

typedef void (*io2_fn)(uint32_t, uint32_t);
typedef void (*io1_fn)(uint32_t);

static int io_event(int list, uint32_t id, uint32_t value)
{
    probe_io(list, id, value);
    return tm_io_event(list, id, value);
}

static void call_list2(const io_list_t *l, uint32_t id, uint32_t v)
{
    for (unsigned i = 0; i < l->count; i++)
        ((io2_fn)l->fn[i])(id, v);
}

void cfw_io_dispatch_a(uint32_t id, uint32_t v)
{
    int r = io_event(0, id, v);
    if (r == TM_PASS)
        call_list2(&g_io_list_a, id, v);
    else if (r == TM_ENC_ONLY)
        relin_io_listener_a(id, v);
}

void cfw_io_dispatch_b(uint32_t id, uint32_t v)
{
    if (io_event(1, id, v) == TM_PASS)
        call_list2(&g_io_list_b, id, v);
}

void cfw_io_dispatch_d(uint32_t id, uint32_t v)
{
    int r = io_event(3, id, v);
    if (r == TM_PASS)
        call_list2(&g_io_list_d, id, v);
    else if (r == TM_ENC_ONLY)
        relin_io_listener_d(id, v);
}

void cfw_io_dispatch_c(uint32_t id)
{
    int r = io_event(2, id, 0);
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

static int enc_event(int list, uint32_t id, uint32_t v, int32_t d)
{
    probe_enc(list, id, v, d);
    return tm_enc_event(list, id, d);
}

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
