/* Stock firmware (fw 1563) functions and data used by cfw code.
 * Addresses come from analysis/symbols.txt; functions carry the Thumb bit. */

/* feature params (confsys) */
PROVIDE(confsys_get_param  = 0x08016C50 | 1);
PROVIDE(confsys_set_param  = 0x08016C88 | 1);
PROVIDE(feat_add_listener  = 0x08016DF6 | 1);
PROVIDE(feat_scale_init    = 0x0801F750 | 1);

/* display / text */
PROVIDE(fmt_version        = 0x08027F2A | 1);
PROVIDE(str_append         = 0x08027DE4 | 1);

/* data */
PROVIDE(g_scale_intervals  = 0x08031DE0);
