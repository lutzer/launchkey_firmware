/* cfw entry points called from hooks in the stock firmware (see cfw/hooks.txt). */
#include <stdint.h>
#include "fw_api.h"

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
}

/* Hook: replaces `bl fmt_version` in ui_show_boot_screen (0x08016E42). Visible proof that cave code runs. */
uint32_t cfw_boot_version(char *buf, uint32_t pos, uint32_t size, const uint32_t *ver)
{
    pos = fmt_version(buf, pos, size, ver);
    return str_append(buf, pos, size, "+cfw");
}
