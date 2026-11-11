/* Settings menu: an extra "Turing Channel" item (feature param TM_CHANNEL_PARAM).
 *
 * The menu reads its items through settings_entry(i) (0x0802B44C): 12-byte entries in flash
 * (table 0x08031AB0 for >= 49 keys, +0xF0 for the Minis). On the Minis items 0..15 sit on the
 * pads and settings_count_nav (0x0802B428) = 17 lets the arrows reach item 16 too ("Power On
 * Modes"); the extra item is number 17, reached with the down arrow. Item names come from
 * str_get, which returns "" for ids >= 0xE2, so the new name uses id 0xE2.
 */
#include <stdint.h>
#include "fw_api.h"
#include "tm_mode.h"

#define SETTINGS_TABLE     0x08031AB0u
#define SETTINGS_MINI_OFS  0xF0u      /* table of the 25/37-key models */
#define MINI_KEYS_LIMIT    0x31       /* key count below this = Mini */
#define TM_ITEM            17
#define STR_TM_CHANNEL     0xE2
#define STR_COUNT          0xE2
#define ITEM_TYPE_CHANNEL  8          /* value 0..15 shown as 1..16, like Keys Channel */

typedef struct {
    uint8_t  str, param;
    uint16_t pad;
    uint32_t type;
    uint8_t  max, steps;
    uint16_t pad2;
} settings_item_t;

static const settings_item_t tm_item = {STR_TM_CHANNEL, TM_CHANNEL_PARAM, 0, ITEM_TYPE_CHANNEL, 16, 16, 0};

static int is_mini(void)
{
    return device_key_count() < MINI_KEYS_LIMIT;
}

/* Hook (b.w at 0x0802B44C): settings_entry(i). */
const settings_item_t *cfw_settings_entry(uint32_t i)
{
    if (!is_mini())
        return (const settings_item_t *)(SETTINGS_TABLE + 12 * i);
    if (i == TM_ITEM)
        return &tm_item;
    return (const settings_item_t *)(SETTINGS_TABLE + SETTINGS_MINI_OFS + 12 * i);
}

/* Hook (b.w at 0x0802B428): number of items the arrows can reach. */
uint32_t cfw_settings_count_nav(void)
{
    return is_mini() ? TM_ITEM + 1 : 0x14;
}

/* Hook (b.w at 0x08015B0C): str_get(id) = language base + offset, "" when out of range. */
const char *cfw_str_get(uint32_t id)
{
    if (id == STR_TM_CHANNEL)
        return "Turing Channel";
    if ((int32_t)id >= STR_COUNT)
        return "";
    const char *s = g_str_lang_base[g_str_lang] + g_str_offsets[id];
    return s ? s : "";
}
