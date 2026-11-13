/* Arp screen additions; Shift + Arp and Shift + Scale stay stock.
 *
 * - On the Arp screen the encoder arrows set the latch (up = on, down = off) and the arrow of the current
 *   setting is lit.
 * - A long press of Arp never switches the arp off. Stock only cancels the pending switch-off when one of its
 *   hold timers expires; with the screen already shown and the pads already on the arp pattern layout
 *   neither timer runs, so the release switched the arp off. Here the hold timer always runs (its expiry
 *   clears pending_off and shows the screen).
 * - The arp follows the transport (Play button in standalone, MIDI Start / Continue / Stop, see
 *   tm_playing): stopped = paused, the sounding note is released; held / latched keys play on resume.
 *
 * Hooks (cfw/hooks.txt): the arp press handler in the central press dispatcher (0x080163E0), the entry
 * of the arp LED refresh (0x08023778) and the arp engine call in the main loop (0x08016746). */
#include <stdint.h>
#include "fw_api.h"
#include "tm_mode.h"

#define CTL_ARP          0x1E
#define CTL_ENC_UP       0x25
#define CTL_ENC_DOWN     0x24
#define HOLD_MS          0x14D       /* stock hold time */

#define FEAT_ARP_LATCH   0x58
#define STR_OFF          0x5D
#define STR_ON           0x5E
#define STR_LATCH        0x70
#define POPUP_KEY        0xFF        /* as used by the stock latch popup */
#define WHITE            0xFFFFFF

const char *cfw_str_get(uint32_t id);   /* settings.c (str_get hook) */

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
    if ((id == CTL_ENC_UP || id == CTL_ENC_DOWN) && g_arp_view.shown) {
        int on = id == CTL_ENC_UP;  /* same popup as the stock Shift + Arp latch toggle */
        feat_set_value_volatile(FEAT_ARP_LATCH, on ? 0x7F : 0);
        arp_popup_title(POPUP_KEY, STR_LATCH);
        popup_field_text(POPUP_KEY, 1, cfw_str_get(on ? STR_ON : STR_OFF));
        cfw_arp_leds();
        return;
    }
    arp_press(id);
    if (id == CTL_ARP && g_arp_view.pending_off && !g_arp_view.hold_timer)
        g_arp_view.hold_timer = HOLD_MS;
}

/* Hook: the arp engine call in the main loop. While stopped the engine runs with the arp switched off
 * for the call: it keeps its time references current (no catch-up burst on resume) and returns before
 * scheduling notes. */
void cfw_arp_poll(void)
{
    static uint8_t paused;
    if (tm_playing()) {
        paused = 0;
        arp_poll();
        return;
    }
    if (!paused) {
        paused = 1;
        arp_notes_flush(&g_arp_notes);  /* note-off for the sounding arp note */
    }
    uint8_t on = g_arp_state.enabled;
    g_arp_state.enabled = 0;
    arp_poll();
    g_arp_state.enabled = on;
}
