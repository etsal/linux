/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2025 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2025 Emil Tsalapatis <etsal@meta.com>
 */

#include <common.h>

#include <asan.h>

#include <buddy.h>
#include <minheap.h>

#include "selftest.h"

#define HEAP_CAPACITY (32ULL)

/*
 * Try to pop an empty heap.
 */
static
int test_minheap_empty(minheap_t *heap)
{
	struct minheap_elem helem;
	int ret;

	if (heap->size)
		return -EINVAL;

	ret = minheap_pop(heap, &helem);
	if (!ret)
		return -EINVAL;

	return 0;
}

/* XXX Overflowing maximum capacity test */

static
int test_minheap_ascending(minheap_t *heap)
{
	u64 keys[] = { 2, 5, 9, 15, 22, 30 };
	const size_t capacity = sizeof(keys) / sizeof(keys[0]);
	struct minheap_elem helem;
	u64 prev = 0;
	int ret, i;

	if (heap->size)
		return -EINVAL;

	if (capacity > heap->capacity)
		return -E2BIG;

	for (i = 0; i < capacity && can_loop; i++) {
		ret = minheap_insert(heap, keys[i], keys[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < capacity && can_loop; i++) {
		ret = minheap_pop(heap, &helem);
		if (ret)
			return ret;

		if (helem.elem != helem.weight) {
			bpf_printk("invalid element (%ld %ld)", prev, helem.weight);
			return -EINVAL;
		}

		if (prev > helem.weight) {
			bpf_printk("weight inversion %ld %ld", prev, helem.weight);
			return -EINVAL;
		}

		prev = helem.elem;
	}

	return 0;
}

static
int test_minheap_descending(minheap_t *heap)
{
	u64 keys[] = { 13, 11, 9, 7, 5, 3, 1 };
	const size_t capacity = sizeof(keys) / sizeof(keys[0]);
	struct minheap_elem helem;
	u64 prev = 0;
	int ret, i;

	if (heap->size)
		return -EINVAL;

	if (capacity > heap->capacity)
		return -E2BIG;

	for (i = 0; i < capacity && can_loop; i++) {
		ret = minheap_insert(heap, keys[i], keys[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < capacity && can_loop; i++) {
		ret = minheap_pop(heap, &helem);
		if (ret)
			return ret;

		if (helem.elem != helem.weight) {
			bpf_printk("invalid element (%ld %ld)", prev, helem.weight);
			return -EINVAL;
		}

		if (prev > helem.weight) {
			bpf_printk("weight inversion %ld %ld", prev, helem.weight);
			return -EINVAL;
		}

		prev = helem.elem;
	}

	return 0;
}

static
int test_minheap_alternating(minheap_t *heap)
{
	u64 keys[] = { 23, 12, 55, 42, 67, 3, 15, 8 };
	const size_t capacity = sizeof(keys) / sizeof(keys[0]);
	struct minheap_elem helem;
	u64 prev = 0;
	int ret, i;

	if (heap->size)
		return -EINVAL;

	if (capacity > heap->capacity)
		return -E2BIG;

	for (i = 0; i < capacity && can_loop; i++) {
		ret = minheap_insert(heap, keys[i], keys[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < capacity && can_loop; i++) {
		ret = minheap_pop(heap, &helem);
		if (ret)
			return ret;

		if (helem.elem != helem.weight) {
			bpf_printk("invalid element (%ld %ld)", prev, helem.weight);
			return -EINVAL;
		}

		if (prev > helem.weight) {
			bpf_printk("weight inversion %ld %ld", prev, helem.weight);
			return -EINVAL;
		}

		prev = helem.elem;
	}

	return 0;
}

static
int test_minheap_random(minheap_t *heap)
{
	u64 keys[] = { 97, 79, 88, 2, 51, 75, 71, 59, 12, 7, 37 };
	const size_t capacity = sizeof(keys) / sizeof(keys[0]);
	struct minheap_elem helem;
	u64 prev = 0;
	int ret, i;

	if (heap->size)
		return -EINVAL;

	if (capacity > heap->capacity)
		return -E2BIG;

	for (i = 0; i < capacity && can_loop; i++) {
		ret = minheap_insert(heap, keys[i], keys[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < capacity && can_loop; i++) {
		ret = minheap_pop(heap, &helem);
		if (ret)
			return ret;

		if (helem.elem != helem.weight) {
			bpf_printk("invalid element (%ld %ld)", prev, helem.weight);
			return -EINVAL;
		}

		if (prev > helem.weight) {
			bpf_printk("weight inversion %ld %ld", prev, helem.weight);
			return -EINVAL;
		}

		prev = helem.elem;
	}

	return 0;
}

static
int test_minheap_read_back(minheap_t *heap)
{
	struct minheap_elem helem;
	u64 elem = 5;
	u64 weight = 12;
	int ret;

	if (heap->size)
		return -EINVAL;

	ret = minheap_insert(heap, elem, weight);
	if (ret)
		return ret;

	ret = minheap_pop(heap, &helem);
	if (ret)
		return ret;

	if (helem.elem != elem) {
		bpf_printk("Expected elem %ld, found %d", elem, helem.elem);
		return -EINVAL;
	}

	if (helem.weight != weight) {
		bpf_printk("Expected elem %ld, found %d", weight, helem.weight);
		return -EINVAL;
	}

	return 0;
}
#define MINHEAP_SELFTEST(suffix) SELFTEST(test_minheap_ ## suffix, heap)

SEC("syscall")
__weak
int test_minheap(void)
{
	minheap_t *heap;

	heap = minheap_alloc(HEAP_CAPACITY);
	if (!heap) {
		bpf_printk("Could not allocate heap");
		return -ENOMEM;
	}

	MINHEAP_SELFTEST(empty);
	MINHEAP_SELFTEST(read_back);
	MINHEAP_SELFTEST(ascending);
	MINHEAP_SELFTEST(descending);
	MINHEAP_SELFTEST(alternating);
	MINHEAP_SELFTEST(random);

	return 0;
}
