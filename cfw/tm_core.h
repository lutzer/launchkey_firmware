/* Turing Machine core: shift register + note quantiser. Pure C, no firmware
 * dependencies, so it is unit-tested on the host (cfw/test/test_tm_core.c). */
#ifndef TM_CORE_H
#define TM_CORE_H

#include <stdint.h>

#define TM_LEN_MIN     2
#define TM_LEN_MAX     16
#define TM_PROB_MAX    127   /* 127 = locked, 64 = random, 0 = locked + inverted (period 2L) */
#define TM_OCT_MAX     8     /* octave 4 = 0 shown: -4..+4 */
#define TM_OCT_ZERO    4
#define TM_RANGE_MIN   1
#define TM_RANGE_MAX   48    /* pitch range in semitones above the root (12 = one octave) */
#define TM_NUM_DIVS    11
#define TM_GATE_MIN    5     /* gate length in % of a step; never reaches the next step */
#define TM_GATE_MAX    95
#define TM_GATE_STEP   5
#define TM_VEL_MAX     100   /* velocity depth in % */
#define TM_VEL_STEP    5
#define TM_VEL_BASE    100   /* velocity at depth 0 */
#define TM_VEL_SHIFT   4     /* velocity window at head 0 = register bits 4..10 (pitch uses bits 0..7) */
#define TM_VEL_HEADS   10    /* velocity head positions: window at bits 0..9 (16 - 7 + 1) */
#define TM_NUM_HEADS   8     /* trigger modes, dense -> sparse (see tm_heads) */
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
    uint8_t  heads;    /* index into tm_heads (0 = every step) */
    uint8_t  vel;      /* velocity depth 0..TM_VEL_MAX % */
    uint8_t  vel_head; /* velocity head 0..TM_VEL_HEADS - 1, see tm_vel_shift */
    uint32_t rng;      /* xorshift32 state, never 0 */
} tm_t;

/* Step length in 24 PPQN clock ticks, slow -> fast (higher index = faster):
 * 4/1 2/1 1/1 1/2 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 */
extern const uint16_t tm_div_ticks[TM_NUM_DIVS];
#define TM_DIV_DEFAULT 8       /* 1/16 */

/* Trigger modes: the heads read register bits and combine them. A head at fraction f of the
 * loop reads bit f * len (bit k = bit 0 delayed k steps), so heads stay on different steps for
 * any length and every mode loops with the register. Dense -> sparse:
 *   Every   every step                       100 %
 *   OR 3    0 | 1/3 | 2/3                    ~88 %
 *   OR 2    0 | 1/2                          ~75 %
 *   Bit 1   0 (the module)                   ~50 %
 *   XOR     0 ^ 1/2                          ~50 %, a different rhythm
 *   AND 2   0 & 1/2                          ~25 %
 *   AND 2+  0 & 1/2 & (1/4 | 3/4)            ~19 %
 *   AND 3   0 & 1/3 & 2/3                    ~12 % */
enum { TM_TRIG_EVERY, TM_TRIG_OR, TM_TRIG_AND, TM_TRIG_XOR, TM_TRIG_AND2_OR };
#define TM_HEAD_FRAC   12    /* head positions in twelfths of the loop */
typedef struct {
    uint8_t     op;       /* TM_TRIG_*; AND2_OR: heads 0, 1 AND-ed with (head 2 | head 3) */
    uint8_t     count;
    uint8_t     at[4];    /* head positions in TM_HEAD_FRAC-ths of the loop length */
    const char *name;
} tm_heads_t;
extern const tm_heads_t tm_heads[TM_NUM_HEADS];

void tm_init(tm_t *tm, uint32_t seed);

/* Advance the register one step; returns the bit shifted in. */
int tm_step(tm_t *tm);

/* 1 if the current step plays a note (trigger mode tm->heads). */
int tm_gate(const tm_t *tm);

/* Gate length in clock ticks: gate % of the step, always 1..step-1 (no ties). */
uint16_t tm_gate_ticks(const tm_t *tm);

/* Lowest register bit of the velocity window: (TM_VEL_SHIFT + vel_head) % TM_VEL_HEADS, so head 0
 * reads bits 4..10 (settings saved before the head existed read 0) and every head gives another
 * accent pattern from the same loop. */
int tm_vel_shift(const tm_t *tm);

/* Note velocity 1..127: TM_VEL_BASE blended with the 7-bit register window at tm_vel_shift,
 * vel % of the way (0 = always TM_VEL_BASE, 100 = window only). */
int tm_velocity(const tm_t *tm);

/* Copy the loop (bits 0..len-1) over the bits above it: bit k = bit k - len, the state a locked
 * loop keeps by itself. Pitch (bits 0..7) and velocity (up to bit 15) read beyond short loops, so every
 * edit tiles to be heard from the next step on instead of after the stale copies shifted out. */
void tm_tile(tm_t *tm);

/* Flip register bit i (0 = newest) if it is inside the loop and tile; returns 1 if flipped. */
int tm_flip(tm_t *tm, int i);

/* Undo n locked steps: rotate the loop (bits 0..len-1) back by n, so the register is where
 * the loop started n steps ago (flipped bits stay flipped), and tile. */
void tm_rewind(tm_t *tm, int n);

/* Register bit holding the first step played after tm_rewind(tm, pos) (pos = steps since the
 * loop start): the rewind puts it back at bit len - 1, the next tm_step shifts it into bit 0. */
int tm_start_bit(const tm_t *tm, int pos);

/* Register bit holding step k (0 = the first) of the loop played after tm_rewind(tm, pos):
 * the bits play from bit len - 1 down, so step k sits k bits below the start bit (wrapping). */
int tm_loop_bit(const tm_t *tm, int pos, int k);

/* Number of notes in a scale table entry (= number of usable lower pads). */
int tm_scale_size(const uint8_t semis[12]);

/* MIDI note for the current register value, or -1 for a rest (no note enabled).
 * semis = 12-byte scale table entry (non-zero = note in scale). The value picks a note from
 * 0..range semitones above the root, quantised to the closest enabled note (tie -> lower,
 * inside the range when possible); tm->mask selects the enabled scale notes. scale_on = 0: all 12 semitones, mask ignored. */
int tm_note(const tm_t *tm, int root, const uint8_t semis[12], int scale_on);

#endif
