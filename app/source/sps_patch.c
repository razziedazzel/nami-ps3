#include "sps_patch.h"

#define NAL_SPS 7
#define MAX_REFS 4
#define MAX_PATCHABLE 6
#define EXTENDED_SAR 255

struct bits {
	uint8_t *d;
	size_t size;
	size_t byte;
	int bit;
	int error;
};

/* Step over emulation prevention bytes (the 03 in 00 00 03) when starting a byte. */
static void skip_emulation(struct bits *b)
{
	if (b->bit == 0 && b->byte >= 2 && b->byte < b->size && b->d[b->byte] == 3 &&
	    b->d[b->byte - 1] == 0 && b->d[b->byte - 2] == 0)
		b->byte++;
}

static uint32_t read_bit(struct bits *b)
{
	skip_emulation(b);
	if (b->byte >= b->size) {
		b->error = 1;
		return 0;
	}
	uint32_t v = (b->d[b->byte] >> (7 - b->bit)) & 1;
	if (++b->bit == 8) {
		b->bit = 0;
		b->byte++;
	}
	return v;
}

static uint32_t read_bits(struct bits *b, int count)
{
	uint32_t v = 0;
	while (count-- > 0)
		v = (v << 1) | read_bit(b);
	return v;
}

static uint32_t read_ue(struct bits *b)
{
	int zeros = 0;
	while (!read_bit(b) && !b->error) {
		if (++zeros > 31) {
			b->error = 1;
			return 0;
		}
	}
	return ((uint32_t)1 << zeros) - 1 + read_bits(b, zeros);
}

static int32_t read_se(struct bits *b)
{
	uint32_t v = read_ue(b);
	return (v & 1) ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2);
}

/* Overwrite the coded value at a saved position with the same-length code for MAX_REFS. */
static void write_max_refs(struct bits at)
{
	/* ue(4) is 00101. Only values 3 to 6 have that length, and callers check for 5 or 6. */
	static const uint8_t code[5] = {0, 0, 1, 0, 1};
	for (int i = 0; i < 5; i++) {
		skip_emulation(&at);
		uint8_t mask = (uint8_t)(0x80 >> at.bit);
		if (code[i])
			at.d[at.byte] |= mask;
		else
			at.d[at.byte] &= (uint8_t)~mask;
		if (++at.bit == 8) {
			at.bit = 0;
			at.byte++;
		}
	}
}

static int has_chroma_fields(uint32_t profile)
{
	switch (profile) {
	case 100: case 110: case 122: case 244: case 44: case 83: case 86:
	case 118: case 128: case 138: case 139: case 134: case 135:
		return 1;
	}
	return 0;
}

static void skip_scaling_list(struct bits *b, int count)
{
	int last = 8, next = 8;
	for (int i = 0; i < count; i++) {
		if (next != 0) {
			next = (last + read_se(b) + 256) % 256;
			last = next == 0 ? last : next;
		}
	}
}

static void skip_hrd(struct bits *b)
{
	uint32_t count = read_ue(b) + 1;
	if (count > 32) {
		b->error = 1;
		return;
	}
	read_bits(b, 8);
	for (uint32_t i = 0; i < count; i++) {
		read_ue(b);
		read_ue(b);
		read_bit(b);
	}
	read_bits(b, 20);
}

/* Returns 1 if changed, 0 if nothing needed changing, -1 on failure. d starts at the NAL header. */
static int patch_sps(uint8_t *d, size_t size)
{
	struct bits b = {d, size, 0, 0, 0};
	int changed = 0;

	read_bits(&b, 8); /* NAL header */
	uint32_t profile = read_bits(&b, 8);
	read_bits(&b, 16); /* constraint flags, level */
	read_ue(&b); /* sps id */

	if (has_chroma_fields(profile)) {
		uint32_t chroma = read_ue(&b);
		if (chroma == 3)
			read_bit(&b);
		read_ue(&b);
		read_ue(&b);
		read_bit(&b);
		if (read_bit(&b)) {
			int lists = chroma == 3 ? 12 : 8;
			for (int i = 0; i < lists; i++) {
				if (read_bit(&b))
					skip_scaling_list(&b, i < 6 ? 16 : 64);
			}
		}
	}

	read_ue(&b); /* log2_max_frame_num */
	uint32_t poc_type = read_ue(&b);
	if (poc_type == 0) {
		read_ue(&b);
	} else if (poc_type == 1) {
		read_bit(&b);
		read_se(&b);
		read_se(&b);
		uint32_t cycle = read_ue(&b);
		if (cycle > 255)
			return -1;
		for (uint32_t i = 0; i < cycle; i++)
			read_se(&b);
	}

	skip_emulation(&b);
	struct bits refs_at = b;
	uint32_t refs = read_ue(&b);
	read_bit(&b); /* gaps allowed */
	read_ue(&b); /* width */
	read_ue(&b); /* height */
	if (!read_bit(&b)) /* frame_mbs_only */
		read_bit(&b);
	read_bit(&b); /* direct 8x8 */
	if (read_bit(&b)) { /* crop */
		for (int i = 0; i < 4; i++)
			read_ue(&b);
	}

	struct bits dpb_at = b;
	uint32_t dpb = 0;
	int has_dpb = 0;
	if (read_bit(&b)) { /* VUI */
		if (read_bit(&b) && read_bits(&b, 8) == EXTENDED_SAR)
			read_bits(&b, 32);
		if (read_bit(&b)) /* overscan */
			read_bit(&b);
		if (read_bit(&b)) { /* video signal */
			read_bits(&b, 4);
			if (read_bit(&b))
				read_bits(&b, 24);
		}
		if (read_bit(&b)) { /* chroma location */
			read_ue(&b);
			read_ue(&b);
		}
		if (read_bit(&b)) { /* timing */
			read_bits(&b, 32);
			read_bits(&b, 32);
			read_bit(&b);
		}
		int nal_hrd = read_bit(&b);
		if (nal_hrd)
			skip_hrd(&b);
		int vcl_hrd = read_bit(&b);
		if (vcl_hrd)
			skip_hrd(&b);
		if (nal_hrd || vcl_hrd)
			read_bit(&b);
		read_bit(&b); /* pic struct */
		if (read_bit(&b)) { /* bitstream restriction */
			read_bit(&b);
			read_ue(&b);
			read_ue(&b);
			read_ue(&b);
			read_ue(&b);
			read_ue(&b); /* num_reorder */
			skip_emulation(&b);
			dpb_at = b;
			dpb = read_ue(&b);
			has_dpb = 1;
		}
	}
	if (b.error)
		return -1;

	if (refs > MAX_REFS) {
		if (refs > MAX_PATCHABLE)
			return -1;
		write_max_refs(refs_at);
		changed = 1;
	}
	if (has_dpb && dpb > MAX_REFS) {
		if (dpb > MAX_PATCHABLE)
			return -1;
		write_max_refs(dpb_at);
		changed = 1;
	}
	return changed;
}

int sps_patch_au(uint8_t *data, size_t size)
{
	int total = 0;
	for (size_t i = 0; i + 4 < size; i++) {
		if (data[i] || data[i + 1] || data[i + 2] != 1)
			continue;
		if ((data[i + 3] & 0x1F) == NAL_SPS) {
			size_t end = i + 4;
			while (end + 3 < size && (data[end] || data[end + 1] || data[end + 2] > 1))
				end++;
			if (end + 3 >= size)
				end = size;
			int r = patch_sps(data + i + 3, end - (i + 3));
			if (r < 0)
				return -1;
			total += r;
		}
		i += 2;
	}
	return total;
}
