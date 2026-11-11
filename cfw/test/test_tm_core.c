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
    CHECK(tm_flip(&tm, 0) && tm.reg == 0x0001, "flip bit 0");
    CHECK(tm_flip(&tm, 3) && tm.reg == 0x0009, "flip bit 3");
    CHECK(!tm_flip(&tm, 4) && tm.reg == 0x0009, "bit 4 is outside length 4");
    CHECK(tm_flip(&tm, 0) && tm.reg == 0x0008, "flip back");
    for (int i = 0; i + 1 < TM_NUM_DIVS; i++)
        CHECK(tm_div_ticks[i] > tm_div_ticks[i + 1], "rates must get faster with the index");
    CHECK(tm_div_ticks[TM_DIV_DEFAULT] == 6, "default rate is 1/16");
}

static int heads_index(int spacing)
{
    for (int i = 0; i < TM_NUM_HEADS; i++)
        if (tm_head_spacing[i] == spacing)
            return i;
    return -1;
}

static void test_heads(void)
{
    tm_t tm;
    tm_init(&tm, 4);
    CHECK(tm.heads == 0 && tm_head_spacing[0] == 1, "default is Full");
    tm.len = 8;
    tm.heads = 0;  tm.reg = 0x0080; CHECK(tm_gate(&tm), "Full: a 1 anywhere in the loop gates");
    tm.reg = 0x0100;                CHECK(!tm_gate(&tm), "Full: bits beyond the loop do not count");
    tm.heads = (uint8_t)heads_index(2);
    tm.reg = 0x0004; CHECK(tm_gate(&tm), "1/2: position 2 is a head");
    tm.reg = 0x0002; CHECK(!tm_gate(&tm), "1/2: position 1 is not a head");
    tm.heads = (uint8_t)heads_index(3);
    tm.reg = 0x0040; CHECK(tm_gate(&tm), "1/3: position 6 is a head");
    tm.reg = 0x0020; CHECK(!tm_gate(&tm), "1/3: position 5 is not a head");
    tm.heads = (uint8_t)heads_index(16);
    tm.len = 16;
    tm.reg = 0x8000; CHECK(!tm_gate(&tm), "1/16: only the first position");
    tm.reg = 0x0001; CHECK(tm_gate(&tm), "1/16: first position gates");
    tm.heads = (uint8_t)heads_index(4);
    tm.len = 3;                     /* shorter than the spacing: only the first bit */
    tm.reg = 0x0006; CHECK(!tm_gate(&tm), "short loop: only bit 0 is a head");
    tm.reg = 0x0001; CHECK(tm_gate(&tm), "short loop: bit 0 gates");
}

static void test_gate(void)
{
    tm_t tm; int gated = 0, ones = 0;
    tm_init(&tm, 77);
    tm.len = 16; tm.prob = 64;
    tm.heads = (uint8_t)heads_index(16);    /* single head = first position */
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
