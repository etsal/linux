#pragma once

/* Constants related to bmags. */
enum bmag_consts {
	/* Same minimum allocation as the buddy allocator. */
	BMAG_MIN_ALLOC_SHIFT = 4,
	BMAG_MIN_ALLOC_BYTES = 1 << BMAG_MIN_ALLOC_SHIFT,

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
	 * Items stored per bmag. Even if changed it should always be a power of
	 * 2 to avoid fragmentation during refills from the buddy allocator.
	 */
	BMAG_CAPACITY = 8,

	/*
	 * Mags per cpu, defined as a const to clearly mark the code that depends
	 * on it. Not much point in changing it.
	 */
	BMAG_PERCPU_MAGS = 2,

	/*
	 * Number of mags we adjust a bmag pool by. Defined as a constant
	 * to make the logic adjustable.
	 */
	BMAG_POOL_ADJUST_STEP = 1,
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
struct bmag_pool_percpu {
	/* 
	 * The cur and prev mags, defined as an array
	 * to slightly simplify bmag_pool_alloc/free.
	 */
	struct bmag __arena *bmags[BMAG_NUM_ORDERS][BMAG_PERCPU_MAGS];
} __attribute__((aligned(64)));

struct bmag_pool {
	/* Per-cpu caches. */
	struct bmag_pool_percpu percpu[BMAG_MAX_CPUS];

	/* Global full magazines list. */
	struct bmag __arena *full[BMAG_NUM_ORDERS];

	/* Global empty magazines list. */
	struct bmag __arena *empty;
};

#define BMAG_POOL_MAG(pool, order, ind)	((pool)->percpu[bpf_get_smp_processor_id()].bmags[(order)][(ind)])
#define BMAG_POOL_CUR(pool, order)	BMAG_POOL_MAG((pool), (order), 0)
#define BMAG_POOL_PREV(pool, order)	BMAG_POOL_MAG((pool), (order), 1)
