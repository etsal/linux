// SPDX-License-Identifier: LGPL-2.1 OR BSD-2-Clause
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */
#include <limits.h>

#include <libarena/common.h>
#include <libarena/asan.h>
#include <libarena/buddy.h>

struct buddy __arena buddy;
volatile u32 zero = 0;

int arena_fls(__u64 word)
{
	if (!word)
		return 0;

	return 64 - __builtin_clzll(word);
}

SEC("syscall")
__weak int arena_get_info(struct arena_get_info_args *args)
{
	args->arena_base = arena_base(&arena);

	return 0;
}

SEC("syscall")
__weak int arena_alloc_reserve(struct arena_alloc_reserve_args *args)
{
	return bpf_arena_reserve_pages(&arena, NULL, args->nr_pages);
}

SEC("syscall")
__weak int arena_buddy_reset(void)
{
	buddy_destroy(&buddy);

	return buddy_init(&buddy);
}

SEC("syscall")
__weak int arena_buddy_destroy(void)
{
	return buddy_destroy(&buddy);
}

__weak void __arena *arena_malloc(size_t size)
{
	return buddy_alloc(&buddy, size);
}

__weak void __arena *arena_calloc(size_t ncount, size_t size)
{
	void __arena *mem;
	size_t total;

	/* 
	 * Ideally we'd be using __builtin_mull_overflow here,
	 * but the BPF compiler backend doesn't implement __multi3.
	 * There are ways to optimize the division away from the
	 * overflow check, but any costs are dwarfed by the 
	 * buddy_alloc() call. Keep it simply for now?
	 */
	if (unlikely(ncount && size >= ULLONG_MAX / ncount))
		return NULL;

	total = ncount * size;

	mem = buddy_alloc(&buddy, total);
	if (likely(mem))
		arena_memset(mem, 0, total);

	return mem;
}

__weak void arena_free(void __arena *ptr)
{
	buddy_free(&buddy, ptr);
}

char _license[] SEC("license") = "GPL";
