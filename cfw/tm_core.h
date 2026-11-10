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
#define TM_NUM_DIVS    9
#define TM_PADS        8     /* lower pads = scale notes, upper pads = register bits */

typedef struct {
    uint16_t reg;      /* shift register, bit 0 = newest */
    uint8_t  len;      /* loop length TM_LEN_MIN..TM_LEN_MAX */
    uint8_t  prob;     /* knob 0..TM_PROB_MAX, flip probability = (127 - prob) / 127 */
    uint8_t  div;      /* index into tm_div_ticks */
    uint8_t  octave;   /* base octave 0..TM_OCT_MAX */
    uint8_t  range;    /* TM_RANGE_MIN..TM_RANGE_MAX */
    uint8_t  mask;     /* bit n = n-th scale note enabled (pad n + 1) */
    uint32_t rng;      /* xorshift32 state, never 0 */
} tm_t;

/* Step length in 24 PPQN clock ticks: 1/32 1/16T 1/16 1/8T 1/8 1/4T 1/4 1/2 1/1 */
extern const uint8_t tm_div_ticks[TM_NUM_DIVS];

void tm_init(tm_t *tm, uint32_t seed);

/* Advance the register one step; returns the bit shifted in. */
int tm_step(tm_t *tm);

/* Semitone offsets (0..11, ascending) that may be played. semis = 12-byte scale table entry
 * (non-zero = note in scale). scale_on = 0 -> all 12 semitones, mask ignored. Returns count. */
int tm_playable(const uint8_t semis[12], int scale_on, uint8_t mask, uint8_t out[12]);

/* Number of notes in a scale table entry (= number of usable lower pads). */
int tm_scale_size(const uint8_t semis[12]);

/* MIDI note for the current register value, or -1 for a rest (nothing playable). */
int tm_note(const tm_t *tm, int root, const uint8_t *playable, int count);

#endif
