// SPDX-License-Identifier: LGPL-2.1 OR BSD-2-Clause
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */

#include <libarena/common.h>

#include <libarena/asan.h>
#include <libarena/bmag.h>
#include <libarena/buddy.h>

extern struct buddy __arena buddy;

struct segarr_entry {
	u8 __arena *block;
	size_t sz;
	u8 poison;
};

#define SEGARRLEN (512)
static struct segarr_entry __arena segarr[SEGARRLEN];
static void __arena *ptrs[17];

enum buddy_alloc_path_test_consts {
	TEST_BULK_NORDER = 3,
	TEST_BULK_ITEMS = 1 << TEST_BULK_NORDER,
	TEST_BULK_SIZE = 17,
	TEST_BULK_STEP = 32,
	TEST_BMAG_ORDER = 2,
	TEST_BMAG_SIZE = BUDDY_MIN_ALLOC_BYTES << TEST_BMAG_ORDER,
};

struct buddy_alloc_path_test_storage {
	void __arena *bulk[BMAG_CACHE_CAPACITY + 1];
	void __arena *to_free[BMAG_CACHE_CAPACITY + 1];
	void __arena *drain[BMAG_CAPACITY];
};

static struct buddy_alloc_path_test_storage __arena alloc_path_test;

size_t __arena alloc_sizes[] = { 3, 17, 1025, 129, 16350, 333, 9, 517 };
size_t __arena alloc_multiple_sizes[] = { 3, 17, 1025, 129, 16350, 333, 9, 517, 2099 };
size_t __arena alloc_free_sizes[] = { 3, 17, 64, 129, 256, 333, 512, 517 };
size_t __arena alignment_sizes[] = { 1, 3, 7, 8, 9, 15, 16, 17, 31,
				     32, 64, 100, 128, 255, 256, 512, 1000 };

SEC("syscall")
__weak int test_buddy_create(void)
{
	const int iters = 10;
	int ret, i;

	for (i = zero; i < iters && can_loop; i++) {
		ret = buddy_init(&buddy);
		if (ret)
			return ret;

#ifndef BPF_ARENA_ASAN
		if (!buddy.bdepot) {
			buddy_destroy(&buddy);
			return -EINVAL;
		}
#endif

		ret = buddy_destroy(&buddy);
		if (ret)
			return ret;
		if (buddy.deferred_free)
			return -EINVAL;
		if (buddy.bdepot)
			return -EINVAL;
	}

	return 0;
}

SEC("syscall")
__weak int test_buddy_alloc(void)
{
	void __arena *mem;
	int ret, i;

	for (i = zero; i < 8 && can_loop; i++) {
		ret = buddy_init(&buddy);
		if (ret)
			return ret;

		mem = buddy_alloc(&buddy, alloc_sizes[i]);
		if (!mem) {
			buddy_destroy(&buddy);
			return -ENOMEM;
		}

		buddy_destroy(&buddy);
	}

	return 0;
}

SEC("syscall")
__weak int test_buddy_alloc_free(void)
{
	const int iters = 800;
	void __arena *mem;
	int ret, i;

	ret = buddy_init(&buddy);
	if (ret)
		return ret;

	for (i = zero; i < iters && can_loop; i++) {
		mem = buddy_alloc(&buddy, alloc_free_sizes[(i * 5) % 8]);
		if (!mem) {
			buddy_destroy(&buddy);
			return -ENOMEM;
		}

		buddy_free(&buddy, mem);
	}

	buddy_destroy(&buddy);

	return 0;
}

SEC("syscall")
__weak int test_buddy_alloc_multiple(void)
{
	int ret, j;
	u32 i, idx;
	u8 __arena *mem;
	size_t sz;
	u8 poison;

	ret = buddy_init(&buddy);
	if (ret)
		return ret;

	/*
	 * Cycle through each size, allocating an entry in the
	 * segarr. Continue for SEGARRLEN iterations. For every
	 * allocation write down the size, use the current index
	 * as a poison value, and log it with the pointer in the
	 * segarr entry. Use the poison value to poison the entire
	 * allocated memory according to the size given.
	 */
	for (i = zero; i < SEGARRLEN && can_loop; i++) {
		sz = alloc_multiple_sizes[i % 9];
		poison = (u8)i;

		mem = buddy_alloc(&buddy, sz);
		if (!mem) {
			buddy_destroy(&buddy);
			arena_stdout("%s:%d", __func__, __LINE__);
			return -ENOMEM;
		}

		segarr[i].block = mem;
		segarr[i].sz = sz;
		segarr[i].poison = poison;

		for (j = zero; j < sz && can_loop; j++) {
			mem[j] = poison;
			if (mem[j] != poison) {
				buddy_destroy(&buddy);
				return -EINVAL;
			}
		}
	}

	/*
	 * Go to (i * 17) % SEGARRLEN, and free the block pointed to.
	 * Before freeing, check all bytes have the poisoned value
	 * corresponding to the element. If any values are unexpected,
	 * return an error. Skip some elements to test destroying the
	 * buddy allocator while data is still allocated.
	 */
	for (i = 10; i < SEGARRLEN && can_loop; i++) {
		idx = (i * 17) % SEGARRLEN;

		mem = segarr[idx].block;
		sz = segarr[idx].sz;
		poison = segarr[idx].poison;

		for (j = zero; j < sz && can_loop; j++) {
			if (mem[j] != poison) {
				buddy_destroy(&buddy);
				arena_stdout("%s:%d %lx %u vs %u", __func__,
					   __LINE__, (uintptr_t)&mem[j],
					   mem[j], poison);
				return -EINVAL;
			}
		}

		buddy_free(&buddy, mem);
	}

	buddy_destroy(&buddy);

	return 0;
}

SEC("syscall")
__weak int test_buddy_alignment(void)
{
	int ret;
	u32 i;

	ret = buddy_init(&buddy);
	if (ret)
		return ret;

	/* Allocate various sizes and check alignment */
	for (i = zero; i < 17 && can_loop; i++) {
		barrier_var(i);
		ptrs[i] = buddy_alloc(&buddy, alignment_sizes[i]);
		if (!ptrs[i]) {
			arena_stdout("alignment test: alloc failed for size %lu",
				   alignment_sizes[i]);
			buddy_destroy(&buddy);
			return -ENOMEM;
		}

		/* Check 8-byte alignment */
		if ((u64)ptrs[i] & 0x7) {
			arena_stdout(
				"alignment test: ptr %llx not 8-byte aligned (size %lu)",
				(u64)ptrs[i], alignment_sizes[i]);
			buddy_destroy(&buddy);
			return -EINVAL;
		}
	}

	/* Free all allocations */
	for (i = zero; i < 17 && can_loop; i++) {
		barrier_var(i);
		buddy_free(&buddy, ptrs[i]);
	}

	buddy_destroy(&buddy);

	return 0;
}

SEC("syscall")
__weak int test_buddy_bulk_alloc_free(void)
{
	const size_t uncached_size =
		(BUDDY_MIN_ALLOC_BYTES << (BMAG_NUM_ORDERS - 1)) + 1;
	void __arena *sentinel = (void __arena *)&alloc_path_test;
	u8 __arena *mem;
	unsigned int i, j, idx;
	int ret, destroy_ret;

	ret = buddy_init(&buddy);
	if (ret)
		return ret;

	/*
	 * A 17-byte request rounds up to a 32-byte buddy allocation. Verify
	 * that the bulk allocator returns every child at the expected stride,
	 * without writing beyond the requested output array.
	 */
	alloc_path_test.bulk[TEST_BULK_ITEMS] = sentinel;
	ret = buddy_alloc_bulk(&buddy, TEST_BULK_SIZE, TEST_BULK_NORDER,
			       alloc_path_test.bulk);
	if (ret)
		goto out;
	if (alloc_path_test.bulk[TEST_BULK_ITEMS] != sentinel) {
		ret = -EOVERFLOW;
		goto out;
	}

	for (i = zero; i < TEST_BULK_ITEMS && can_loop; i++) {
		mem = alloc_path_test.bulk[i];
		if (!mem) {
			ret = -ENOMEM;
			goto out;
		}
		if (i && (u64)mem !=
			 (u64)alloc_path_test.bulk[i - 1] + TEST_BULK_STEP) {
			ret = -EINVAL;
			goto out;
		}

		for (j = zero; j < TEST_BULK_SIZE && can_loop; j++)
			mem[j] = (u8)(i + 1);
	}

	for (i = zero; i < TEST_BULK_ITEMS && can_loop; i++) {
		mem = alloc_path_test.bulk[i];
		for (j = zero; j < TEST_BULK_SIZE && can_loop; j++) {
			if (mem[j] != (u8)(i + 1)) {
				ret = -EINVAL;
				goto out;
			}
		}

		/* Five is coprime to eight, producing a permutation of all items. */
		idx = (i * 5) & (TEST_BULK_ITEMS - 1);
		alloc_path_test.to_free[i] = alloc_path_test.bulk[idx];
	}
	alloc_path_test.to_free[TEST_BULK_ITEMS] = NULL;

	ret = buddy_free_bulk(&buddy, alloc_path_test.to_free,
			      TEST_BULK_ITEMS + 1);
	if (ret)
		goto out;

	/*
	 * Order eight is immediately above the magazine range. Free a bulk
	 * allocation one child at a time in a non-sequential order to exercise
	 * independent metadata and coalescing without the cache intercepting it.
	 */
	ret = buddy_alloc_bulk(&buddy, uncached_size, TEST_BULK_NORDER,
			       alloc_path_test.bulk);
	if (ret)
		goto out;

	for (i = zero; i < TEST_BULK_ITEMS && can_loop; i++) {
		idx = (i * 5) & (TEST_BULK_ITEMS - 1);
		ret = buddy_free(&buddy, alloc_path_test.bulk[idx]);
		if (ret)
			goto out;
	}

out:
	destroy_ret = buddy_destroy(&buddy);
	return ret ?: destroy_ret;
}

SEC("syscall")
__weak int test_buddy_bmag_paths(void)
{
#ifdef BPF_ARENA_ASAN
	return 0;
#else
	struct bdepot __arena *depot;
	struct bmag_cache __arena *cache;
	struct bmag_cache_order __arena *slots;
	void __arena *direct, *cached, *reused, *refilled, *rotated;
	unsigned int cpu, i;
	int ret, destroy_ret;

	ret = buddy_init(&buddy);
	if (ret)
		return ret;

	depot = buddy.bdepot;
	if (!depot) {
		ret = -EINVAL;
		goto out;
	}

	cpu = bpf_get_smp_processor_id();
	cache = BDEPOT_CACHE(depot, cpu);
	if (!cache) {
		ret = -EINVAL;
		goto out;
	}
	slots = &cache->orders[TEST_BMAG_ORDER];
	if (READ_ONCE(slots->elems)) {
		ret = -EINVAL;
		goto out;
	}

	/* A miss bulk-refills the local order and returns one object directly. */
	direct = buddy_alloc(&buddy, TEST_BMAG_SIZE);
	if (!direct || READ_ONCE(slots->elems) != BMAG_CAPACITY - 1) {
		ret = -ENOMEM;
		goto out;
	}

	/* Subsequent allocations and frees stay on the packed local stack. */
	cached = buddy_alloc(&buddy, TEST_BMAG_SIZE);
	if (!cached || READ_ONCE(slots->elems) != BMAG_CAPACITY - 2) {
		ret = -EINVAL;
		goto out;
	}

	ret = buddy_free(&buddy, cached);
	if (ret)
		goto out;
	reused = buddy_alloc(&buddy, TEST_BMAG_SIZE);
	if (reused != cached) {
		ret = -EINVAL;
		goto out;
	}

	/* Drain the local stack, then verify that the next miss refills it. */
	for (i = zero; i < BMAG_CAPACITY - 2 && can_loop; i++) {
		alloc_path_test.drain[i] = buddy_alloc(&buddy, TEST_BMAG_SIZE);
		if (!alloc_path_test.drain[i]) {
			ret = -ENOMEM;
			goto out;
		}
	}
	if (READ_ONCE(slots->elems)) {
		ret = -EINVAL;
		goto out;
	}

	refilled = buddy_alloc(&buddy, TEST_BMAG_SIZE);
	if (!refilled || READ_ONCE(slots->elems) != BMAG_CAPACITY - 1) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Bulk allocation bypasses the cache. Fill the remaining local slots and
	 * verify duplicate frees are rejected while the object is still cached.
	 */
	ret = buddy_alloc_bulk(&buddy, TEST_BMAG_SIZE,
			       arena_fls(BMAG_CACHE_CAPACITY) - 1,
			       alloc_path_test.bulk);
	if (ret)
		goto out;

	ret = buddy_free(&buddy, alloc_path_test.bulk[0]);
	if (ret)
		goto out;
	if (!buddy_free(&buddy, alloc_path_test.bulk[0])) {
		ret = -EINVAL;
		goto out;
	}

	ret = buddy_free(&buddy, alloc_path_test.bulk[1]);
	if (ret)
		goto out;
	alloc_path_test.to_free[0] = alloc_path_test.bulk[1];
	if (!buddy_free_bulk(&buddy, alloc_path_test.to_free, 1)) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 2; i <= BMAG_CACHE_CAPACITY - BMAG_CAPACITY && can_loop; i++) {
		ret = buddy_free(&buddy, alloc_path_test.bulk[i]);
		if (ret)
			goto out;
	}
	if (READ_ONCE(slots->elems) != BMAG_CACHE_CAPACITY) {
		ret = -EINVAL;
		goto out;
	}

	/* A free beyond local capacity falls through to the central buddy. */
	ret = buddy_free(&buddy, alloc_path_test.bulk[BMAG_CACHE_CAPACITY -
						       BMAG_CAPACITY + 1]);
	if (ret)
		goto out;

	rotated = buddy_alloc(&buddy, TEST_BMAG_SIZE);
	if (rotated != alloc_path_test.bulk[BMAG_CACHE_CAPACITY -
						 BMAG_CAPACITY] ||
	    READ_ONCE(slots->elems) != BMAG_CACHE_CAPACITY - 1) {
		ret = -EINVAL;
		goto out;
	}

	/* Interior pointers are rejected without consuming the allocation. */
	i = BMAG_CACHE_CAPACITY - BMAG_CAPACITY + 2;
	if (!buddy_free(&buddy, (u8 __arena *)alloc_path_test.bulk[i] +
				 BUDDY_MIN_ALLOC_BYTES)) {
		ret = -EINVAL;
		goto out;
	}
	ret = buddy_free(&buddy, alloc_path_test.bulk[i]);

out:
	destroy_ret = buddy_destroy(&buddy);
	return ret ?: destroy_ret;
#endif
}

__weak char _license[] SEC("license") = "GPL";
