/* Stock firmware (fw 1563) API used by cfw code. Addresses: cfw/fw_api.ld. */
#ifndef FW_API_H
#define FW_API_H

#include <stdint.h>

/* Feature parameter value as passed to confsys_{get,set}_param. flags on set:
 * bit0 = definition (first one wins, no listeners), bit1 = persistent. */
typedef struct {
    uint32_t value;
    uint32_t flags;
} fw_param_t;

#define FW_GROUP_FEATURE 0xD0

/* group 0xD0 = feature params; ids: 0x4A scale on/off, 0x3E scale type, 0x3D root, 0x3C scale mode */
void confsys_get_param(uint32_t group, uint32_t id, fw_param_t *out);
void confsys_set_param(uint32_t group, uint32_t id, const fw_param_t *in, uint32_t r3);

/* listener(id, value) called after a feature param changed */
void feat_add_listener(void (*listener)(uint32_t id, uint32_t value));

void feat_scale_init(void);

/* Appends "a.b.c.d" from ver[4] to buf at pos (bounded by size); returns the new pos. */
uint32_t fmt_version(char *buf, uint32_t pos, uint32_t size, const uint32_t *ver);

/* Appends str to the NUL-terminated buf (bounded by size, always terminated); returns the new length. */
uint32_t str_append(char *buf, uint32_t pos, uint32_t size, const char *str);

/* 30 x 12 bytes, index = scale type (1..30); non-zero = semitone in scale */
extern const uint8_t g_scale_intervals[][12];

#endif
