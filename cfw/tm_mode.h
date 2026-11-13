/* Turing Machine mode: entry points used by cfw_init.c (hooks, dispatchers). */
#ifndef TM_MODE_H
#define TM_MODE_H

#include <stdint.h>

#define TM_PAD_MODE      8      /* feature param 0x1D (pad mode) value of pad Custom 4 = this mode */
#define TM_CHANNEL_PARAM 0x61   /* feature param: Turing Machine MIDI channel 0..15 (Settings) */

void tm_mode_init(void);
void tm_mode_poll(void);
void tm_feat_changed(uint32_t id, uint32_t value);

/* io listener lists (tm_io_event list argument) */
#define TM_IO_PRESS    0        /* A: press (value = velocity; encoders: touch) */
#define TM_IO_B        1        /* B: pressure / value stream */
#define TM_IO_RELEASE  2        /* C: release */
#define TM_IO_D        3        /* D: pressure / value stream */
/* tm_io_event result */
#define TM_PASS      0   /* stock listeners get the event */
#define TM_CONSUME   1   /* only Turing Machine mode */
#define TM_ENC_ONLY  2   /* only the encoder module, so it still produces turn events */
int tm_io_event(int list, uint32_t id, uint32_t value);

/* relative-input listener lists (tm_enc_event list argument) */
#define TM_REL_POS     0        /* every tick */
#define TM_REL_HIRES   1
#define TM_REL_STEP    2        /* per detent */
/* returns 1 if consumed */
int tm_enc_event(int list, uint32_t id, int32_t delta);

/* received MIDI transport; may run outside the main loop, so it only records the event */
#define TM_START     1
#define TM_CONTINUE  2
#define TM_STOP      3
void tm_transport(int event);
/* transport state (Play button, MIDI Start..Stop) */
int tm_playing(void);
/* Play button (main loop, standalone only): start from the first step / stop */
void tm_play_button(int start);
/* Shift + Play: arm a one-shot run. The next start (Play, MIDI Start / Continue) plays TM_RUN_STEPS
 * steps of TM_RUN_STEP_TICKS clock ticks (1/16 at 24 PPQN), then stops the transport. */
#define TM_RUN_STEPS       8
#define TM_RUN_STEP_TICKS  6
void tm_arm(int on);
int tm_armed(void);
/* 1 once after a run stopped the transport by itself */
int tm_run_done(void);
/* two-line popup (title, value); redraws the mode's button LEDs after the stock popup start */
void tm_popup(const char *title, const char *value);
/* after the Shift overlay drew its LEDs: colour the pad that selects the mode */
void tm_shift_leds(void);
/* mode popup content (title + encoder labels), without starting a popup */
void tm_overview_text(void);

#endif
