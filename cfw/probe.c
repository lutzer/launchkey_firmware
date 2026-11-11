/* Debug reports as SysEx  F0 00 20 29 7D <type> <data...> F7  on both app MIDI ports,
 * decoded on the host by tools/probe_listen.py. Temporary (development builds).
 *
 *   type 1 io      list(0..3 = A..D), id, value lo7, value hi7
 *   type 2 enc     list(0 = pos, 2 = step; hires not reported), id, delta + 64, value lo7, value hi7
 *   type 3 feat    id, value lo7, value hi7
 *   type 4 stack   free bytes lo7, hi7
 *   type 7 flags   128 bytes: g_feat_flags (bit0 defined, bit1 persistent, bit2 14-bit)
 */
#include <stdint.h>
#include "fw_api.h"
#include "probe.h"

uint32_t cfw_stack_free(void);

#define G_FEAT_FLAGS ((const uint8_t *)0x2000E094)

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

void probe_io(int list, uint32_t id, uint32_t value)
{
    uint8_t m[] = {1, (uint8_t)list, (uint8_t)id, (uint8_t)value, (uint8_t)(value >> 7)};
    report(m, sizeof m);
}

void probe_enc(int list, uint32_t id, uint32_t value, int32_t delta)
{
    if (list == 1)
        return;
    uint8_t m[] = {2, (uint8_t)list, (uint8_t)id, (uint8_t)(delta + 64), (uint8_t)value, (uint8_t)(value >> 7)};
    report(m, sizeof m);
}

void probe_stack(void)
{
    uint32_t f = cfw_stack_free();
    uint8_t m[] = {4, (uint8_t)f, (uint8_t)(f >> 7)};
    report(m, sizeof m);
}

void probe_feat(uint32_t id, uint32_t value)
{
    uint8_t m[] = {3, (uint8_t)id, (uint8_t)value, (uint8_t)(value >> 7)};
    report(m, sizeof m);
    probe_stack();
}

void probe_flags(void)
{
    uint8_t m[129];
    m[0] = 7;
    for (int i = 0; i < 128; i++)
        m[1 + i] = G_FEAT_FLAGS[i] & 0x7F;
    report(m, sizeof m);
}
