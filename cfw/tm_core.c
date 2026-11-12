#include "tm_core.h"

const tm_heads_t tm_heads[TM_NUM_HEADS] = {
    {TM_TRIG_EVERY,   0, {0},           "Every"},
    {TM_TRIG_OR,      3, {0, 4, 8},     "OR 3"},
    {TM_TRIG_OR,      2, {0, 6},        "OR 2"},
    {TM_TRIG_OR,      1, {0},           "Bit 1"},
    {TM_TRIG_XOR,     2, {0, 6},        "XOR"},
    {TM_TRIG_AND,     2, {0, 6},        "AND 2"},
    {TM_TRIG_AND2_OR, 4, {0, 6, 3, 9},  "AND 2+"},
    {TM_TRIG_AND,     3, {0, 4, 8},     "AND 3"},
};

const uint16_t tm_div_ticks[TM_NUM_DIVS] = {384, 192, 96, 48, 24, 16, 12, 8, 6, 4, 3};

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
    tm->div = TM_DIV_DEFAULT;
    tm->octave = TM_OCT_ZERO;
    tm->range = 12;
    tm->mask = 0xFF;
    tm->gate = 50;
    tm->heads = 0;          /* every step */
    tm->vel = 0;
    tm->vel_head = 0;
}

int tm_step(tm_t *tm)
{
    int in = (tm->reg >> (tm->len - 1)) & 1;
    if (xorshift32(&tm->rng) % 127 >= tm->prob)
        in ^= 1;
    tm->reg = (uint16_t)(tm->reg << 1 | in);
    return in;
}

/* Bit read by a head at fraction at/TM_HEAD_FRAC of the loop; heads after the first never
 * share bit 0 (loops of length 2 still get two different steps). */
static int head_bit(const tm_t *tm, int at)
{
    int pos = at * tm->len / TM_HEAD_FRAC;
    if (at && pos == 0)
        pos = 1;
    return tm->reg >> pos & 1;
}

int tm_gate(const tm_t *tm)
{
    const tm_heads_t *h = &tm_heads[tm->heads];
    int any = 0, all = 1, odd = 0;
    for (int i = 0; i < h->count; i++) {
        int b = head_bit(tm, h->at[i]);
        any |= b;
        all &= b;
        odd ^= b;
    }
    switch (h->op) {
    case TM_TRIG_OR:      return any;
    case TM_TRIG_AND:     return all;
    case TM_TRIG_XOR:     return odd;
    case TM_TRIG_AND2_OR: return head_bit(tm, h->at[0]) && head_bit(tm, h->at[1]) &&
                                 (head_bit(tm, h->at[2]) || head_bit(tm, h->at[3]));
    default:              return 1;
    }
}

uint16_t tm_gate_ticks(const tm_t *tm)
{
    uint32_t step = tm_div_ticks[tm->div];
    uint32_t t = step * tm->gate / 100;
    if (t < 1)
        t = 1;
    if (t > step - 1)
        t = step - 1;
    return (uint16_t)t;
}

int tm_vel_shift(const tm_t *tm)
{
    return (TM_VEL_SHIFT + tm->vel_head) % TM_VEL_HEADS;
}

int tm_velocity(const tm_t *tm)
{
    int window = 1 + (tm->reg >> tm_vel_shift(tm) & 0x7F) * 126 / 127;     /* 1..127 */
    int v = (TM_VEL_BASE * (TM_VEL_MAX - tm->vel) + window * tm->vel) / TM_VEL_MAX;
    return v < 1 ? 1 : v > 127 ? 127 : v;
}

void tm_tile(tm_t *tm)
{
    uint32_t loop = tm->reg & ((1u << tm->len) - 1);
    uint32_t r = loop;
    for (int k = tm->len; k < 16; k += tm->len)
        r |= loop << k;
    tm->reg = (uint16_t)r;
}

void tm_rewind(tm_t *tm, int n)
{
    n %= tm->len;
    uint32_t mask = (1u << tm->len) - 1;
    uint32_t loop = tm->reg & mask;
    if (n)
        loop = (loop >> n | loop << (tm->len - n)) & mask;
    tm->reg = (uint16_t)loop;
    tm_tile(tm);
}

int tm_flip(tm_t *tm, int i)
{
    if (i < 0 || i >= tm->len)
        return 0;
    tm->reg ^= (uint16_t)(1u << i);
    tm_tile(tm);
    return 1;
}

int tm_start_bit(const tm_t *tm, int pos)
{
    return ((tm->len - 1 + pos) % tm->len + tm->len) % tm->len;
}

int tm_loop_bit(const tm_t *tm, int pos, int k)
{
    return ((tm_start_bit(tm, pos) - k) % tm->len + tm->len) % tm->len;
}

int tm_scale_size(const uint8_t semis[12])
{
    int n = 0;
    for (int i = 0; i < 12; i++)
        n += semis[i] != 0;
    return n;
}

/* Note for the current register value. The value picks a pitch 0..range semitones above
 * the root, which is quantised to the closest enabled scale note (tie -> lower), staying
 * inside 0..range when such a note exists. */
int tm_note(const tm_t *tm, int root, const uint8_t semis[12], int scale_on)
{
    uint16_t enabled = 0;       /* bit = semitone above the root */
    int n = 0;
    for (int i = 0; i < 12; i++) {
        if (scale_on && !semis[i])
            continue;
        if (!scale_on || (n < TM_PADS && (tm->mask >> n & 1)))
            enabled |= (uint16_t)(1u << i);
        n++;
    }
    if (!enabled)
        return -1;

    int target = (tm->reg & 0xFF) * (tm->range + 1) / 256;     /* 0..range */
    int best = 0, best_cost = 100000;
    for (int p = -12; p <= tm->range + 12; p++) {
        if (!(enabled >> ((p + 12) % 12) & 1))
            continue;
        int cost = (p > target ? p - target : target - p) + (p < 0 || p > tm->range ? 1000 : 0);
        if (cost < best_cost) {     /* ascending p: a tie keeps the lower note */
            best = p;
            best_cost = cost;
        }
    }
    int note = 12 * tm->octave + root + best;
    while (note > 127)
        note -= 12;
    while (note < 0)
        note += 12;
    return note;
}
