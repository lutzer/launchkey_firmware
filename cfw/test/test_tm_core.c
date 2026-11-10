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

static void test_scale_notes(void)
{
    uint8_t pl[12]; tm_t tm;
    tm_init(&tm, 7);
    CHECK(tm_scale_size(MAJOR) == 7, "major size");
    CHECK(tm_scale_size(MINOR_PENT) == 5, "pent size");

    int n = tm_playable(MAJOR, 1, 0xFF, pl);
    CHECK(n == 7, "major playable %d", n);
    for (int root = 0; root < 12; root++)
        for (int range = TM_RANGE_MIN; range <= TM_RANGE_MAX; range++)
            for (int v = 0; v < 65536; v += 97) {
                tm.reg = (uint16_t)v; tm.range = (uint8_t)range; tm.octave = 3;
                int note = tm_note(&tm, root, pl, n);
                int rel = note - root - 36;
                CHECK(note >= 36 + root && MAJOR[rel % 12], "note %d not in major (root %d)", note, root);
                CHECK(rel < 12 * ((range - 1) / n + 1), "note %d outside range %d", note, range);
            }

    n = tm_playable(MAJOR, 1, 0x05, pl);          /* pads 1 and 3: root and 3rd */
    CHECK(n == 2 && pl[0] == 0 && pl[1] == 4, "mask 0x05 -> %d notes", n);

    n = tm_playable(MAJOR, 1, 0x00, pl);
    CHECK(n == 0 && tm_note(&tm, 0, pl, n) == -1, "all notes off must rest");

    n = tm_playable(MINOR_PENT, 1, 0xE0, pl);     /* pads 6..8 do not exist in a 5-note scale */
    CHECK(n == 0, "mask bits past the scale size must be ignored (%d)", n);
}

static void test_scale_off(void)
{
    uint8_t pl[12]; tm_t tm; int seen[12] = {0};
    int n = tm_playable(MAJOR, 0, 0x00, pl);
    CHECK(n == 12, "scale off must play all 12 semitones, got %d", n);
    tm_init(&tm, 5);
    tm.range = 12; tm.octave = 4;
    for (int v = 0; v < 256; v++) {
        tm.reg = (uint16_t)v;
        seen[tm_note(&tm, 0, pl, n) % 12]++;
    }
    for (int i = 0; i < 12; i++)
        CHECK(seen[i], "semitone %d never produced with scale off", i);
}

static void test_clamp(void)
{
    uint8_t pl[12]; tm_t tm;
    int n = tm_playable(MAJOR, 0, 0xFF, pl);
    tm_init(&tm, 3);
    tm.octave = TM_OCT_MAX; tm.range = TM_RANGE_MAX; tm.reg = 0xFF;
    int note = tm_note(&tm, 11, pl, n);
    CHECK(note >= 0 && note <= 127, "note %d out of MIDI range", note);
}

int main(void)
{
    test_locked();
    test_inverted();
    test_random();
    test_scale_notes();
    test_scale_off();
    test_clamp();
    printf(failures ? "%d FAILED\n" : "all tm_core tests passed\n", failures);
    return failures != 0;
}
