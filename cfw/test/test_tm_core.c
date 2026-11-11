/* Host unit tests for tm_core (make cfw-test). */
#include <stdio.h>
#include <stdlib.h>
#include "../tm_core.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                  printf(__VA_ARGS__); printf("\n"); } } while (0)

/* entries of g_scale_intervals @0x08031DE0 */
static const uint8_t MAJOR[12]     = {1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1};
static const uint8_t MINOR_PENT[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0};

static void run_bits(tm_t *tm, int *bits, int n)
{
    for (int i = 0; i < n; i++)
        bits[i] = tm_step(tm);
}

static void test_locked(void)
{
    for (int len = TM_LEN_MIN; len <= TM_LEN_MAX; len++) {
        tm_t tm; int b[200];
        tm_init(&tm, 1234 + len);
        tm.len = len; tm.prob = TM_PROB_MAX;
        run_bits(&tm, b, 200);
        for (int i = 0; i + len < 200; i++)
            CHECK(b[i] == b[i + len], "locked len %d: bit %d != bit %d", len, i, i + len);
    }
}

static void test_inverted(void)
{
    for (int len = TM_LEN_MIN; len <= TM_LEN_MAX; len++) {
        tm_t tm; int b[200];
        tm_init(&tm, 99 + len);
        tm.len = len; tm.prob = 0;
        run_bits(&tm, b, 200);
        for (int i = 0; i + len < 200; i++)
            CHECK(b[i + len] == !b[i], "inverted len %d: bit %d not inverted", len, i + len);
        for (int i = 0; i + 2 * len < 200; i++)
            CHECK(b[i] == b[i + 2 * len], "inverted len %d: period is not 2L at %d", len, i);
    }
}

static void test_random(void)
{
    tm_t tm; int flips = 0, n = 20000;
    tm_init(&tm, 42);
    tm.len = 16; tm.prob = 64;
    for (int i = 0; i < n; i++) {
        int expect = tm.reg >> (tm.len - 1) & 1;
        flips += tm_step(&tm) != expect;
    }
    CHECK(flips > n * 45 / 100 && flips < n * 55 / 100, "prob 64: %d flips of %d, expected ~50%%", flips, n);
}

static int note_at(tm_t *tm, int v, int root, const uint8_t *scale, int scale_on)
{
    tm->reg = (uint16_t)v;
    return tm_note(tm, root, scale, scale_on);
}

static void test_scale_notes(void)
{
    tm_t tm;
    tm_init(&tm, 7);
    CHECK(tm_scale_size(MAJOR) == 7, "major size");
    CHECK(tm_scale_size(MINOR_PENT) == 5, "pent size");

    tm.mask = 0xFF;                               /* all notes on: plain quantiser */
    for (int root = 0; root < 12; root++)
        for (int range = TM_RANGE_MIN; range <= TM_RANGE_MAX; range++)
            for (int v = 0; v < 65536; v += 97) {
                tm.range = (uint8_t)range; tm.octave = 3;
                int note = note_at(&tm, v, root, MAJOR, 1);
                int rel = note - root - 36;
                CHECK(note >= 36 + root && MAJOR[rel % 12], "note %d not in major (root %d)", note, root);
                CHECK(rel < 12 * ((range - 1) / 7 + 1), "note %d outside range %d", note, range);
            }

    tm.mask = 0x05;                               /* pads 1 and 3: root and 3rd -> only C and E */
    tm.range = 21; tm.octave = 0;
    for (int v = 0; v < 256; v++) {
        int pc = note_at(&tm, v, 0, MAJOR, 1) % 12;
        CHECK(pc == 0 || pc == 4, "mask 0x05: v %d gave pitch class %d", v, pc);
    }

    tm.mask = 0x00;
    CHECK(note_at(&tm, 123, 0, MAJOR, 1) == -1, "all notes off must rest");
    tm.mask = 0xE0;                               /* pads 6..8 do not exist in a 5-note scale */
    CHECK(note_at(&tm, 123, 0, MINOR_PENT, 1) == -1, "mask bits past the scale size must be ignored");
}

/* A disabled note is replaced by the closest enabled one (tie -> lower), never skipped. */
static void test_closest(void)
{
    tm_t tm;
    tm_init(&tm, 9);
    tm.octave = 0; tm.range = 7;                  /* idx = v * 7 / 256 -> one octave of C major */
    tm.mask = 0x11;                               /* C (pad 1) and G (pad 5) */
    CHECK(note_at(&tm, 0, 0, MAJOR, 1) == 0, "C stays C");
    CHECK(note_at(&tm, 40, 0, MAJOR, 1) == 0, "D (2) -> C (dist 2) not G (dist 5)");
    CHECK(note_at(&tm, 80, 0, MAJOR, 1) == 7, "E (4) -> G (dist 3) not C (dist 4)");
    CHECK(note_at(&tm, 115, 0, MAJOR, 1) == 7, "F (5) -> G");
    CHECK(note_at(&tm, 220, 0, MAJOR, 1) == 12, "B (11) -> C an octave up (dist 1)");
    tm.mask = 0x05;                               /* C and E */
    CHECK(note_at(&tm, 40, 0, MAJOR, 1) == 0, "D is between C and E: tie -> lower (C)");
    tm.mask = 0x01;                               /* root only: every step is a C, none skipped */
    tm.range = 21;
    for (int v = 0; v < 256; v++)
        CHECK(note_at(&tm, v, 0, MAJOR, 1) % 12 == 0, "root-only mask must always give C");
}

static void test_scale_off(void)
{
    tm_t tm; int seen[12] = {0};
    tm_init(&tm, 5);
    tm.mask = 0x00;                               /* ignored with Scale off */
    tm.range = 12; tm.octave = 4;
    for (int v = 0; v < 256; v++)
        seen[note_at(&tm, v, 0, MAJOR, 0) % 12]++;
    for (int i = 0; i < 12; i++)
        CHECK(seen[i], "semitone %d never produced with scale off", i);
}

static void test_clamp(void)
{
    tm_t tm;
    tm_init(&tm, 3);
    tm.octave = TM_OCT_MAX; tm.range = TM_RANGE_MAX; tm.mask = 0x40;
    for (int v = 0; v < 256; v++) {
        int note = note_at(&tm, v, 11, MAJOR, 1);
        CHECK(note >= 0 && note <= 127, "note %d out of MIDI range", note);
    }
}

static void test_flip(void)
{
    tm_t tm;
    tm_init(&tm, 11);
    tm.len = 4; tm.reg = 0x0000;
    CHECK(tm_flip(&tm, 0) && tm.reg == 0x1111, "flip bit 0 (tiled over the register)");
    CHECK(tm_flip(&tm, 3) && tm.reg == 0x9999, "flip bit 3");
    CHECK(!tm_flip(&tm, 4) && tm.reg == 0x9999, "bit 4 is outside length 4");
    CHECK(tm_flip(&tm, 0) && tm.reg == 0x8888, "flip back");
    tm.len = 3; tm.reg = 0xFFF8;
    CHECK(tm_flip(&tm, 1) && tm.reg == 0x2492, "len 3: copies at 1 + 3k");
    for (int i = 0; i + 1 < TM_NUM_DIVS; i++)
        CHECK(tm_div_ticks[i] > tm_div_ticks[i + 1], "rates must get faster with the index");
    CHECK(tm_div_ticks[TM_DIV_DEFAULT] == 6, "default rate is 1/16");
}

/* After a flip the locked loop repeats exactly from the next step on: pitch (bits 0..7) and
 * velocity (bits 4..10) must not wait for stale copies above the loop to shift out. */
static void test_flip_instant(void)
{
    for (int len = TM_LEN_MIN; len <= TM_LEN_MAX; len++)
        for (int bit = 0; bit < len; bit++) {
            tm_t tm;
            tm_init(&tm, 900 + len * 16 + bit);
            tm.len = len; tm.prob = TM_PROB_MAX; tm.range = TM_RANGE_MAX; tm.vel = TM_VEL_MAX;
            for (int i = 0; i < 37; i++)
                tm_step(&tm);
            tm_flip(&tm, bit);
            int note[2 * TM_LEN_MAX], vel[2 * TM_LEN_MAX];
            for (int i = 0; i < 2 * len; i++) {
                tm_step(&tm);
                note[i] = tm_note(&tm, 0, MAJOR, 1);
                vel[i] = tm_velocity(&tm);
            }
            for (int i = 0; i < len; i++) {
                CHECK(note[i] == note[i + len], "len %d flip %d: note of step %d changes later", len, bit, i);
                CHECK(vel[i] == vel[i + len], "len %d flip %d: velocity of step %d changes later", len, bit, i);
            }
        }
}

/* The marked bit is the one played first after a rewind: a lone set bit there must reach bit 0
 * after tm_rewind(pos) + one step. */
static void test_start_bit(void)
{
    for (int len = TM_LEN_MIN; len <= TM_LEN_MAX; len++)
        for (int pos = 0; pos < len; pos++) {
            tm_t tm;
            tm_init(&tm, 1);
            tm.len = len; tm.prob = TM_PROB_MAX;
            int b = tm_start_bit(&tm, pos);
            CHECK(b >= 0 && b < len, "len %d pos %d: start bit %d outside the loop", len, pos, b);
            tm.reg = (uint16_t)(1u << b);
            tm_rewind(&tm, pos);
            tm_step(&tm);
            CHECK((tm.reg & 1) && !(tm.reg & ((1u << len) - 2)), "len %d pos %d: bit %d is not played first", len, pos, b);
        }
    tm_t tm;
    tm_init(&tm, 1);
    tm.len = 5;
    CHECK(tm_start_bit(&tm, -3) == 1, "negative pos wraps");
}

static void test_rewind(void)
{
    for (int len = TM_LEN_MIN; len <= TM_LEN_MAX; len++)
        for (int n = 0; n < 40; n++) {
            tm_t tm;
            tm_init(&tm, 500 + len * 40 + n);
            tm.len = len; tm.prob = TM_PROB_MAX;
            uint32_t mask = (1u << len) - 1;
            uint16_t start = tm.reg;
            for (int i = 0; i < n; i++)
                tm_step(&tm);
            tm_rewind(&tm, n);
            CHECK((tm.reg & mask) == (start & mask), "rewind len %d after %d steps", len, n);
            for (int k = len; k < 16; k++)
                CHECK((tm.reg >> k & 1) == (tm.reg >> (k - len) & 1), "rewind len %d: bit %d not tiled", len, k);
        }
}

static void test_velocity(void)
{
    tm_t tm; int lo = 127, hi = 0;
    tm_init(&tm, 21);
    for (int v = 0; v < 65536; v += 13) {
        tm.reg = (uint16_t)v;
        tm.vel = 0;
        CHECK(tm_velocity(&tm) == TM_VEL_BASE, "depth 0 must give the base velocity");
        for (int d = 0; d <= TM_VEL_MAX; d += TM_VEL_STEP) {
            tm.vel = (uint8_t)d;
            int vel = tm_velocity(&tm);
            CHECK(vel >= 1 && vel <= 127, "velocity %d out of range", vel);
        }
        tm.vel = TM_VEL_MAX;
        int vel = tm_velocity(&tm);
        lo = vel < lo ? vel : lo; hi = vel > hi ? vel : hi;
    }
    CHECK(lo == 1 && hi == 127, "full depth must span 1..127 (got %d..%d)", lo, hi);
    tm.vel = TM_VEL_MAX;
    tm.reg = 0x000F;                /* bits below the window do not matter */
    CHECK(tm_velocity(&tm) == 1, "window 0 -> velocity 1");
    tm.reg = 0x07F0;
    CHECK(tm_velocity(&tm) == 127, "window 127 -> velocity 127");
}

static void test_heads(void)
{
    tm_t tm;
    tm_init(&tm, 4);
    CHECK(tm.heads == 0 && tm_heads[0].op == TM_TRIG_EVERY, "default is every step");
    tm.reg = 0x0000; CHECK(tm_gate(&tm), "Every: plays with all bits 0");
    /* density over a long random run must fall from dense to sparse */
    int prev = 100000;
    tm.len = 16; tm.prob = 64;
    for (int m = 0; m < TM_NUM_HEADS; m++) {
        int n = 0;
        tm.heads = (uint8_t)m;
        for (int i = 0; i < 20000; i++) {
            tm_step(&tm);
            n += tm_gate(&tm);
        }
        CHECK(n <= prev + 600, "mode %s (%d gates) denser than the one before (%d)", tm_heads[m].name, n, prev);
        prev = n;
    }
    tm.len = 16;                    /* heads: 1/2 -> bit 8, 1/3 -> 5, 2/3 -> 10, 1/4 -> 4, 3/4 -> 12 */
    tm.heads = 3;                   /* Bit 1 = the module */
    tm.reg = 0x0001; CHECK(tm_gate(&tm), "Bit 1: bit 0 set");
    tm.reg = 0xFFFE; CHECK(!tm_gate(&tm), "Bit 1: only bit 0 counts");
    tm.heads = 7;                   /* AND 3 */
    tm.reg = 0x0421; CHECK(tm_gate(&tm), "AND 3: b0 b5 b10 set");
    tm.reg = 0x0021; CHECK(!tm_gate(&tm), "AND 3: b10 missing");
    tm.heads = 6;                   /* AND 2+ */
    tm.reg = 0x0111; CHECK(tm_gate(&tm), "AND 2+: b0 b8 and b4");
    tm.reg = 0x1101; CHECK(tm_gate(&tm), "AND 2+: b0 b8 and b12");
    tm.reg = 0x0101; CHECK(!tm_gate(&tm), "AND 2+: neither b4 nor b12");
    tm.heads = 4;                   /* XOR */
    tm.reg = 0x0100; CHECK(tm_gate(&tm), "XOR: one of two");
    tm.reg = 0x0101; CHECK(!tm_gate(&tm), "XOR: both");
    /* short loops: the heads must stay on different steps */
    tm.len = 3; tm.reg = 0x0002;    /* XOR heads at bits 0 and 1 */
    CHECK(tm_gate(&tm), "XOR len 3: second head at bit 1");
    tm.len = 2; tm.reg = 0x0001;
    CHECK(tm_gate(&tm), "XOR len 2: heads at bits 0 and 1");
    tm.heads = 5; tm.reg = 0x0001;  /* AND 2, len 2 */
    CHECK(!tm_gate(&tm), "AND 2 len 2: bit 1 must count too");
}

static void test_gate(void)
{
    tm_t tm; int gated = 0, ones = 0;
    tm_init(&tm, 77);
    tm.len = 16; tm.prob = 64;
    tm.heads = 3;                           /* Bit 1: single head = first position */
    for (int i = 0; i < 5000; i++) {
        tm_step(&tm);
        ones += tm.reg & 1;
        gated += tm_gate(&tm);
    }
    CHECK(gated == ones && gated > 0, "gates must follow bit 0 (%d gates, %d ones)", gated, ones);
    for (int d = 0; d < TM_NUM_DIVS; d++)
        for (int g = TM_GATE_MIN; g <= TM_GATE_MAX; g += TM_GATE_STEP) {
            tm.div = (uint8_t)d; tm.gate = (uint8_t)g;
            int t = tm_gate_ticks(&tm);
            CHECK(t >= 1 && t < tm_div_ticks[d], "gate %d%% at div %d = %d ticks, must be 1..step-1", g, d, t);
        }
    tm.div = TM_NUM_DIVS - 1;                       /* 1/32 = 3 ticks */
    tm.gate = 5;  CHECK(tm_gate_ticks(&tm) == 1, "1/32 + 5%%");
    tm.gate = 95; CHECK(tm_gate_ticks(&tm) == 2, "1/32 + 95%%");
    tm.div = TM_DIV_DEFAULT; tm.gate = 50;
    CHECK(tm_gate_ticks(&tm) == 3, "1/16 + 50%% = 3 ticks");
}

int main(void)
{
    test_flip();
    test_flip_instant();
    test_start_bit();
    test_rewind();
    test_velocity();
    test_gate();
    test_heads();
    test_locked();
    test_inverted();
    test_random();
    test_scale_notes();
    test_closest();
    test_scale_off();
    test_clamp();
    printf(failures ? "%d FAILED\n" : "all tm_core tests passed\n", failures);
    return failures != 0;
}
