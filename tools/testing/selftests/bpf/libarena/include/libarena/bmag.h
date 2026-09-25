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
	 * The per-cpu rotation logic uses two magazines: current and previous.
	 */
	BMAG_PERCPU_MAGS = 2,

	/*
	 * Number of mags we adjust a bmag depot by. Defined as a constant
	 * to make the logic adjustable.
	 */
	BDEPOT_ADJUST_STEP = 1,
};

struct bmag {
	void __arena *objects[BMAG_CAPACITY];

	/*
	 * Either a bmag is in a full/empty freelist, or it is
	 * in use and can have 0 <= N <= BMAG_CAPACITY elements.
	 */
	union {
		struct bmag __arena *next;
		size_t elems;
	};
};

/* Per-cpu bmag cache. */
struct bmag_cache {
	/*
	 * The cur and prev mags, defined as an array
	 * to slightly simplify bdepot_alloc/free.
	 */
	struct bmag __arena *bmags[BMAG_NUM_ORDERS][BMAG_PERCPU_MAGS];
} __attribute__((aligned(64)));

struct bdepot {
	/* Per-cpu caches. */
	struct bmag_cache percpu[BMAG_MAX_CPUS];

	/* Global full magazines list. */
	struct bmag __arena *full[BMAG_NUM_ORDERS];

	/* Global empty magazines list. */
	struct bmag __arena *empty;
};

#define BDEPOT_MAG(depot, order, ind) \
	((depot)->percpu[bpf_get_smp_processor_id()].bmags[(order)][(ind)])
#define BDEPOT_CUR(depot, order)	BDEPOT_MAG((depot), (order), 0)
#define BDEPOT_PREV(depot, order)	BDEPOT_MAG((depot), (order), 1)

/* Helpers for NMI reentrancy. */

struct __attribute__((aligned(64))) bmag_active {
	unsigned int value;
};
