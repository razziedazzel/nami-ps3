#ifndef SPS_PATCH_H
#define SPS_PATCH_H

#include <stddef.h>
#include <stdint.h>

/* The PS3's decoder rejects H.264 streams declaring more than 4 reference frames at 1080p. Rewrite
 * any such SPS in the access unit to declare 4. Fields with a value of 5 or 6 are the same length
 * as 4 when coded, so this works in place.
 * Returns how many SPSs were changed, or -1 if one couldn't be parsed or patched. */
int sps_patch_au(uint8_t *data, size_t size);

#endif
