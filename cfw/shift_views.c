/* Shift + Scale / Shift + Arp open the settings screens without switching anything.
 *
 * Stock: Shift + Scale (control 0x42) acts like Scale (tap toggles the scale, hold shows its screen),
 * Shift + Arp (0x60) toggles the arp latch. Here both open their screen (press again: close). Shift + Scale
 * switches the scale on (like stock) but never off; Shift + Arp leaves arp on/off and latch alone. On the Arp screen the encoder arrows set the latch
 * (up = on, down = off) and the arrow of the current setting is lit.
 *
 * Hooks (cfw/hooks.txt): the scale and arp press handlers in the central press dispatcher
 * (0x080163E0) and the entry of the arp LED refresh (0x08023778). */
#include <stdint.h>
#include "fw_api.h"

#define CTL_SHIFT_SCALE  0x42
#define CTL_SHIFT_ARP    0x60
#define CTL_ENC_UP       0x25
#define CTL_ENC_DOWN     0x24

#define FEAT_ARP_LATCH   0x58
#define STR_OFF          0x5D
#define STR_ON           0x5E
#define STR_LATCH        0x70
#define POPUP_KEY        0xFF        /* as used by the stock latch popup */
#define WHITE            0xFFFFFF

const char *cfw_str_get(uint32_t id);   /* settings.c (str_get hook) */

void cfw_scale_press(uint32_t id)
{
    if (id != CTL_SHIFT_SCALE || !ui_menu_owns(id, UI_MENU_SCALE)) {
        scale_press(id);
        return;
    }
    g_scale_view.hold_timer = 0;
    g_scale_view.pending_off = 0;
    if (g_scale_view.shown) {
        scale_view_close();
        ui_popup_begin();
    } else {
        scale_view_open();          /* switches the scale on: its settings only apply while it is on */
    }
}

/* Stock arp LED refresh: its first two instructions, then the rest of the original. */
__attribute__((naked)) static void arp_leds_stock(void)
{
    __asm volatile(
        "push  {r3, r4, lr}\n"
        "movw  r0, #0x1094\n"       /* ldr.w r0, =g_arp_view */
        "movt  r0, #0x2000\n"
        "movw  r12, #0x377F\n"      /* continue at 0x0802377E (Thumb) */
        "movt  r12, #0x0802\n"
        "bx    r12\n");
}

/* Hook: entry of the arp LED refresh (also the arp feature listener). */
void cfw_arp_leds(void)
{
    arp_leds_stock();
    if (g_arp_view.shown) {
        int on = feat_get_value(FEAT_ARP_LATCH) != 0;
        arp_led_set(CTL_ENC_UP, on ? WHITE : 0);
        arp_led_set(CTL_ENC_DOWN, on ? 0 : WHITE);
    }
}

void cfw_arp_press(uint32_t id)
{
    if (!ui_menu_owns(id, UI_MENU_ARP)) {
        arp_press(id);
        return;
    }
    if (id == CTL_SHIFT_ARP) {
        g_arp_view.hold_timer = 0;
        g_arp_view.pending_off = 0;
        if (g_arp_view.shown) {
            arp_view_close();
        } else {                    /* arp_view_open (0x0802386C) without enabling the arp */
            ui_close_views();
            g_arp_view.shown = 1;
            ui_view_changed();
        }
        ui_popup_begin();
        return;
    }
    if ((id == CTL_ENC_UP || id == CTL_ENC_DOWN) && g_arp_view.shown) {
        int on = id == CTL_ENC_UP;  /* same popup as the stock Shift + Arp latch toggle */
        feat_set_value_volatile(FEAT_ARP_LATCH, on ? 0x7F : 0);
        arp_popup_title(POPUP_KEY, STR_LATCH);
        popup_field_text(POPUP_KEY, 1, cfw_str_get(on ? STR_ON : STR_OFF));
        cfw_arp_leds();
        return;
    }
    arp_press(id);
}
