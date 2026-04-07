/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2025-2026 Meta Platforms, Inc. and affiliates.
 */

#include <common.h>

#include <asan.h>
#include <buddy.h>
#include <minheap.h>

static void
minheap_swap_elems(struct minheap_elem __arena *a __arg_arena,
                       struct minheap_elem __arena *b __arg_arena)
{
        u64 tmp_elem = a->elem;
        u64 tmp_weight = a->weight;

        a->elem = b->elem;
        a->weight = b->weight;

        b->elem = tmp_elem;
        b->weight = tmp_weight;
}

__weak
u64 minheap_alloc_internal(size_t capacity)
{
	size_t alloc_size = sizeof(minheap_t);
	minheap_t *heap;

	heap = malloc(alloc_size);
	if (!heap)
		return (u64)NULL;

	heap->helems = malloc(capacity * sizeof(*heap->helems));
	if (!heap->helems) {
		free(heap);
		return (u64)NULL;
	}

	heap->capacity = capacity;
	heap->size = 0;

	return (u64)heap;
}

__weak
void minheap_free(u64 heap)
{
	if (heap)
		free(heap);
}

__weak
int minheap_balance_top_down(void __arena *heap_ptr __arg_arena)
{
	minheap_t *heap = (minheap_t *)heap_ptr;
	int child, next;
	int off, ind;

	for (ind = 0; ind < heap->size && can_loop; ind = next) {

		next = ind;
		for (off = 1; off < 3 && can_loop; off++) {
			/*
			 * Correspondence between parent and children is:
			 * y = 2x + 1, y = 2x + 2
			 */
			child = 2 * ind + off;

			if (child >= heap->size)
				continue;

			if (heap->helems[next].weight <= heap->helems[child].weight)
				continue;

			next = child;
		}

		if (next == ind)
			break;

		minheap_swap_elems(&heap->helems[next], &heap->helems[ind]);
	}

	return 0;
}

static
int minheap_balance_bottom_up(void __arena *heap_ptr __arg_arena)
{
	minheap_t *heap = (minheap_t *)heap_ptr;
	int parent;
	int ind;

	for (ind = heap->size - 1; ind > 0 && can_loop; ind = parent) {
		parent = (ind - 1) >> 1;

		if (heap->helems[parent].weight <= heap->helems[ind].weight)
			break;

		minheap_swap_elems(&heap->helems[parent], &heap->helems[ind]);
	}

	return 0;
}

__hidden
int minheap_insert(void __arena *heap_ptr __arg_arena, u64 elem, u64 weight)
{
	minheap_t *heap = (minheap_t *)heap_ptr;

	if (heap->size == heap->capacity)
		return -ENOSPC;

	heap->helems[heap->size].elem = elem;
	heap->helems[heap->size].weight = weight;

	heap->size += 1;

	minheap_balance_bottom_up(heap);

	return 0;
}

/* Inlined because we are passing a non-arena pointer argument. */
__hidden
int minheap_pop(void __arena *heap_ptr __arg_arena, struct minheap_elem *helem __arg_trusted)
{
	minheap_t *heap = (minheap_t *)heap_ptr;

	if (heap->size == 0)
		return -EINVAL;

	helem->elem = heap->helems[0].elem;
	helem->weight = heap->helems[0].weight;

	heap->helems[0].elem = heap->helems[heap->size - 1].elem;
	heap->helems[0].weight = heap->helems[heap->size - 1].weight;

	heap->size -= 1;

	minheap_balance_top_down(heap);

	return 0;
}

__hidden
int minheap_dump(minheap_t *heap __arg_arena)
{
	int i;

	arena_stdout("HEAP %p SIZE %ld", heap, heap->size);
	for (i = 0; i < heap->size && can_loop; i++)
		arena_stdout("[0] (0x%lx, %ld)", heap->helems[i].elem, heap->helems[i].weight);

	return 0;
}
