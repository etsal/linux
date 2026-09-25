// SPDX-License-Identifier: LGPL-2.1 OR BSD-2-Clause
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */
#pragma once

/* Constants related to bmags. */
enum bmag_consts {
	/*
	 * Do not cache larger allocations to avoid excessive
	 * memory consumption from the per-cpu lists.
	 */
	BMAG_NUM_ORDERS = 1 << 3,

	/*
	 * Just like arena spinlocks, hardcode percpu state to a maximum number of CPUs.
	 * Since arena spinlocks are a hard dependency, use the exact same limit.
	 */
	BMAG_MAX_CPUS = _Q_MAX_CPUS,

	/*
	 * Items stored per bmag. This must remain a power of two because
	 * buddy_alloc_bulk() expresses the number of objects as an order.
	 */
	BMAG_CAPACITY = 8,

	/*
	 * Preserve the capacity of the previous two-magazine cache while storing
	 * all slots directly in a packed per-order slab.
	 */
	BMAG_CACHE_CAPACITY = BMAG_CAPACITY * 2,
};

struct bmag_cache_order {
	u32 elems;
	u32 __pad;
	void __arena *objects[BMAG_CACHE_CAPACITY];
} __attribute__((aligned(64)));

/* Dynamically allocated packed per-CPU slot slab. */
struct bmag_cache {
	struct bmag_cache_order orders[BMAG_NUM_ORDERS];
};

struct bdepot {
	struct bmag_cache __arena *percpu[BMAG_MAX_CPUS];
};

#define BDEPOT_CACHE(depot, cpu) ((depot)->percpu[(cpu)])

/* Helpers for NMI reentrancy. */

struct __attribute__((aligned(64))) bmag_active {
	unsigned int value;
};
