/* Debug reports over SysEx (see probe.c). */
#ifndef PROBE_H
#define PROBE_H

#include <stdint.h>

void probe_io(int list, uint32_t id, uint32_t value);
void probe_enc(int list, uint32_t id, uint32_t value, int32_t delta);
void probe_feat(uint32_t id, uint32_t value);
void probe_stack(void);
void probe_flags(void);

#endif
