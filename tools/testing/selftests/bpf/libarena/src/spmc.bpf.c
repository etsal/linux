// SPDX-License-Identifier: LGPL-2.1 OR BSD-2-Clause
/*
 * Copyright (c) 2025-2026 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2025-2026 Emil Tsalapatis <etsal@meta.com>
 */

#include <bpf_atomic.h>

#include <libarena/common.h>

#include <libarena/asan.h>
#include <libarena/spmc.h>

static inline
u64 spmc_arr_size(spmc_arr_t spmc_arr)
{
	return SPMC_ARR_BASESZ << READ_ONCE(spmc_arr->order);
}

static inline
u64 spmc_arr_get(spmc_arr_t spmc_arr, u64 ind)
{
	u64 ret = READ_ONCE(spmc_arr->data[ind % spmc_arr_size(spmc_arr)]);

	return ret;
}

static inline
void spmc_arr_put(spmc_arr_t spmc_arr, u64 ind, u64 value)
{
	WRITE_ONCE(spmc_arr->data[ind % spmc_arr_size(spmc_arr)], value);
}

static inline
void spmc_arr_copy(spmc_arr_t dst, spmc_arr_t src, u64 b, u64 t)
{
	u64 i;

	for (i = t; i < b && can_loop; i++)
		spmc_arr_put(dst, i, spmc_arr_get(src, i));
}

static inline
int spmc_order_init(spmc_t spmc, int order)
{
	spmc_arr_t arr = &spmc->arr[order];

	if (unlikely(!spmc))
		return -EINVAL;

	if (order >= SPMC_ARR_ORDERS)
		return -E2BIG;

	/* Already allocated? */
	if (arr->data)
		return 0;

	arr->data = arena_malloc((SPMC_ARR_BASESZ << order) * sizeof(*arr->data));
	if (!arr->data)
		return -ENOMEM;

	return 0;
}

__weak
int spmc_owned_add(spmc_t spmc, u64 val)
{
	volatile u64 b, t;
	spmc_arr_t newarr;
	spmc_arr_t arr;
	ssize_t sz;
	int ret;

	if (unlikely(!spmc))
		return -EINVAL;

	b = smp_load_acquire(&spmc->bottom);

	/*
	 * In this call, loads from bottom and top should be
	 * in this order specifically (also see spmc_steal()).
	 */
	smp_rmb();

	t = READ_ONCE(spmc->top);
	arr = READ_ONCE(spmc->cur);

	sz = b - t;
	if (sz >= spmc_arr_size(arr) - 1) {
		ret = spmc_order_init(spmc, arr->order + 1);
		if (ret)
			return ret;

		newarr = &spmc->arr[arr->order + 1];

		spmc_arr_copy(newarr, arr, b, t);
		smp_store_release(&spmc->cur, newarr);
	}

	spmc_arr_put(spmc->cur, b, val);
	smp_store_release(&spmc->bottom, b + 1);

	return 0;
}


__weak
int spmc_owned_remove(spmc_t spmc, u64 *val)
{
	spmc_arr_t arr;
	volatile u64 b, t;
	int ret = 0;
	ssize_t sz;
	u64 value;

	if (unlikely(!spmc || !val))
		return -EINVAL;

	arr = smp_load_acquire(&spmc->cur);

	b = READ_ONCE(spmc->bottom);
	b -= 1;

	WRITE_ONCE(spmc->bottom, b);

	smp_mb();

	t = READ_ONCE(spmc->top);
	sz = b - t;
	if (sz < 0) {
		smp_store_release(&spmc->bottom, t);
		return -ENOENT;
	}

	value = spmc_arr_get(arr, b);
	if (sz > 0) {
		*val = value;
		return 0;
	}

	if (cmpxchg(&spmc->top, t, t + 1) != t)
		ret = -EAGAIN;

	smp_store_release(&spmc->bottom, t + 1);

	if (ret)
		return ret;

	*val = value;

	return 0;
}

__weak
int spmc_steal(spmc_t spmc, u64 *val)
{
	volatile u64 b, t;
	spmc_arr_t arr;
	ssize_t sz;
	u64 value;

	if (unlikely(!spmc || !val))
		return -EINVAL;

	t = smp_load_acquire(&spmc->top);

	/*
	 * It is important that t is read before b for
	 * stealers to avoid racing with the owner.
	 * Races between stealers are dealt with using
	 * CAS to increment the top value below.
	 */
	smp_rmb();

	b = READ_ONCE(spmc->bottom);
	arr = READ_ONCE(spmc->cur);

	sz = b - t;
	if (sz <= 0)
		return -ENOENT;

	value = spmc_arr_get(arr, t);

	if (cmpxchg(&spmc->top, t, t + 1) != t)
		return -EAGAIN;

	smp_store_release(val, value);

	return 0;
}


__weak
spmc_t spmc_create(void)
{
	/*
	 * Marked as volatile because otherwise the array
	 * reference in the internal loop gets demoted to
	 * scalar and the program fails verification.
	 */
	volatile spmc_t spmc;
	int ret, i;

	spmc = arena_malloc(sizeof(*spmc));
	if (!spmc)
		return NULL;

	WRITE_ONCE(spmc->bottom, 0);
	WRITE_ONCE(spmc->top, 0);

	for (i = 0; i < SPMC_ARR_ORDERS && can_loop; i++) {
		spmc->arr[i].data = NULL;
		spmc->arr[i].order = i;
	}

	ret = spmc_order_init((spmc_t)spmc, 0);
	if (ret) {
		arena_free(spmc);
		return NULL;
	}

	smp_store_release(&spmc->cur, &spmc->arr[0]);

	return (spmc_t)spmc;
}

__weak
int spmc_destroy(spmc_t spmc)
{
	int i;

	if (unlikely(!spmc))
		return -EINVAL;

	for (i = 0; i < SPMC_ARR_ORDERS && can_loop; i++)
		arena_free(spmc->arr[i].data);

	arena_free(spmc);

	return 0;
}
