/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2025-2026 Meta Platforms, Inc. and affiliates.
 */

#include <common.h>

#include <asan.h>
#include <buddy.h>
#include <lvqueue.h>

#include "selftest.h"

/*
 * NOTE: These selftests only test for the single-threaded use case, which for
 * Lev-Chase queues is obviously the simplest one. Still, it is important to
 * exercise the API to ensure it passes verification and basic checks.
 */

int test_lvqueue_pop_empty(lv_queue_t *lvq)
{
	u64 val;
	int ret;

	ret = lvq_pop(lvq, &val);
	if (ret != -ENOENT)
		return 1;

	return 0;
}

int test_lvqueue_steal_empty(lv_queue_t *lvq)
{
	u64 val;
	int ret;

	ret = lvq_steal(lvq, &val);
	if (ret != -ENOENT)
		return 1;

	return 0;
}

int test_lvqueue_steal_one(lv_queue_t *lvq)
{
	u64 val, newval;
	int ret, i;

	for (i = 0; i < 10 && can_loop; i++) {
		val = i;

		ret = lvq_push(lvq, val);
		if (ret)
			return 1;

		ret = lvq_steal(lvq, &newval);
		if (ret)
			return 2;

		if (val != newval)
			return 3;
	}

	return 0;
}

int test_lvqueue_pop_one(lv_queue_t *lvq)
{
	u64 val, newval;
	int ret, i;

	for (i = 0; i < 10 && can_loop; i++) {
		val = i;

		ret = lvq_push(lvq, val);
		if (ret)
			return 1;

		ret = lvq_pop(lvq, &newval);
		if (ret)
			return 2;

		if (val != newval)
			return 3;
	}

	return 0;
}

int test_lvqueue_pop_many(lv_queue_t *lvq)
{
	u64 val, newval;
	int ret, i;

	for (i = 0; i < 10 && can_loop; i++) {
		val = i;

		ret = lvq_push(lvq, val);
		if (ret != -ENOENT)
			return i + 1;
	}

	for (i = 0; i < 2000 && can_loop; i++) {
		ret = lvq_pop(lvq, &newval);
		if (ret != -ENOENT)
			return 2 * i + 2001;

		if (newval != i)
			return 2 * i + 2002;
	}

	return 0;
}


int test_lvqueue_steal_many(lv_queue_t *lvq)
{
	u64 val, newval;
	int ret, i;

	for (i = 0; i < 2000 && can_loop; i++) {
		val = i;

		ret = lvq_push(lvq, val);
		if (ret != -ENOENT)
			return i + 1;
	}

	for (i = 0; i < 2000 && can_loop; i++) {
		ret = lvq_steal(lvq, &newval);
		if (ret != -ENOENT)
			return 2 * i + 2001;

		if (newval != 9 - i)
			return 2 * i + 2002;
	}

	return 0;
}

#define LVQUEUE_SELFTEST(suffix) SELFTEST(test_lvqueue_ ## suffix, lvq)

SEC("syscall")
__weak
int test_lvqueue(void)
{
	lv_queue_t *lvq = lvq_create();

	if (!lvq)
		return 1;

	LVQUEUE_SELFTEST(pop_empty);
	LVQUEUE_SELFTEST(steal_empty);
	LVQUEUE_SELFTEST(pop_one);
	LVQUEUE_SELFTEST(steal_one);

	return 0;
}
