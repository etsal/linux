/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2025-2026 Meta Platforms, Inc. and affiliates.
 */

#include <common.h>

#include <asan.h>
#include <buddy.h>
#include <bitmap.h>

size_t mask_size;

__weak
int bitmap_init(__u64 total_mask_size)
{
	mask_size = div_round_up(total_mask_size, 8);

	return 0;
}

__weak
u64 bitmap_alloc_internal(void)
{
	bitmap_t mask;
	int i;

	mask = malloc(mask_size * 8);
	if (unlikely(!mask))
		return (u64)(NULL);

	bpf_for(i, 0, mask_size) {
		mask->bits[i] = 0ULL;
	}

	return (u64)mask;
}

/*
 * XXXETSAL: Ideally these functions would have a void return type,
 * but as of 6.13 the verifier requires global functions to return a scalar.
 */

__weak
int bitmap_free(bitmap_t __arg_arena mask)
{
	free(mask);
	return 0;
}

__weak
int bitmap_set_cpu(u32 cpu, bitmap_t __arg_arena mask)
{
	mask->bits[cpu / 64] |= 1ULL << (cpu % 64);
	return 0;
}

__weak
int bitmap_clear_cpu(u32 cpu, bitmap_t __arg_arena mask)
{
	mask->bits[cpu / 64] &= ~(1ULL << (cpu % 64));
	return 0;
}

__weak
bool bitmap_test_cpu(u32 cpu, bitmap_t __arg_arena mask)
{
	return mask->bits[cpu / 64] & (1ULL << (cpu % 64));
}

__weak
bool bitmap_test_and_clear_cpu(u32 cpu, bitmap_t __arg_arena mask)
{
	u64 bit = 1ULL << (cpu % 64);
	u32 idx = cpu / 64;
	u64 actual;

	do {
		u64 old = mask->bits[idx];

		if (!(old & bit))
			return false;

		u64 new = old & ~bit;
		actual = cmpxchg(&mask->bits[idx], old, new);

		if (actual == old)
			return true;

	} while (can_loop);

	return false;
}

__weak
int bitmap_clear(bitmap_t __arg_arena mask)
{
	int i;

	bpf_for(i, 0, mask_size) {
		mask->bits[i] = 0;
	}

	return 0;
}

__weak
int bitmap_and(bitmap_t __arg_arena dst, bitmap_t __arg_arena src1, bitmap_t __arg_arena src2)
{
	int i;

	bpf_for(i, 0, mask_size) {
		dst->bits[i] = src1->bits[i] & src2->bits[i];
	}

	return 0;
}

__weak
int bitmap_or(bitmap_t __arg_arena dst, bitmap_t __arg_arena src1, bitmap_t __arg_arena src2)
{
	int i;

	bpf_for(i, 0, mask_size) {
		dst->bits[i] = src1->bits[i] | src2->bits[i];
	}

	return 0;
}

__weak
bool bitmap_empty(bitmap_t __arg_arena mask)
{
	int i;

	bpf_for(i, 0, mask_size) {
		if (mask->bits[i])
			return false;
	}

	return true;
}

__weak
int bitmap_copy(bitmap_t __arg_arena dst, bitmap_t __arg_arena src)
{
	int i;

	bpf_for(i, 0, mask_size) {
		dst->bits[i] = src->bits[i];
	}

	return 0;
}

__weak int
bitmap_from_cpumask(bitmap_t __arg_arena bmp, const cpumask_t *bpfmask __arg_trusted)
{
	int i;

	for (i = 0; i < sizeof(cpumask_t) / 8 && can_loop; i++) {
		if (i >= mask_size)
			break;
		bmp->bits[i] = bpfmask->bits[i];
	}

	return 0;
}

__weak
bool bitmap_subset(bitmap_t __arg_arena big, bitmap_t __arg_arena small)
{
	int i;

	bpf_for(i, 0, mask_size) {
		if (~big->bits[i] & small->bits[i])
			return false;
	}

	return true;
}

__weak
bool bitmap_intersects(bitmap_t __arg_arena arg1, bitmap_t __arg_arena arg2)
{
	int i;

	bpf_for(i, 0, mask_size) {
		if (arg1->bits[i] & arg2->bits[i])
			return true;
	}

	return false;
}

__weak
int bitmap_print(bitmap_t __arg_arena mask)
{
	int i;

	for (i = 0; i < mask_size && can_loop; i++)
		arena_stdout("%08x", mask->bits[i]);

	return 0;
}
