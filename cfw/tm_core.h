/* Turing Machine core: shift register + note quantiser. Pure C, no firmware
 * dependencies, so it is unit-tested on the host (cfw/test/test_tm_core.c). */
#ifndef TM_CORE_H
#define TM_CORE_H

#include <stdint.h>

#define TM_LEN_MIN     2
#define TM_LEN_MAX     16
#define TM_PROB_MAX    127   /* 127 = locked, 64 = random, 0 = locked + inverted (period 2L) */
#define TM_OCT_MAX     8
#define TM_RANGE_MIN   1
#define TM_RANGE_MAX   36    /* note range in playable-note steps */
#define TM_NUM_DIVS    11
#define TM_GATE_MIN    5     /* gate length in % of a step; never reaches the next step */
#define TM_GATE_MAX    95
#define TM_GATE_STEP   5
#define TM_NUM_HEADS   7     /* trigger head spacing: Full 1/2 1/3 1/4 1/6 1/8 1/16 */
#define TM_PADS        8     /* lower pads = scale notes, upper pads = register bits */

typedef struct {
    uint16_t reg;      /* shift register, bit 0 = newest */
    uint8_t  len;      /* loop length TM_LEN_MIN..TM_LEN_MAX */
    uint8_t  prob;     /* knob 0..TM_PROB_MAX, flip probability = (127 - prob) / 127 */
    uint8_t  div;      /* index into tm_div_ticks (higher = faster) */
    uint8_t  octave;   /* base octave 0..TM_OCT_MAX */
    uint8_t  range;    /* TM_RANGE_MIN..TM_RANGE_MAX */
    uint8_t  mask;     /* bit n = n-th scale note enabled (pad n + 1) */
    uint8_t  gate;     /* gate length in % of a step, TM_GATE_MIN..TM_GATE_MAX */
    uint8_t  heads;    /* index into tm_head_spacing (0 = Full) */
    uint32_t rng;      /* xorshift32 state, never 0 */
} tm_t;

/* Step length in 24 PPQN clock ticks, slow -> fast (higher index = faster):
 * 4/1 2/1 1/1 1/2 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 */
extern const uint16_t tm_div_ticks[TM_NUM_DIVS];
#define TM_DIV_DEFAULT 8       /* 1/16 */

/* Trigger heads sit at register positions 0, s, 2s, ... below the loop length. */
extern const uint8_t tm_head_spacing[TM_NUM_HEADS];

void tm_init(tm_t *tm, uint32_t seed);

/* Advance the register one step; returns the bit shifted in. */
int tm_step(tm_t *tm);

/* A step plays a note when any trigger head (positions 0, s, 2s, ... < len; bit 0 = newest)
 * reads a 1. Full (s = 1) checks the whole loop, 1/16 only the first position. */
int tm_gate(const tm_t *tm);

/* Gate length in clock ticks: gate % of the step, always 1..step-1 (no ties). */
uint16_t tm_gate_ticks(const tm_t *tm);

/* Flip register bit i (0 = newest) if it is inside the loop; returns 1 if flipped. */
int tm_flip(tm_t *tm, int i);

/* Undo n locked steps: rotate the loop (bits 0..len-1) back by n, so the register is where
 * the loop started n steps ago (flipped bits stay flipped). */
void tm_rewind(tm_t *tm, int n);

/* Number of notes in a scale table entry (= number of usable lower pads). */
int tm_scale_size(const uint8_t semis[12]);

/* MIDI note for the current register value, or -1 for a rest (no note enabled).
 * semis = 12-byte scale table entry (non-zero = note in scale). The value picks a note from
 * the full scale (range counts all scale notes); a note disabled in tm->mask is replaced by
 * the closest enabled note in pitch (tie -> lower). scale_on = 0: all 12 semitones, mask ignored. */
int tm_note(const tm_t *tm, int root, const uint8_t semis[12], int scale_on);

#endif
