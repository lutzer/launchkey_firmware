#include "tm_core.h"

const uint8_t tm_div_ticks[TM_NUM_DIVS] = {3, 4, 6, 8, 12, 16, 24, 48, 96};

static uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

void tm_init(tm_t *tm, uint32_t seed)
{
    tm->rng = seed ? seed : 0x2545F491u;
    tm->reg = (uint16_t)xorshift32(&tm->rng);
    tm->len = 8;
    tm->prob = 96;
    tm->div = 2;           /* 1/16 */
    tm->octave = 3;
    tm->range = 8;
    tm->mask = 0xFF;
}

int tm_step(tm_t *tm)
{
    int in = (tm->reg >> (tm->len - 1)) & 1;
    if (xorshift32(&tm->rng) % 127 >= tm->prob)
        in ^= 1;
    tm->reg = (uint16_t)(tm->reg << 1 | in);
    return in;
}

int tm_scale_size(const uint8_t semis[12])
{
    int n = 0;
    for (int i = 0; i < 12; i++)
        n += semis[i] != 0;
    return n;
}

int tm_playable(const uint8_t semis[12], int scale_on, uint8_t mask, uint8_t out[12])
{
    int n = 0, degree = 0;
    for (int i = 0; i < 12; i++) {
        if (!scale_on) {
            out[n++] = (uint8_t)i;
        } else if (semis[i]) {
            if (degree < TM_PADS && (mask >> degree & 1))
                out[n++] = (uint8_t)i;
            degree++;
        }
    }
    return n;
}

int tm_note(const tm_t *tm, int root, const uint8_t *playable, int count)
{
    if (count == 0)
        return -1;
    int idx = (tm->reg & 0xFF) * tm->range / 256;
    int note = 12 * (tm->octave + idx / count) + root + playable[idx % count];
    while (note > 127)
        note -= 12;
    return note;
}
