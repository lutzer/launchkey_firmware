/* Turing Machine mode: entry points used by cfw_init.c (hooks, dispatchers). */
#ifndef TM_MODE_H
#define TM_MODE_H

#include <stdint.h>

void tm_mode_init(void);
void tm_mode_poll(void);
void tm_feat_changed(uint32_t id, uint32_t value);
/* tm_io_event result */
#define TM_PASS      0   /* stock listeners get the event */
#define TM_CONSUME   1   /* only Turing Machine mode */
#define TM_ENC_ONLY  2   /* only the encoder module, so it still produces turn events */
int tm_io_event(int list, uint32_t id, uint32_t value);
int tm_enc_event(int list, uint32_t id, int32_t delta);

#endif
