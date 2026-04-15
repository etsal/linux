#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/vmpressure.h>
#include <linux/vmstat.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* for buffer_heads_over_limit */
#include <linux/mm_inline.h>
#include <linux/backing-dev.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/compaction.h>
#include <linux/notifier.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/memcontrol.h>
#include <linux/migrate.h>
#include <linux/delayacct.h>
#include <linux/sysctl.h>
#include <linux/memory-tiers.h>
#include <linux/oom.h>
#include <linux/pagevec.h>
#include <linux/prefetch.h>
#include <linux/printk.h>
#include <linux/dax.h>
#include <linux/psi.h>
#include <linux/pagewalk.h>
#include <linux/shmem_fs.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/khugepaged.h>
#include <linux/rculist_nulls.h>
#include <linux/random.h>
#include <linux/mmu_notifier.h>
#include <linux/parser.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>

#include <linux/swapops.h>
#include <linux/sched/sysctl.h>

#include "internal.h"
#include "swap.h"
#include "lru.h"

#include <trace/events/vmscan.h>

#ifdef CONFIG_LRU_GEN_ENABLED
DEFINE_STATIC_KEY_ARRAY_TRUE(lru_gen_caps, NR_LRU_GEN_CAPS);
#define get_cap(cap)	static_branch_likely(&lru_gen_caps[cap])
#else
DEFINE_STATIC_KEY_ARRAY_FALSE(lru_gen_caps, NR_LRU_GEN_CAPS);
#define get_cap(cap)	static_branch_unlikely(&lru_gen_caps[cap])
#endif

static bool should_walk_mmu(void)
{
	return arch_has_hw_pte_young() && get_cap(LRU_GEN_MM_WALK);
}

static bool should_clear_pmd_young(void)
{
	return arch_has_hw_nonleaf_pmd_young() && get_cap(LRU_GEN_NONLEAF_YOUNG);
}

/******************************************************************************
 *                          shorthand helpers
 ******************************************************************************/

#define DEFINE_MAX_SEQ(lruvec)						\
	unsigned long max_seq = READ_ONCE((lruvec)->lrugen.max_seq)

#define DEFINE_MIN_SEQ(lruvec)						\
	unsigned long min_seq[ANON_AND_FILE] = {			\
		READ_ONCE((lruvec)->lrugen.min_seq[LRU_GEN_ANON]),	\
		READ_ONCE((lruvec)->lrugen.min_seq[LRU_GEN_FILE]),	\
	}

/* Get the min/max evictable type based on swappiness */
#define min_type(swappiness) (!(swappiness))
#define max_type(swappiness) ((swappiness) < SWAPPINESS_ANON_ONLY)

#define evictable_min_seq(min_seq, swappiness)				\
	min((min_seq)[min_type(swappiness)], (min_seq)[max_type(swappiness)])

#define for_each_gen_type_zone(gen, type, zone)				\
	for ((gen) = 0; (gen) < MAX_NR_GENS; (gen)++)			\
		for ((type) = 0; (type) < ANON_AND_FILE; (type)++)	\
			for ((zone) = 0; (zone) < MAX_NR_ZONES; (zone)++)

#define for_each_evictable_type(type, swappiness)			\
	for ((type) = min_type(swappiness); (type) <= max_type(swappiness); (type)++)

#define get_memcg_gen(seq)	((seq) % MEMCG_NR_GENS)
#define get_memcg_bin(bin)	((bin) % MEMCG_NR_BINS)

static struct lruvec *get_lruvec(struct mem_cgroup *memcg, int nid)
{
	struct pglist_data *pgdat = NODE_DATA(nid);

#ifdef CONFIG_MEMCG
	if (memcg) {
		struct lruvec *lruvec = &memcg->nodeinfo[nid]->lruvec;

		/* see the comment in mem_cgroup_lruvec() */
		if (!lruvec->pgdat)
			lruvec->pgdat = pgdat;

		return lruvec;
	}
#endif
	VM_WARN_ON_ONCE(!mem_cgroup_disabled());

	return &pgdat->__lruvec;
}

static int get_swappiness(struct lruvec *lruvec, struct scan_control *sc)
{
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	if (!sc->may_swap)
		return 0;

	if (!can_demote(pgdat->node_id, sc, memcg) &&
	    mem_cgroup_get_nr_swap_pages(memcg) < MIN_LRU_BATCH)
		return 0;

	return sc_swappiness(sc, memcg);
}

static int get_nr_gens(struct lruvec *lruvec, int type)
{
	return lruvec->lrugen.max_seq - lruvec->lrugen.min_seq[type] + 1;
}

static bool __maybe_unused seq_is_valid(struct lruvec *lruvec)
{
	int type;

	for (type = 0; type < ANON_AND_FILE; type++) {
		int n = get_nr_gens(lruvec, type);

		if (n < MIN_NR_GENS || n > MAX_NR_GENS)
			return false;
	}

	return true;
}

/******************************************************************************
 *                          Bloom filters
 ******************************************************************************/

/*
 * Bloom filters with m=1<<15, k=2 and the false positive rates of ~1/5 when
 * n=10,000 and ~1/2 when n=20,000, where, conventionally, m is the number of
 * bits in a bitmap, k is the number of hash functions and n is the number of
 * inserted items.
 *
 * Page table walkers use one of the two filters to reduce their search space.
 * To get rid of non-leaf entries that no longer have enough leaf entries, the
 * aging uses the double-buffering technique to flip to the other filter each
 * time it produces a new generation. For non-leaf entries that have enough
 * leaf entries, the aging carries them over to the next generation in
 * walk_pmd_range(); the eviction also report them when walking the rmap
 * in lru_gen_look_around().
 *
 * For future optimizations:
 * 1. It's not necessary to keep both filters all the time. The spare one can be
 *    freed after the RCU grace period and reallocated if needed again.
 * 2. And when reallocating, it's worth scaling its size according to the number
 *    of inserted entries in the other filter, to reduce the memory overhead on
 *    small systems and false positives on large systems.
 * 3. Jenkins' hash function is an alternative to Knuth's.
 */
#define BLOOM_FILTER_SHIFT	15

static inline int filter_gen_from_seq(unsigned long seq)
{
	return seq % NR_BLOOM_FILTERS;
}

static void get_item_key(void *item, int *key)
{
	u32 hash = hash_ptr(item, BLOOM_FILTER_SHIFT * 2);

	BUILD_BUG_ON(BLOOM_FILTER_SHIFT * 2 > BITS_PER_TYPE(u32));

	key[0] = hash & (BIT(BLOOM_FILTER_SHIFT) - 1);
	key[1] = hash >> BLOOM_FILTER_SHIFT;
}

static bool test_bloom_filter(struct lru_gen_mm_state *mm_state, unsigned long seq,
			      void *item)
{
	int key[2];
	unsigned long *filter;
	int gen = filter_gen_from_seq(seq);

	filter = READ_ONCE(mm_state->filters[gen]);
	if (!filter)
		return true;

	get_item_key(item, key);

	return test_bit(key[0], filter) && test_bit(key[1], filter);
}

static void update_bloom_filter(struct lru_gen_mm_state *mm_state, unsigned long seq,
				void *item)
{
	int key[2];
	unsigned long *filter;
	int gen = filter_gen_from_seq(seq);

	filter = READ_ONCE(mm_state->filters[gen]);
	if (!filter)
		return;

	get_item_key(item, key);

	if (!test_bit(key[0], filter))
		set_bit(key[0], filter);
	if (!test_bit(key[1], filter))
		set_bit(key[1], filter);
}

static void reset_bloom_filter(struct lru_gen_mm_state *mm_state, unsigned long seq)
{
	unsigned long *filter;
	int gen = filter_gen_from_seq(seq);

	filter = mm_state->filters[gen];
	if (filter) {
		bitmap_clear(filter, 0, BIT(BLOOM_FILTER_SHIFT));
		return;
	}

	filter = bitmap_zalloc(BIT(BLOOM_FILTER_SHIFT),
			       __GFP_HIGH | __GFP_NOMEMALLOC | __GFP_NOWARN);
	WRITE_ONCE(mm_state->filters[gen], filter);
}

/******************************************************************************
 *                          mm_struct list
 ******************************************************************************/

#ifdef CONFIG_LRU_GEN_WALKS_MMU

static struct lru_gen_mm_list *get_mm_list(struct mem_cgroup *memcg)
{
	static struct lru_gen_mm_list mm_list = {
		.fifo = LIST_HEAD_INIT(mm_list.fifo),
		.lock = __SPIN_LOCK_UNLOCKED(mm_list.lock),
	};

#ifdef CONFIG_MEMCG
	if (memcg)
		return &memcg->mm_list;
#endif
	VM_WARN_ON_ONCE(!mem_cgroup_disabled());

	return &mm_list;
}

static struct lru_gen_mm_state *get_mm_state(struct lruvec *lruvec)
{
	return &lruvec->mm_state;
}

static struct mm_struct *get_next_mm(struct lru_gen_mm_walk *walk)
{
	int key;
	struct mm_struct *mm;
	struct pglist_data *pgdat = lruvec_pgdat(walk->lruvec);
	struct lru_gen_mm_state *mm_state = get_mm_state(walk->lruvec);

	mm = list_entry(mm_state->head, struct mm_struct, lru_gen.list);
	key = pgdat->node_id % BITS_PER_TYPE(mm->lru_gen.bitmap);

	if (!walk->force_scan && !test_bit(key, &mm->lru_gen.bitmap))
		return NULL;

	clear_bit(key, &mm->lru_gen.bitmap);

	return mmget_not_zero(mm) ? mm : NULL;
}

void lru_gen_add_mm(struct mm_struct *mm)
{
	int nid;
	struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);

	VM_WARN_ON_ONCE(!list_empty(&mm->lru_gen.list));
#ifdef CONFIG_MEMCG
	VM_WARN_ON_ONCE(mm->lru_gen.memcg);
	mm->lru_gen.memcg = memcg;
#endif
	spin_lock(&mm_list->lock);

	for_each_node_state(nid, N_MEMORY) {
		struct lruvec *lruvec = get_lruvec(memcg, nid);
		struct lru_gen_mm_state *mm_state = get_mm_state(lruvec);

		/* the first addition since the last iteration */
		if (mm_state->tail == &mm_list->fifo)
			mm_state->tail = &mm->lru_gen.list;
	}

	list_add_tail(&mm->lru_gen.list, &mm_list->fifo);

	spin_unlock(&mm_list->lock);
}

void lru_gen_del_mm(struct mm_struct *mm)
{
	int nid;
	struct lru_gen_mm_list *mm_list;
	struct mem_cgroup *memcg = NULL;

	if (list_empty(&mm->lru_gen.list))
		return;

#ifdef CONFIG_MEMCG
	memcg = mm->lru_gen.memcg;
#endif
	mm_list = get_mm_list(memcg);

	spin_lock(&mm_list->lock);

	for_each_node(nid) {
		struct lruvec *lruvec = get_lruvec(memcg, nid);
		struct lru_gen_mm_state *mm_state = get_mm_state(lruvec);

		/* where the current iteration continues after */
		if (mm_state->head == &mm->lru_gen.list)
			mm_state->head = mm_state->head->prev;

		/* where the last iteration ended before */
		if (mm_state->tail == &mm->lru_gen.list)
			mm_state->tail = mm_state->tail->next;
	}

	list_del_init(&mm->lru_gen.list);

	spin_unlock(&mm_list->lock);

#ifdef CONFIG_MEMCG
	mem_cgroup_put(mm->lru_gen.memcg);
	mm->lru_gen.memcg = NULL;
#endif
}

#ifdef CONFIG_MEMCG
void lru_gen_migrate_mm(struct mm_struct *mm)
{
	struct mem_cgroup *memcg;
	struct task_struct *task = rcu_dereference_protected(mm->owner, true);

	VM_WARN_ON_ONCE(task->mm != mm);
	lockdep_assert_held(&task->alloc_lock);

	/* for mm_update_next_owner() */
	if (mem_cgroup_disabled())
		return;

	/* migration can happen before addition */
	if (!mm->lru_gen.memcg)
		return;

	rcu_read_lock();
	memcg = mem_cgroup_from_task(task);
	rcu_read_unlock();
	if (memcg == mm->lru_gen.memcg)
		return;

	VM_WARN_ON_ONCE(list_empty(&mm->lru_gen.list));

	lru_gen_del_mm(mm);
	lru_gen_add_mm(mm);
}
#endif

#else /* !CONFIG_LRU_GEN_WALKS_MMU */

static struct lru_gen_mm_list *get_mm_list(struct mem_cgroup *memcg)
{
	return NULL;
}

static struct lru_gen_mm_state *get_mm_state(struct lruvec *lruvec)
{
	return NULL;
}

static struct mm_struct *get_next_mm(struct lru_gen_mm_walk *walk)
{
	return NULL;
}

#endif

static void reset_mm_stats(struct lru_gen_mm_walk *walk, bool last)
{
	int i;
	int hist;
	struct lruvec *lruvec = walk->lruvec;
	struct lru_gen_mm_state *mm_state = get_mm_state(lruvec);

	lockdep_assert_held(&get_mm_list(lruvec_memcg(lruvec))->lock);

	hist = lru_hist_from_seq(walk->seq);

	for (i = 0; i < NR_MM_STATS; i++) {
		WRITE_ONCE(mm_state->stats[hist][i],
			   mm_state->stats[hist][i] + walk->mm_stats[i]);
		walk->mm_stats[i] = 0;
	}

	if (NR_HIST_GENS > 1 && last) {
		hist = lru_hist_from_seq(walk->seq + 1);

		for (i = 0; i < NR_MM_STATS; i++)
			WRITE_ONCE(mm_state->stats[hist][i], 0);
	}
}

static bool iterate_mm_list(struct lru_gen_mm_walk *walk, struct mm_struct **iter)
{
	bool first = false;
	bool last = false;
	struct mm_struct *mm = NULL;
	struct lruvec *lruvec = walk->lruvec;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);
	struct lru_gen_mm_state *mm_state = get_mm_state(lruvec);

	/*
	 * mm_state->seq is incremented after each iteration of mm_list. There
	 * are three interesting cases for this page table walker:
	 * 1. It tries to start a new iteration with a stale max_seq: there is
	 *    nothing left to do.
	 * 2. It started the next iteration: it needs to reset the Bloom filter
	 *    so that a fresh set of PTE tables can be recorded.
	 * 3. It ended the current iteration: it needs to reset the mm stats
	 *    counters and tell its caller to increment max_seq.
	 */
	spin_lock(&mm_list->lock);

	VM_WARN_ON_ONCE(mm_state->seq + 1 < walk->seq);

	if (walk->seq <= mm_state->seq)
		goto done;

	if (!mm_state->head)
		mm_state->head = &mm_list->fifo;

	if (mm_state->head == &mm_list->fifo)
		first = true;

	do {
		mm_state->head = mm_state->head->next;
		if (mm_state->head == &mm_list->fifo) {
			WRITE_ONCE(mm_state->seq, mm_state->seq + 1);
			last = true;
			break;
		}

		/* force scan for those added after the last iteration */
		if (!mm_state->tail || mm_state->tail == mm_state->head) {
			mm_state->tail = mm_state->head->next;
			walk->force_scan = true;
		}
	} while (!(mm = get_next_mm(walk)));
done:
	if (*iter || last)
		reset_mm_stats(walk, last);

	spin_unlock(&mm_list->lock);

	if (mm && first)
		reset_bloom_filter(mm_state, walk->seq + 1);

	if (*iter)
		mmput_async(*iter);

	*iter = mm;

	return last;
}

static bool iterate_mm_list_nowalk(struct lruvec *lruvec, unsigned long seq)
{
	bool success = false;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);
	struct lru_gen_mm_state *mm_state = get_mm_state(lruvec);

	spin_lock(&mm_list->lock);

	VM_WARN_ON_ONCE(mm_state->seq + 1 < seq);

	if (seq > mm_state->seq) {
		mm_state->head = NULL;
		mm_state->tail = NULL;
		WRITE_ONCE(mm_state->seq, mm_state->seq + 1);
		success = true;
	}

	spin_unlock(&mm_list->lock);

	return success;
}

/******************************************************************************
 *                          PID controller
 ******************************************************************************/

/*
 * A feedback loop based on Proportional-Integral-Derivative (PID) controller.
 *
 * The P term is refaulted/(evicted+protected) from a tier in the generation
 * currently being evicted; the I term is the exponential moving average of the
 * P term over the generations previously evicted, using the smoothing factor
 * 1/2; the D term isn't supported.
 *
 * The setpoint (SP) is always the first tier of one type; the process variable
 * (PV) is either any tier of the other type or any other tier of the same
 * type.
 *
 * The error is the difference between the SP and the PV; the correction is to
 * turn off protection when SP>PV or turn on protection when SP<PV.
 *
 * For future optimizations:
 * 1. The D term may discount the other two terms over time so that long-lived
 *    generations can resist stale information.
 */
struct ctrl_pos {
	unsigned long refaulted;
	unsigned long total;
	int gain;
};

static void read_ctrl_pos(struct lruvec *lruvec, int type, int tier, int gain,
			  struct ctrl_pos *pos)
{
	int i;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	int hist = lru_hist_from_seq(lrugen->min_seq[type]);

	pos->gain = gain;
	pos->refaulted = pos->total = 0;

	for (i = tier % MAX_NR_TIERS; i <= min(tier, MAX_NR_TIERS - 1); i++) {
		pos->refaulted += lrugen->avg_refaulted[type][i] +
				  atomic_long_read(&lrugen->refaulted[hist][type][i]);
		pos->total += lrugen->avg_total[type][i] +
			      lrugen->protected[hist][type][i] +
			      atomic_long_read(&lrugen->evicted[hist][type][i]);
	}
}

static void reset_ctrl_pos(struct lruvec *lruvec, int type, bool carryover)
{
	int hist, tier;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	bool clear = carryover ? NR_HIST_GENS == 1 : NR_HIST_GENS > 1;
	unsigned long seq = carryover ? lrugen->min_seq[type] : lrugen->max_seq + 1;

	lockdep_assert_held(&lruvec->lru_lock);

	if (!carryover && !clear)
		return;

	hist = lru_hist_from_seq(seq);

	for (tier = 0; tier < MAX_NR_TIERS; tier++) {
		if (carryover) {
			unsigned long sum;

			sum = lrugen->avg_refaulted[type][tier] +
			      atomic_long_read(&lrugen->refaulted[hist][type][tier]);
			WRITE_ONCE(lrugen->avg_refaulted[type][tier], sum / 2);

			sum = lrugen->avg_total[type][tier] +
			      lrugen->protected[hist][type][tier] +
			      atomic_long_read(&lrugen->evicted[hist][type][tier]);
			WRITE_ONCE(lrugen->avg_total[type][tier], sum / 2);
		}

		if (clear) {
			atomic_long_set(&lrugen->refaulted[hist][type][tier], 0);
			atomic_long_set(&lrugen->evicted[hist][type][tier], 0);
			WRITE_ONCE(lrugen->protected[hist][type][tier], 0);
		}
	}
}

static bool positive_ctrl_err(struct ctrl_pos *sp, struct ctrl_pos *pv)
{
	/*
	 * Return true if the PV has a limited number of refaults or a lower
	 * refaulted/total than the SP.
	 */
	return pv->refaulted < MIN_LRU_BATCH ||
	       pv->refaulted * (sp->total + MIN_LRU_BATCH) * sp->gain <=
	       (sp->refaulted + 1) * pv->total * pv->gain;
}

/******************************************************************************
 *                          the aging
 ******************************************************************************/

/* promote pages accessed through page tables */
static int folio_update_gen(struct folio *folio, int gen)
{
	unsigned long new_flags, old_flags = READ_ONCE(folio->flags.f);

	VM_WARN_ON_ONCE(gen >= MAX_NR_GENS);

	/* see the comment on LRU_REFS_FLAGS */
	if (!folio_test_referenced(folio) && !folio_test_workingset(folio)) {
		set_mask_bits(&folio->flags.f, LRU_REFS_MASK, BIT(PG_referenced));
		return -1;
	}

	do {
		/* lru_gen_del_folio() has isolated this page? */
		if (!(old_flags & LRU_GEN_MASK))
			return -1;

		new_flags = old_flags & ~(LRU_GEN_MASK | LRU_REFS_FLAGS);
		new_flags |= ((gen + 1UL) << LRU_GEN_PGOFF) | BIT(PG_workingset);
	} while (!try_cmpxchg(&folio->flags.f, &old_flags, new_flags));

	return ((old_flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
}

/* protect pages accessed multiple times through file descriptors */
static int folio_inc_gen(struct lruvec *lruvec, struct folio *folio, bool reclaiming)
{
	int type = folio_is_file_lru(folio);
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	int new_gen, old_gen = lru_gen_from_seq(lrugen->min_seq[type]);
	unsigned long new_flags, old_flags = READ_ONCE(folio->flags.f);

	VM_WARN_ON_ONCE_FOLIO(!(old_flags & LRU_GEN_MASK), folio);

	do {
		new_gen = ((old_flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
		/* folio_update_gen() has promoted this page? */
		if (new_gen >= 0 && new_gen != old_gen)
			return new_gen;

		new_gen = (old_gen + 1) % MAX_NR_GENS;

		new_flags = old_flags & ~(LRU_GEN_MASK | LRU_REFS_FLAGS);
		new_flags |= (new_gen + 1UL) << LRU_GEN_PGOFF;
		/* for folio_end_writeback() */
		if (reclaiming)
			new_flags |= BIT(PG_reclaim);
	} while (!try_cmpxchg(&folio->flags.f, &old_flags, new_flags));

	lru_gen_update_size(lruvec, folio, old_gen, new_gen);

	return new_gen;
}

static void update_batch_size(struct lru_gen_mm_walk *walk, struct folio *folio,
			      int old_gen, int new_gen)
{
	int type = folio_is_file_lru(folio);
	int zone = folio_zonenum(folio);
	int delta = folio_nr_pages(folio);

	VM_WARN_ON_ONCE(old_gen >= MAX_NR_GENS);
	VM_WARN_ON_ONCE(new_gen >= MAX_NR_GENS);

	walk->batched++;

	walk->nr_pages[old_gen][type][zone] -= delta;
	walk->nr_pages[new_gen][type][zone] += delta;
}

static void reset_batch_size(struct lru_gen_mm_walk *walk)
{
	int gen, type, zone;
	struct lruvec *lruvec = walk->lruvec;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;

	walk->batched = 0;

	for_each_gen_type_zone(gen, type, zone) {
		enum lru_list lru = type * LRU_INACTIVE_FILE;
		int delta = walk->nr_pages[gen][type][zone];

		if (!delta)
			continue;

		walk->nr_pages[gen][type][zone] = 0;
		WRITE_ONCE(lrugen->nr_pages[gen][type][zone],
			   lrugen->nr_pages[gen][type][zone] + delta);

		if (lru_gen_is_active(lruvec, gen))
			lru += LRU_ACTIVE;
		__update_lru_size(lruvec, lru, zone, delta);
	}
}

static int should_skip_vma(unsigned long start, unsigned long end, struct mm_walk *args)
{
	struct address_space *mapping;
	struct vm_area_struct *vma = args->vma;
	struct lru_gen_mm_walk *walk = args->private;

	if (!vma_is_accessible(vma))
		return true;

	if (is_vm_hugetlb_page(vma))
		return true;

	if (!vma_has_recency(vma))
		return true;

	if (vma->vm_flags & (VM_LOCKED | VM_SPECIAL))
		return true;

	if (vma == get_gate_vma(vma->vm_mm))
		return true;

	if (vma_is_anonymous(vma))
		return !walk->swappiness;

	if (WARN_ON_ONCE(!vma->vm_file || !vma->vm_file->f_mapping))
		return true;

	mapping = vma->vm_file->f_mapping;
	if (mapping_unevictable(mapping))
		return true;

	if (shmem_mapping(mapping))
		return !walk->swappiness;

	if (walk->swappiness > MAX_SWAPPINESS)
		return true;

	/* to exclude special mappings like dax, etc. */
	return !mapping->a_ops->read_folio;
}

/*
 * Some userspace memory allocators map many single-page VMAs. Instead of
 * returning back to the PGD table for each of such VMAs, finish an entire PMD
 * table to reduce zigzags and improve cache performance.
 */
static bool get_next_vma(unsigned long mask, unsigned long size, struct mm_walk *args,
			 unsigned long *vm_start, unsigned long *vm_end)
{
	unsigned long start = round_up(*vm_end, size);
	unsigned long end = (start | ~mask) + 1;
	VMA_ITERATOR(vmi, args->mm, start);

	VM_WARN_ON_ONCE(mask & size);
	VM_WARN_ON_ONCE((start & mask) != (*vm_start & mask));

	for_each_vma(vmi, args->vma) {
		if (end && end <= args->vma->vm_start)
			return false;

		if (should_skip_vma(args->vma->vm_start, args->vma->vm_end, args))
			continue;

		*vm_start = max(start, args->vma->vm_start);
		*vm_end = min(end - 1, args->vma->vm_end - 1) + 1;

		return true;
	}

	return false;
}

static unsigned long get_pte_pfn(pte_t pte, struct vm_area_struct *vma, unsigned long addr,
				 struct pglist_data *pgdat)
{
	unsigned long pfn = pte_pfn(pte);

	VM_WARN_ON_ONCE(addr < vma->vm_start || addr >= vma->vm_end);

	if (!pte_present(pte) || is_zero_pfn(pfn))
		return -1;

	if (WARN_ON_ONCE(pte_special(pte)))
		return -1;

	if (!pte_young(pte) && !mm_has_notifiers(vma->vm_mm))
		return -1;

	if (WARN_ON_ONCE(!pfn_valid(pfn)))
		return -1;

	if (pfn < pgdat->node_start_pfn || pfn >= pgdat_end_pfn(pgdat))
		return -1;

	return pfn;
}

static unsigned long get_pmd_pfn(pmd_t pmd, struct vm_area_struct *vma, unsigned long addr,
				 struct pglist_data *pgdat)
{
	unsigned long pfn = pmd_pfn(pmd);

	VM_WARN_ON_ONCE(addr < vma->vm_start || addr >= vma->vm_end);

	if (!pmd_present(pmd) || is_huge_zero_pmd(pmd))
		return -1;

	if (!pmd_young(pmd) && !mm_has_notifiers(vma->vm_mm))
		return -1;

	if (WARN_ON_ONCE(!pfn_valid(pfn)))
		return -1;

	if (pfn < pgdat->node_start_pfn || pfn >= pgdat_end_pfn(pgdat))
		return -1;

	return pfn;
}

static struct folio *get_pfn_folio(unsigned long pfn, struct mem_cgroup *memcg,
				   struct pglist_data *pgdat)
{
	struct folio *folio = pfn_folio(pfn);

	if (folio_lru_gen(folio) < 0)
		return NULL;

	if (folio_nid(folio) != pgdat->node_id)
		return NULL;

	if (folio_memcg(folio) != memcg)
		return NULL;

	return folio;
}

static bool suitable_to_scan(int total, int young)
{
	int n = clamp_t(int, cache_line_size() / sizeof(pte_t), 2, 8);

	/* suitable if the average number of young PTEs per cacheline is >=1 */
	return young * n >= total;
}

/*
 * Only used on a mapped folio in the eviction (rmap walk) path, where promotion
 * needs to be done by taking the folio off the LRU list and then adding it back
 * with PG_active set. In contrast, the aging (page table walk) path uses
 * folio_update_gen().
 */
static bool lru_gen_set_refs(struct folio *folio)
{
	/* see the comment on LRU_REFS_FLAGS */
	if (!folio_test_referenced(folio) && !folio_test_workingset(folio)) {
		set_mask_bits(&folio->flags.f, LRU_REFS_MASK, BIT(PG_referenced));
		return false;
	}

	set_mask_bits(&folio->flags.f, LRU_REFS_FLAGS, BIT(PG_workingset));
	return true;
}

enum folio_references lru_gen_folio_check_references(struct folio *folio,
						  struct scan_control *sc)
{
	int referenced_ptes;
	vm_flags_t vm_flags;

	referenced_ptes = folio_referenced(folio, 1, sc->target_mem_cgroup,
					   &vm_flags);

	/*
	 * The supposedly reclaimable folio was found to be in a VM_LOCKED vma.
	 * Let the folio, now marked Mlocked, be moved to the unevictable list.
	 */
	if (vm_flags & VM_LOCKED)
		return FOLIOREF_ACTIVATE;

	/*
	 * There are two cases to consider.
	 * 1) Rmap lock contention: rotate.
	 * 2) Skip the non-shared swapbacked folio mapped solely by
	 *    the exiting or OOM-reaped process.
	 */
	if (referenced_ptes == -1)
		return FOLIOREF_KEEP;

	if (!referenced_ptes)
		return FOLIOREF_RECLAIM;

	return lru_gen_set_refs(folio) ? FOLIOREF_ACTIVATE : FOLIOREF_KEEP;
}

static void walk_update_folio(struct lru_gen_mm_walk *walk, struct folio *folio,
			      int new_gen, bool dirty)
{
	int old_gen;

	if (!folio)
		return;

	if (dirty && !folio_test_dirty(folio) &&
	    !(folio_test_anon(folio) && folio_test_swapbacked(folio) &&
	      !folio_test_swapcache(folio)))
		folio_mark_dirty(folio);

	if (walk) {
		old_gen = folio_update_gen(folio, new_gen);
		if (old_gen >= 0 && old_gen != new_gen)
			update_batch_size(walk, folio, old_gen, new_gen);
	} else if (lru_gen_set_refs(folio)) {
		old_gen = folio_lru_gen(folio);
		if (old_gen >= 0 && old_gen != new_gen)
			folio_activate(folio);
	}
}

static bool walk_pte_range(pmd_t *pmd, unsigned long start, unsigned long end,
			   struct mm_walk *args)
{
	int i;
	bool dirty;
	pte_t *pte;
	spinlock_t *ptl;
	unsigned long addr;
	int total = 0;
	int young = 0;
	struct folio *last = NULL;
	struct lru_gen_mm_walk *walk = args->private;
	struct mem_cgroup *memcg = lruvec_memcg(walk->lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(walk->lruvec);
	DEFINE_MAX_SEQ(walk->lruvec);
	int gen = lru_gen_from_seq(max_seq);
	pmd_t pmdval;

	pte = pte_offset_map_rw_nolock(args->mm, pmd, start & PMD_MASK, &pmdval, &ptl);
	if (!pte)
		return false;

	if (!spin_trylock(ptl)) {
		pte_unmap(pte);
		return true;
	}

	if (unlikely(!pmd_same(pmdval, pmdp_get_lockless(pmd)))) {
		pte_unmap_unlock(pte, ptl);
		return false;
	}

	lazy_mmu_mode_enable();
restart:
	for (i = pte_index(start), addr = start; addr != end; i++, addr += PAGE_SIZE) {
		unsigned long pfn;
		struct folio *folio;
		pte_t ptent = ptep_get(pte + i);

		total++;
		walk->mm_stats[MM_LEAF_TOTAL]++;

		pfn = get_pte_pfn(ptent, args->vma, addr, pgdat);
		if (pfn == -1)
			continue;

		folio = get_pfn_folio(pfn, memcg, pgdat);
		if (!folio)
			continue;

		if (!ptep_clear_young_notify(args->vma, addr, pte + i))
			continue;

		if (last != folio) {
			walk_update_folio(walk, last, gen, dirty);

			last = folio;
			dirty = false;
		}

		if (pte_dirty(ptent))
			dirty = true;

		young++;
		walk->mm_stats[MM_LEAF_YOUNG]++;
	}

	walk_update_folio(walk, last, gen, dirty);
	last = NULL;

	if (i < PTRS_PER_PTE && get_next_vma(PMD_MASK, PAGE_SIZE, args, &start, &end))
		goto restart;

	lazy_mmu_mode_disable();
	pte_unmap_unlock(pte, ptl);

	return suitable_to_scan(total, young);
}

static void walk_pmd_range_locked(pud_t *pud, unsigned long addr, struct vm_area_struct *vma,
				  struct mm_walk *args, unsigned long *bitmap, unsigned long *first)
{
	int i;
	bool dirty;
	pmd_t *pmd;
	spinlock_t *ptl;
	struct folio *last = NULL;
	struct lru_gen_mm_walk *walk = args->private;
	struct mem_cgroup *memcg = lruvec_memcg(walk->lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(walk->lruvec);
	DEFINE_MAX_SEQ(walk->lruvec);
	int gen = lru_gen_from_seq(max_seq);

	VM_WARN_ON_ONCE(pud_leaf(*pud));

	/* try to batch at most 1+MIN_LRU_BATCH+1 entries */
	if (*first == -1) {
		*first = addr;
		bitmap_zero(bitmap, MIN_LRU_BATCH);
		return;
	}

	i = addr == -1 ? 0 : pmd_index(addr) - pmd_index(*first);
	if (i && i <= MIN_LRU_BATCH) {
		__set_bit(i - 1, bitmap);
		return;
	}

	pmd = pmd_offset(pud, *first);

	ptl = pmd_lockptr(args->mm, pmd);
	if (!spin_trylock(ptl))
		goto done;

	lazy_mmu_mode_enable();

	do {
		unsigned long pfn;
		struct folio *folio;

		/* don't round down the first address */
		addr = i ? (*first & PMD_MASK) + i * PMD_SIZE : *first;

		if (!pmd_present(pmd[i]))
			goto next;

		if (!pmd_trans_huge(pmd[i])) {
			if (!walk->force_scan && should_clear_pmd_young() &&
			    !mm_has_notifiers(args->mm))
				pmdp_test_and_clear_young(vma, addr, pmd + i);
			goto next;
		}

		pfn = get_pmd_pfn(pmd[i], vma, addr, pgdat);
		if (pfn == -1)
			goto next;

		folio = get_pfn_folio(pfn, memcg, pgdat);
		if (!folio)
			goto next;

		if (!pmdp_clear_young_notify(vma, addr, pmd + i))
			goto next;

		if (last != folio) {
			walk_update_folio(walk, last, gen, dirty);

			last = folio;
			dirty = false;
		}

		if (pmd_dirty(pmd[i]))
			dirty = true;

		walk->mm_stats[MM_LEAF_YOUNG]++;
next:
		i = i > MIN_LRU_BATCH ? 0 : find_next_bit(bitmap, MIN_LRU_BATCH, i) + 1;
	} while (i <= MIN_LRU_BATCH);

	walk_update_folio(walk, last, gen, dirty);

	lazy_mmu_mode_disable();
	spin_unlock(ptl);
done:
	*first = -1;
}

static void walk_pmd_range(pud_t *pud, unsigned long start, unsigned long end,
			   struct mm_walk *args)
{
	int i;
	pmd_t *pmd;
	unsigned long next;
	unsigned long addr;
	struct vm_area_struct *vma;
	DECLARE_BITMAP(bitmap, MIN_LRU_BATCH);
	unsigned long first = -1;
	struct lru_gen_mm_walk *walk = args->private;
	struct lru_gen_mm_state *mm_state = get_mm_state(walk->lruvec);

	VM_WARN_ON_ONCE(pud_leaf(*pud));

	/*
	 * Finish an entire PMD in two passes: the first only reaches to PTE
	 * tables to avoid taking the PMD lock; the second, if necessary, takes
	 * the PMD lock to clear the accessed bit in PMD entries.
	 */
	pmd = pmd_offset(pud, start & PUD_MASK);
restart:
	/* walk_pte_range() may call get_next_vma() */
	vma = args->vma;
	for (i = pmd_index(start), addr = start; addr != end; i++, addr = next) {
		pmd_t val = pmdp_get_lockless(pmd + i);

		next = pmd_addr_end(addr, end);

		if (!pmd_present(val) || is_huge_zero_pmd(val)) {
			walk->mm_stats[MM_LEAF_TOTAL]++;
			continue;
		}

		if (pmd_trans_huge(val)) {
			struct pglist_data *pgdat = lruvec_pgdat(walk->lruvec);
			unsigned long pfn = get_pmd_pfn(val, vma, addr, pgdat);

			walk->mm_stats[MM_LEAF_TOTAL]++;

			if (pfn != -1)
				walk_pmd_range_locked(pud, addr, vma, args, bitmap, &first);
			continue;
		}

		if (!walk->force_scan && should_clear_pmd_young() &&
		    !mm_has_notifiers(args->mm)) {
			if (!pmd_young(val))
				continue;

			walk_pmd_range_locked(pud, addr, vma, args, bitmap, &first);
		}

		if (!walk->force_scan && !test_bloom_filter(mm_state, walk->seq, pmd + i))
			continue;

		walk->mm_stats[MM_NONLEAF_FOUND]++;

		if (!walk_pte_range(&val, addr, next, args))
			continue;

		walk->mm_stats[MM_NONLEAF_ADDED]++;

		/* carry over to the next generation */
		update_bloom_filter(mm_state, walk->seq + 1, pmd + i);
	}

	walk_pmd_range_locked(pud, -1, vma, args, bitmap, &first);

	if (i < PTRS_PER_PMD && get_next_vma(PUD_MASK, PMD_SIZE, args, &start, &end))
		goto restart;
}

static int walk_pud_range(p4d_t *p4d, unsigned long start, unsigned long end,
			  struct mm_walk *args)
{
	int i;
	pud_t *pud;
	unsigned long addr;
	unsigned long next;
	struct lru_gen_mm_walk *walk = args->private;

	VM_WARN_ON_ONCE(p4d_leaf(*p4d));

	pud = pud_offset(p4d, start & P4D_MASK);
restart:
	for (i = pud_index(start), addr = start; addr != end; i++, addr = next) {
		pud_t val = pudp_get(pud + i);

		next = pud_addr_end(addr, end);

		if (!pud_present(val) || WARN_ON_ONCE(pud_leaf(val)))
			continue;

		walk_pmd_range(&val, addr, next, args);

		if (need_resched() || walk->batched >= MAX_LRU_BATCH) {
			end = (addr | ~PUD_MASK) + 1;
			goto done;
		}
	}

	if (i < PTRS_PER_PUD && get_next_vma(P4D_MASK, PUD_SIZE, args, &start, &end))
		goto restart;

	end = round_up(end, P4D_SIZE);
done:
	if (!end || !args->vma)
		return 1;

	walk->next_addr = max(end, args->vma->vm_start);

	return -EAGAIN;
}

static void walk_mm(struct mm_struct *mm, struct lru_gen_mm_walk *walk)
{
	static const struct mm_walk_ops mm_walk_ops = {
		.test_walk = should_skip_vma,
		.p4d_entry = walk_pud_range,
		.walk_lock = PGWALK_RDLOCK,
	};
	int err;
	struct lruvec *lruvec = walk->lruvec;

	walk->next_addr = FIRST_USER_ADDRESS;

	do {
		DEFINE_MAX_SEQ(lruvec);

		err = -EBUSY;

		/* another thread might have called inc_max_seq() */
		if (walk->seq != max_seq)
			break;

		/* the caller might be holding the lock for write */
		if (mmap_read_trylock(mm)) {
			err = walk_page_range(mm, walk->next_addr, ULONG_MAX, &mm_walk_ops, walk);

			mmap_read_unlock(mm);
		}

		if (walk->batched) {
			spin_lock_irq(&lruvec->lru_lock);
			reset_batch_size(walk);
			spin_unlock_irq(&lruvec->lru_lock);
		}

		cond_resched();
	} while (err == -EAGAIN);
}

static struct lru_gen_mm_walk *set_mm_walk(struct pglist_data *pgdat, bool force_alloc)
{
	struct lru_gen_mm_walk *walk = current->reclaim_state->mm_walk;

	if (pgdat && current_is_kswapd()) {
		VM_WARN_ON_ONCE(walk);

		walk = &pgdat->mm_walk;
	} else if (!walk && force_alloc) {
		VM_WARN_ON_ONCE(current_is_kswapd());

		walk = kzalloc_obj(*walk,
				   __GFP_HIGH | __GFP_NOMEMALLOC | __GFP_NOWARN);
	}

	current->reclaim_state->mm_walk = walk;

	return walk;
}

static void clear_mm_walk(void)
{
	struct lru_gen_mm_walk *walk = current->reclaim_state->mm_walk;

	VM_WARN_ON_ONCE(walk && memchr_inv(walk->nr_pages, 0, sizeof(walk->nr_pages)));
	VM_WARN_ON_ONCE(walk && memchr_inv(walk->mm_stats, 0, sizeof(walk->mm_stats)));

	current->reclaim_state->mm_walk = NULL;

	if (!current_is_kswapd())
		kfree(walk);
}

static bool inc_min_seq(struct lruvec *lruvec, int type, int swappiness)
{
	int zone;
	int remaining = MAX_LRU_BATCH;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	int hist = lru_hist_from_seq(lrugen->min_seq[type]);
	int new_gen, old_gen = lru_gen_from_seq(lrugen->min_seq[type]);

	/* For file type, skip the check if swappiness is anon only */
	if (type && (swappiness == SWAPPINESS_ANON_ONLY))
		goto done;

	/* For anon type, skip the check if swappiness is zero (file only) */
	if (!type && !swappiness)
		goto done;

	/* prevent cold/hot inversion if the type is evictable */
	for (zone = 0; zone < MAX_NR_ZONES; zone++) {
		struct list_head *head = &lrugen->folios[old_gen][type][zone];

		while (!list_empty(head)) {
			struct folio *folio = lru_to_folio(head);
			int refs = folio_lru_refs(folio);
			bool workingset = folio_test_workingset(folio);

			VM_WARN_ON_ONCE_FOLIO(folio_test_unevictable(folio), folio);
			VM_WARN_ON_ONCE_FOLIO(folio_test_active(folio), folio);
			VM_WARN_ON_ONCE_FOLIO(folio_is_file_lru(folio) != type, folio);
			VM_WARN_ON_ONCE_FOLIO(folio_zonenum(folio) != zone, folio);

			new_gen = folio_inc_gen(lruvec, folio, false);
			list_move_tail(&folio->lru, &lrugen->folios[new_gen][type][zone]);

			/* don't count the workingset being lazily promoted */
			if (refs + workingset != BIT(LRU_REFS_WIDTH) + 1) {
				int tier = lru_tier_from_refs(refs, workingset);
				int delta = folio_nr_pages(folio);

				WRITE_ONCE(lrugen->protected[hist][type][tier],
					   lrugen->protected[hist][type][tier] + delta);
			}

			if (!--remaining)
				return false;
		}
	}
done:
	reset_ctrl_pos(lruvec, type, true);
	WRITE_ONCE(lrugen->min_seq[type], lrugen->min_seq[type] + 1);

	return true;
}

static bool try_to_inc_min_seq(struct lruvec *lruvec, int swappiness)
{
	int gen, type, zone;
	bool success = false;
	bool seq_inc_flag = false;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	DEFINE_MIN_SEQ(lruvec);

	VM_WARN_ON_ONCE(!seq_is_valid(lruvec));

	/* find the oldest populated generation */
	for_each_evictable_type(type, swappiness) {
		while (min_seq[type] + MIN_NR_GENS <= lrugen->max_seq) {
			gen = lru_gen_from_seq(min_seq[type]);

			for (zone = 0; zone < MAX_NR_ZONES; zone++) {
				if (!list_empty(&lrugen->folios[gen][type][zone]))
					goto next;
			}

			min_seq[type]++;
			seq_inc_flag = true;
		}
next:
		;
	}

	/*
	 * If min_seq[type] of both anonymous and file is not increased,
	 * we can directly return false to avoid unnecessary checking
	 * overhead later.
	 */
	if (!seq_inc_flag)
		return success;

	/* see the comment on lru_gen_folio */
	if (swappiness && swappiness <= MAX_SWAPPINESS) {
		unsigned long seq = lrugen->max_seq - MIN_NR_GENS;

		if (min_seq[LRU_GEN_ANON] > seq && min_seq[LRU_GEN_FILE] < seq)
			min_seq[LRU_GEN_ANON] = seq;
		else if (min_seq[LRU_GEN_FILE] > seq && min_seq[LRU_GEN_ANON] < seq)
			min_seq[LRU_GEN_FILE] = seq;
	}

	for_each_evictable_type(type, swappiness) {
		if (min_seq[type] <= lrugen->min_seq[type])
			continue;

		reset_ctrl_pos(lruvec, type, true);
		WRITE_ONCE(lrugen->min_seq[type], min_seq[type]);
		success = true;
	}

	return success;
}

static bool inc_max_seq(struct lruvec *lruvec, unsigned long seq, int swappiness)
{
	bool success;
	int prev, next;
	int type, zone;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
restart:
	if (seq < READ_ONCE(lrugen->max_seq))
		return false;

	spin_lock_irq(&lruvec->lru_lock);

	VM_WARN_ON_ONCE(!seq_is_valid(lruvec));

	success = seq == lrugen->max_seq;
	if (!success)
		goto unlock;

	for (type = 0; type < ANON_AND_FILE; type++) {
		if (get_nr_gens(lruvec, type) != MAX_NR_GENS)
			continue;

		if (inc_min_seq(lruvec, type, swappiness))
			continue;

		spin_unlock_irq(&lruvec->lru_lock);
		cond_resched();
		goto restart;
	}

	/*
	 * Update the active/inactive LRU sizes for compatibility. Both sides of
	 * the current max_seq need to be covered, since max_seq+1 can overlap
	 * with min_seq[LRU_GEN_ANON] if swapping is constrained. And if they do
	 * overlap, cold/hot inversion happens.
	 */
	prev = lru_gen_from_seq(lrugen->max_seq - 1);
	next = lru_gen_from_seq(lrugen->max_seq + 1);

	for (type = 0; type < ANON_AND_FILE; type++) {
		for (zone = 0; zone < MAX_NR_ZONES; zone++) {
			enum lru_list lru = type * LRU_INACTIVE_FILE;
			long delta = lrugen->nr_pages[prev][type][zone] -
				     lrugen->nr_pages[next][type][zone];

			if (!delta)
				continue;

			__update_lru_size(lruvec, lru, zone, delta);
			__update_lru_size(lruvec, lru + LRU_ACTIVE, zone, -delta);
		}
	}

	for (type = 0; type < ANON_AND_FILE; type++)
		reset_ctrl_pos(lruvec, type, false);

	WRITE_ONCE(lrugen->timestamps[next], jiffies);
	/* make sure preceding modifications appear */
	smp_store_release(&lrugen->max_seq, lrugen->max_seq + 1);
unlock:
	spin_unlock_irq(&lruvec->lru_lock);

	return success;
}

static bool try_to_inc_max_seq(struct lruvec *lruvec, unsigned long seq,
			       int swappiness, bool force_scan)
{
	bool success;
	struct lru_gen_mm_walk *walk;
	struct mm_struct *mm = NULL;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	struct lru_gen_mm_state *mm_state = get_mm_state(lruvec);

	VM_WARN_ON_ONCE(seq > READ_ONCE(lrugen->max_seq));

	if (!mm_state)
		return inc_max_seq(lruvec, seq, swappiness);

	/* see the comment in iterate_mm_list() */
	if (seq <= READ_ONCE(mm_state->seq))
		return false;

	/*
	 * If the hardware doesn't automatically set the accessed bit, fallback
	 * to lru_gen_look_around(), which only clears the accessed bit in a
	 * handful of PTEs. Spreading the work out over a period of time usually
	 * is less efficient, but it avoids bursty page faults.
	 */
	if (!should_walk_mmu()) {
		success = iterate_mm_list_nowalk(lruvec, seq);
		goto done;
	}

	walk = set_mm_walk(NULL, true);
	if (!walk) {
		success = iterate_mm_list_nowalk(lruvec, seq);
		goto done;
	}

	walk->lruvec = lruvec;
	walk->seq = seq;
	walk->swappiness = swappiness;
	walk->force_scan = force_scan;

	do {
		success = iterate_mm_list(walk, &mm);
		if (mm)
			walk_mm(mm, walk);
	} while (mm);
done:
	if (success) {
		success = inc_max_seq(lruvec, seq, swappiness);
		WARN_ON_ONCE(!success);
	}

	return success;
}

/******************************************************************************
 *                          working set protection
 ******************************************************************************/

static void set_initial_priority(struct pglist_data *pgdat, struct scan_control *sc)
{
	int priority;
	unsigned long reclaimable;

	if (sc->priority != DEF_PRIORITY || sc->nr_to_reclaim < MIN_LRU_BATCH)
		return;
	/*
	 * Determine the initial priority based on
	 * (total >> priority) * reclaimed_to_scanned_ratio = nr_to_reclaim,
	 * where reclaimed_to_scanned_ratio = inactive / total.
	 */
	reclaimable = node_page_state(pgdat, NR_INACTIVE_FILE);
	if (can_reclaim_anon_pages(NULL, pgdat->node_id, sc))
		reclaimable += node_page_state(pgdat, NR_INACTIVE_ANON);

	/* round down reclaimable and round up sc->nr_to_reclaim */
	priority = fls_long(reclaimable) - 1 - fls_long(sc->nr_to_reclaim - 1);

	/*
	 * The estimation is based on LRU pages only, so cap it to prevent
	 * overshoots of shrinker objects by large margins.
	 */
	sc->priority = clamp(priority, DEF_PRIORITY / 2, DEF_PRIORITY);
}

static bool lruvec_is_sizable(struct lruvec *lruvec, struct scan_control *sc)
{
	int gen, type, zone;
	unsigned long total = 0;
	int swappiness = get_swappiness(lruvec, sc);
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	for_each_evictable_type(type, swappiness) {
		unsigned long seq;

		for (seq = min_seq[type]; seq <= max_seq; seq++) {
			gen = lru_gen_from_seq(seq);

			for (zone = 0; zone < MAX_NR_ZONES; zone++)
				total += max(READ_ONCE(lrugen->nr_pages[gen][type][zone]), 0L);
		}
	}

	/* whether the size is big enough to be helpful */
	return mem_cgroup_online(memcg) ? (total >> sc->priority) : total;
}

static bool lruvec_is_reclaimable(struct lruvec *lruvec, struct scan_control *sc,
				  unsigned long min_ttl)
{
	int gen;
	unsigned long birth;
	int swappiness = get_swappiness(lruvec, sc);
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	if (mem_cgroup_below_min(NULL, memcg))
		return false;

	if (!lruvec_is_sizable(lruvec, sc))
		return false;

	gen = lru_gen_from_seq(evictable_min_seq(min_seq, swappiness));
	birth = READ_ONCE(lruvec->lrugen.timestamps[gen]);

	return time_is_before_jiffies(birth + min_ttl);
}

/* to protect the working set of the last N jiffies */
static unsigned long lru_gen_min_ttl __read_mostly;

void lru_gen_age_node(struct pglist_data *pgdat, struct scan_control *sc)
{
	struct mem_cgroup *memcg;
	unsigned long min_ttl = READ_ONCE(lru_gen_min_ttl);
	bool reclaimable = !min_ttl;

	VM_WARN_ON_ONCE(!current_is_kswapd());

	set_initial_priority(pgdat, sc);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);

		mem_cgroup_calculate_protection(NULL, memcg);

		if (!reclaimable)
			reclaimable = lruvec_is_reclaimable(lruvec, sc, min_ttl);
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));

	/*
	 * The main goal is to OOM kill if every generation from all memcgs is
	 * younger than min_ttl. However, another possibility is all memcgs are
	 * either too small or below min.
	 */
	if (!reclaimable && mutex_trylock(&oom_lock)) {
		struct oom_control oc = {
			.gfp_mask = sc->gfp_mask,
		};

		out_of_memory(&oc);

		mutex_unlock(&oom_lock);
	}
}

/******************************************************************************
 *                          rmap/PT walk feedback
 ******************************************************************************/

/*
 * This function exploits spatial locality when shrink_folio_list() walks the
 * rmap. It scans the adjacent PTEs of a young PTE and promotes hot pages. If
 * the scan was done cacheline efficiently, it adds the PMD entry pointing to
 * the PTE table to the Bloom filter. This forms a feedback loop between the
 * eviction and the aging.
 */
bool lru_gen_look_around(struct page_vma_mapped_walk *pvmw)
{
	int i;
	bool dirty;
	unsigned long start;
	unsigned long end;
	struct lru_gen_mm_walk *walk;
	struct folio *last = NULL;
	int young = 1;
	pte_t *pte = pvmw->pte;
	unsigned long addr = pvmw->address;
	struct vm_area_struct *vma = pvmw->vma;
	struct folio *folio = pfn_folio(pvmw->pfn);
	struct mem_cgroup *memcg = folio_memcg(folio);
	struct pglist_data *pgdat = folio_pgdat(folio);
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
	struct lru_gen_mm_state *mm_state = get_mm_state(lruvec);
	DEFINE_MAX_SEQ(lruvec);
	int gen = lru_gen_from_seq(max_seq);

	lockdep_assert_held(pvmw->ptl);
	VM_WARN_ON_ONCE_FOLIO(folio_test_lru(folio), folio);

	if (!ptep_clear_young_notify(vma, addr, pte))
		return false;

	if (spin_is_contended(pvmw->ptl))
		return true;

	/* exclude special VMAs containing anon pages from COW */
	if (vma->vm_flags & VM_SPECIAL)
		return true;

	/* avoid taking the LRU lock under the PTL when possible */
	walk = current->reclaim_state ? current->reclaim_state->mm_walk : NULL;

	start = max(addr & PMD_MASK, vma->vm_start);
	end = min(addr | ~PMD_MASK, vma->vm_end - 1) + 1;

	if (end - start == PAGE_SIZE)
		return true;

	if (end - start > MIN_LRU_BATCH * PAGE_SIZE) {
		if (addr - start < MIN_LRU_BATCH * PAGE_SIZE / 2)
			end = start + MIN_LRU_BATCH * PAGE_SIZE;
		else if (end - addr < MIN_LRU_BATCH * PAGE_SIZE / 2)
			start = end - MIN_LRU_BATCH * PAGE_SIZE;
		else {
			start = addr - MIN_LRU_BATCH * PAGE_SIZE / 2;
			end = addr + MIN_LRU_BATCH * PAGE_SIZE / 2;
		}
	}

	lazy_mmu_mode_enable();

	pte -= (addr - start) / PAGE_SIZE;

	for (i = 0, addr = start; addr != end; i++, addr += PAGE_SIZE) {
		unsigned long pfn;
		pte_t ptent = ptep_get(pte + i);

		pfn = get_pte_pfn(ptent, vma, addr, pgdat);
		if (pfn == -1)
			continue;

		folio = get_pfn_folio(pfn, memcg, pgdat);
		if (!folio)
			continue;

		if (!ptep_clear_young_notify(vma, addr, pte + i))
			continue;

		if (last != folio) {
			walk_update_folio(walk, last, gen, dirty);

			last = folio;
			dirty = false;
		}

		if (pte_dirty(ptent))
			dirty = true;

		young++;
	}

	walk_update_folio(walk, last, gen, dirty);

	lazy_mmu_mode_disable();

	/* feedback from rmap walkers to page table walkers */
	if (mm_state && suitable_to_scan(i, young))
		update_bloom_filter(mm_state, max_seq, pvmw->pmd);

	return true;
}

/******************************************************************************
 *                          memcg LRU
 ******************************************************************************/

/* see the comment on MEMCG_NR_GENS */
enum {
	MEMCG_LRU_NOP,
	MEMCG_LRU_HEAD,
	MEMCG_LRU_TAIL,
	MEMCG_LRU_OLD,
	MEMCG_LRU_YOUNG,
};

static void lru_gen_rotate_memcg(struct lruvec *lruvec, int op)
{
	int seg;
	int old, new;
	unsigned long flags;
	int bin = get_random_u32_below(MEMCG_NR_BINS);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	spin_lock_irqsave(&pgdat->memcg_lru.lock, flags);

	VM_WARN_ON_ONCE(hlist_nulls_unhashed(&lruvec->lrugen.list));

	seg = 0;
	new = old = lruvec->lrugen.gen;

	/* see the comment on MEMCG_NR_GENS */
	if (op == MEMCG_LRU_HEAD)
		seg = MEMCG_LRU_HEAD;
	else if (op == MEMCG_LRU_TAIL)
		seg = MEMCG_LRU_TAIL;
	else if (op == MEMCG_LRU_OLD)
		new = get_memcg_gen(pgdat->memcg_lru.seq);
	else if (op == MEMCG_LRU_YOUNG)
		new = get_memcg_gen(pgdat->memcg_lru.seq + 1);
	else
		VM_WARN_ON_ONCE(true);

	WRITE_ONCE(lruvec->lrugen.seg, seg);
	WRITE_ONCE(lruvec->lrugen.gen, new);

	hlist_nulls_del_rcu(&lruvec->lrugen.list);

	if (op == MEMCG_LRU_HEAD || op == MEMCG_LRU_OLD)
		hlist_nulls_add_head_rcu(&lruvec->lrugen.list, &pgdat->memcg_lru.fifo[new][bin]);
	else
		hlist_nulls_add_tail_rcu(&lruvec->lrugen.list, &pgdat->memcg_lru.fifo[new][bin]);

	pgdat->memcg_lru.nr_memcgs[old]--;
	pgdat->memcg_lru.nr_memcgs[new]++;

	if (!pgdat->memcg_lru.nr_memcgs[old] && old == get_memcg_gen(pgdat->memcg_lru.seq))
		WRITE_ONCE(pgdat->memcg_lru.seq, pgdat->memcg_lru.seq + 1);

	spin_unlock_irqrestore(&pgdat->memcg_lru.lock, flags);
}

#ifdef CONFIG_MEMCG

void lru_gen_online_memcg(struct mem_cgroup *memcg)
{
	int gen;
	int nid;
	int bin = get_random_u32_below(MEMCG_NR_BINS);

	for_each_node(nid) {
		struct pglist_data *pgdat = NODE_DATA(nid);
		struct lruvec *lruvec = get_lruvec(memcg, nid);

		spin_lock_irq(&pgdat->memcg_lru.lock);

		VM_WARN_ON_ONCE(!hlist_nulls_unhashed(&lruvec->lrugen.list));

		gen = get_memcg_gen(pgdat->memcg_lru.seq);

		lruvec->lrugen.gen = gen;

		hlist_nulls_add_tail_rcu(&lruvec->lrugen.list, &pgdat->memcg_lru.fifo[gen][bin]);
		pgdat->memcg_lru.nr_memcgs[gen]++;

		spin_unlock_irq(&pgdat->memcg_lru.lock);
	}
}

void lru_gen_offline_memcg(struct mem_cgroup *memcg)
{
	int nid;

	for_each_node(nid) {
		struct lruvec *lruvec = get_lruvec(memcg, nid);

		lru_gen_rotate_memcg(lruvec, MEMCG_LRU_OLD);
	}
}

void lru_gen_release_memcg(struct mem_cgroup *memcg)
{
	int gen;
	int nid;

	for_each_node(nid) {
		struct pglist_data *pgdat = NODE_DATA(nid);
		struct lruvec *lruvec = get_lruvec(memcg, nid);

		spin_lock_irq(&pgdat->memcg_lru.lock);

		if (hlist_nulls_unhashed(&lruvec->lrugen.list))
			goto unlock;

		gen = lruvec->lrugen.gen;

		hlist_nulls_del_init_rcu(&lruvec->lrugen.list);
		pgdat->memcg_lru.nr_memcgs[gen]--;

		if (!pgdat->memcg_lru.nr_memcgs[gen] && gen == get_memcg_gen(pgdat->memcg_lru.seq))
			WRITE_ONCE(pgdat->memcg_lru.seq, pgdat->memcg_lru.seq + 1);
unlock:
		spin_unlock_irq(&pgdat->memcg_lru.lock);
	}
}

void lru_gen_soft_reclaim(struct mem_cgroup *memcg, int nid)
{
	struct lruvec *lruvec = get_lruvec(memcg, nid);

	/* see the comment on MEMCG_NR_GENS */
	if (READ_ONCE(lruvec->lrugen.seg) != MEMCG_LRU_HEAD)
		lru_gen_rotate_memcg(lruvec, MEMCG_LRU_HEAD);
}

#endif /* CONFIG_MEMCG */

/******************************************************************************
 *                          the eviction
 ******************************************************************************/

static bool sort_folio(struct lruvec *lruvec, struct folio *folio, struct scan_control *sc,
		       int tier_idx)
{
	bool success;
	bool dirty, writeback;
	int gen = folio_lru_gen(folio);
	int type = folio_is_file_lru(folio);
	int zone = folio_zonenum(folio);
	int delta = folio_nr_pages(folio);
	int refs = folio_lru_refs(folio);
	bool workingset = folio_test_workingset(folio);
	int tier = lru_tier_from_refs(refs, workingset);
	struct lru_gen_folio *lrugen = &lruvec->lrugen;

	VM_WARN_ON_ONCE_FOLIO(gen >= MAX_NR_GENS, folio);

	/* unevictable */
	if (!folio_evictable(folio)) {
		success = lru_gen_del_folio(lruvec, folio, true);
		VM_WARN_ON_ONCE_FOLIO(!success, folio);
		folio_set_unevictable(folio);
		lruvec_add_folio(lruvec, folio);
		__count_vm_events(UNEVICTABLE_PGCULLED, delta);
		return true;
	}

	/* promoted */
	if (gen != lru_gen_from_seq(lrugen->min_seq[type])) {
		list_move(&folio->lru, &lrugen->folios[gen][type][zone]);
		return true;
	}

	/* protected */
	if (tier > tier_idx || refs + workingset == BIT(LRU_REFS_WIDTH) + 1) {
		gen = folio_inc_gen(lruvec, folio, false);
		list_move(&folio->lru, &lrugen->folios[gen][type][zone]);

		/* don't count the workingset being lazily promoted */
		if (refs + workingset != BIT(LRU_REFS_WIDTH) + 1) {
			int hist = lru_hist_from_seq(lrugen->min_seq[type]);

			WRITE_ONCE(lrugen->protected[hist][type][tier],
				   lrugen->protected[hist][type][tier] + delta);
		}
		return true;
	}

	/* ineligible */
	if (zone > sc->reclaim_idx) {
		gen = folio_inc_gen(lruvec, folio, false);
		list_move_tail(&folio->lru, &lrugen->folios[gen][type][zone]);
		return true;
	}

	dirty = folio_test_dirty(folio);
	writeback = folio_test_writeback(folio);
	if (type == LRU_GEN_FILE && dirty) {
		sc->nr.file_taken += delta;
		if (!writeback)
			sc->nr.unqueued_dirty += delta;
	}

	/* waiting for writeback */
	if (writeback || (type == LRU_GEN_FILE && dirty)) {
		gen = folio_inc_gen(lruvec, folio, true);
		list_move(&folio->lru, &lrugen->folios[gen][type][zone]);
		return true;
	}

	return false;
}

static bool isolate_folio(struct lruvec *lruvec, struct folio *folio, struct scan_control *sc)
{
	bool success;

	/* swap constrained */
	if (!(sc->gfp_mask & __GFP_IO) &&
	    (folio_test_dirty(folio) ||
	     (folio_test_anon(folio) && !folio_test_swapcache(folio))))
		return false;

	/* raced with release_pages() */
	if (!folio_try_get(folio))
		return false;

	/* raced with another isolation */
	if (!folio_test_clear_lru(folio)) {
		folio_put(folio);
		return false;
	}

	/* see the comment on LRU_REFS_FLAGS */
	if (!folio_test_referenced(folio))
		set_mask_bits(&folio->flags.f, LRU_REFS_MASK, 0);

	/* for shrink_folio_list() */
	folio_clear_reclaim(folio);

	success = lru_gen_del_folio(lruvec, folio, true);
	VM_WARN_ON_ONCE_FOLIO(!success, folio);

	return true;
}

static int scan_folios(unsigned long nr_to_scan, struct lruvec *lruvec,
		       struct scan_control *sc, int type, int tier,
		       struct list_head *list)
{
	int i;
	int gen;
	enum vm_event_item item;
	int sorted = 0;
	int scanned = 0;
	int isolated = 0;
	int skipped = 0;
	int scan_batch = min(nr_to_scan, MAX_LRU_BATCH);
	int remaining = scan_batch;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);

	VM_WARN_ON_ONCE(!list_empty(list));

	if (get_nr_gens(lruvec, type) == MIN_NR_GENS)
		return 0;

	gen = lru_gen_from_seq(lrugen->min_seq[type]);

	for (i = MAX_NR_ZONES; i > 0; i--) {
		LIST_HEAD(moved);
		int skipped_zone = 0;
		int zone = (sc->reclaim_idx + i) % MAX_NR_ZONES;
		struct list_head *head = &lrugen->folios[gen][type][zone];

		while (!list_empty(head)) {
			struct folio *folio = lru_to_folio(head);
			int delta = folio_nr_pages(folio);

			VM_WARN_ON_ONCE_FOLIO(folio_test_unevictable(folio), folio);
			VM_WARN_ON_ONCE_FOLIO(folio_test_active(folio), folio);
			VM_WARN_ON_ONCE_FOLIO(folio_is_file_lru(folio) != type, folio);
			VM_WARN_ON_ONCE_FOLIO(folio_zonenum(folio) != zone, folio);

			scanned += delta;

			if (sort_folio(lruvec, folio, sc, tier))
				sorted += delta;
			else if (isolate_folio(lruvec, folio, sc)) {
				list_add(&folio->lru, list);
				isolated += delta;
			} else {
				list_move(&folio->lru, &moved);
				skipped_zone += delta;
			}

			if (!--remaining || max(isolated, skipped_zone) >= MIN_LRU_BATCH)
				break;
		}

		if (skipped_zone) {
			list_splice(&moved, head);
			__count_zid_vm_events(PGSCAN_SKIP, zone, skipped_zone);
			skipped += skipped_zone;
		}

		if (!remaining || isolated >= MIN_LRU_BATCH)
			break;
	}

	item = PGSCAN_KSWAPD + reclaimer_offset(sc);
	if (!cgroup_reclaim(sc)) {
		__count_vm_events(item, isolated);
		__count_vm_events(PGREFILL, sorted);
	}
	count_memcg_events(memcg, item, isolated);
	count_memcg_events(memcg, PGREFILL, sorted);
	__count_vm_events(PGSCAN_ANON + type, isolated);
	trace_mm_vmscan_lru_isolate(sc->reclaim_idx, sc->order, scan_batch,
				scanned, skipped, isolated,
				type ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON);
	if (type == LRU_GEN_FILE)
		sc->nr.file_taken += isolated;
	/*
	 * There might not be eligible folios due to reclaim_idx. Check the
	 * remaining to prevent livelock if it's not making progress.
	 */
	return isolated || !remaining ? scanned : 0;
}

static int get_tier_idx(struct lruvec *lruvec, int type)
{
	int tier;
	struct ctrl_pos sp, pv;

	/*
	 * To leave a margin for fluctuations, use a larger gain factor (2:3).
	 * This value is chosen because any other tier would have at least twice
	 * as many refaults as the first tier.
	 */
	read_ctrl_pos(lruvec, type, 0, 2, &sp);
	for (tier = 1; tier < MAX_NR_TIERS; tier++) {
		read_ctrl_pos(lruvec, type, tier, 3, &pv);
		if (!positive_ctrl_err(&sp, &pv))
			break;
	}

	return tier - 1;
}

static int get_type_to_scan(struct lruvec *lruvec, int swappiness)
{
	struct ctrl_pos sp, pv;

	if (swappiness <= MIN_SWAPPINESS + 1)
		return LRU_GEN_FILE;

	if (swappiness >= MAX_SWAPPINESS)
		return LRU_GEN_ANON;
	/*
	 * Compare the sum of all tiers of anon with that of file to determine
	 * which type to scan.
	 */
	read_ctrl_pos(lruvec, LRU_GEN_ANON, MAX_NR_TIERS, swappiness, &sp);
	read_ctrl_pos(lruvec, LRU_GEN_FILE, MAX_NR_TIERS, MAX_SWAPPINESS - swappiness, &pv);

	return positive_ctrl_err(&sp, &pv);
}

static int isolate_folios(unsigned long nr_to_scan, struct lruvec *lruvec,
			  struct scan_control *sc, int swappiness,
			  int *type_scanned, struct list_head *list)
{
	int i;
	int type = get_type_to_scan(lruvec, swappiness);

	for_each_evictable_type(i, swappiness) {
		int scanned;
		int tier = get_tier_idx(lruvec, type);

		*type_scanned = type;

		scanned = scan_folios(nr_to_scan, lruvec, sc, type, tier, list);
		if (scanned)
			return scanned;

		type = !type;
	}

	return 0;
}

static int evict_folios(unsigned long nr_to_scan, struct lruvec *lruvec,
			struct scan_control *sc, int swappiness)
{
	int type;
	int scanned;
	int reclaimed;
	LIST_HEAD(list);
	LIST_HEAD(clean);
	struct folio *folio;
	struct folio *next;
	enum vm_event_item item;
	struct reclaim_stat stat;
	struct lru_gen_mm_walk *walk;
	bool skip_retry = false;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	spin_lock_irq(&lruvec->lru_lock);

	scanned = isolate_folios(nr_to_scan, lruvec, sc, swappiness, &type, &list);

	scanned += try_to_inc_min_seq(lruvec, swappiness);

	if (evictable_min_seq(lrugen->min_seq, swappiness) + MIN_NR_GENS > lrugen->max_seq)
		scanned = 0;

	spin_unlock_irq(&lruvec->lru_lock);

	if (list_empty(&list))
		return scanned;
retry:
	reclaimed = shrink_folio_list(&list, pgdat, sc, &stat, false, memcg);
	sc->nr.unqueued_dirty += stat.nr_unqueued_dirty;
	sc->nr_reclaimed += reclaimed;
	trace_mm_vmscan_lru_shrink_inactive(pgdat->node_id,
			scanned, reclaimed, &stat, sc->priority,
			type ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON);

	list_for_each_entry_safe_reverse(folio, next, &list, lru) {
		DEFINE_MIN_SEQ(lruvec);

		if (!folio_evictable(folio)) {
			list_del(&folio->lru);
			folio_putback_lru(folio);
			continue;
		}

		/* retry folios that may have missed folio_rotate_reclaimable() */
		if (!skip_retry && !folio_test_active(folio) && !folio_mapped(folio) &&
		    !folio_test_dirty(folio) && !folio_test_writeback(folio)) {
			list_move(&folio->lru, &clean);
			continue;
		}

		/* don't add rejected folios to the oldest generation */
		if (lru_gen_folio_seq(lruvec, folio, false) == min_seq[type])
			set_mask_bits(&folio->flags.f, LRU_REFS_FLAGS, BIT(PG_active));
	}

	spin_lock_irq(&lruvec->lru_lock);

	move_folios_to_lru(lruvec, &list);

	walk = current->reclaim_state->mm_walk;
	if (walk && walk->batched) {
		walk->lruvec = lruvec;
		reset_batch_size(walk);
	}

	mod_lruvec_state(lruvec, PGDEMOTE_KSWAPD + reclaimer_offset(sc),
					stat.nr_demoted);

	item = PGSTEAL_KSWAPD + reclaimer_offset(sc);
	if (!cgroup_reclaim(sc))
		__count_vm_events(item, reclaimed);
	count_memcg_events(memcg, item, reclaimed);
	__count_vm_events(PGSTEAL_ANON + type, reclaimed);

	spin_unlock_irq(&lruvec->lru_lock);

	list_splice_init(&clean, &list);

	if (!list_empty(&list)) {
		skip_retry = true;
		goto retry;
	}

	return scanned;
}

static bool should_run_aging(struct lruvec *lruvec, unsigned long max_seq,
			     int swappiness, unsigned long *nr_to_scan)
{
	int gen, type, zone;
	unsigned long size = 0;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	DEFINE_MIN_SEQ(lruvec);

	*nr_to_scan = 0;
	/* have to run aging, since eviction is not possible anymore */
	if (evictable_min_seq(min_seq, swappiness) + MIN_NR_GENS > max_seq)
		return true;

	for_each_evictable_type(type, swappiness) {
		unsigned long seq;

		for (seq = min_seq[type]; seq <= max_seq; seq++) {
			gen = lru_gen_from_seq(seq);

			for (zone = 0; zone < MAX_NR_ZONES; zone++)
				size += max(READ_ONCE(lrugen->nr_pages[gen][type][zone]), 0L);
		}
	}

	*nr_to_scan = size;
	/* better to run aging even though eviction is still possible */
	return evictable_min_seq(min_seq, swappiness) + MIN_NR_GENS == max_seq;
}

/*
 * For future optimizations:
 * 1. Defer try_to_inc_max_seq() to workqueues to reduce latency for memcg
 *    reclaim.
 */
static long get_nr_to_scan(struct lruvec *lruvec, struct scan_control *sc, int swappiness)
{
	bool success;
	unsigned long nr_to_scan;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	DEFINE_MAX_SEQ(lruvec);

	if (mem_cgroup_below_min(sc->target_mem_cgroup, memcg))
		return -1;

	success = should_run_aging(lruvec, max_seq, swappiness, &nr_to_scan);

	/* try to scrape all its memory if this memcg was deleted */
	if (nr_to_scan && !mem_cgroup_online(memcg))
		return nr_to_scan;

	nr_to_scan = apply_proportional_protection(memcg, sc, nr_to_scan);

	/* try to get away with not aging at the default priority */
	if (!success || sc->priority == DEF_PRIORITY)
		return nr_to_scan >> sc->priority;

	/* stop scanning this lruvec as it's low on cold folios */
	return try_to_inc_max_seq(lruvec, max_seq, swappiness, false) ? -1 : 0;
}

static bool should_abort_scan(struct lruvec *lruvec, struct scan_control *sc)
{
	int i;
	enum zone_watermarks mark;

	/* don't abort memcg reclaim to ensure fairness */
	if (!root_reclaim(sc))
		return false;

	if (sc->nr_reclaimed >= max(sc->nr_to_reclaim, compact_gap(sc->order)))
		return true;

	/* check the order to exclude compaction-induced reclaim */
	if (!current_is_kswapd() || sc->order)
		return false;

	mark = sysctl_numa_balancing_mode & NUMA_BALANCING_MEMORY_TIERING ?
	       WMARK_PROMO : WMARK_HIGH;

	for (i = 0; i <= sc->reclaim_idx; i++) {
		struct zone *zone = lruvec_pgdat(lruvec)->node_zones + i;
		unsigned long size = wmark_pages(zone, mark) + MIN_LRU_BATCH;

		if (managed_zone(zone) && !zone_watermark_ok(zone, 0, size, sc->reclaim_idx, 0))
			return false;
	}

	/* kswapd should abort if all eligible zones are safe */
	return true;
}

bool try_to_shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
	long nr_to_scan;
	unsigned long scanned = 0;
	int swappiness = get_swappiness(lruvec, sc);

	while (true) {
		int delta;

		nr_to_scan = get_nr_to_scan(lruvec, sc, swappiness);
		if (nr_to_scan <= 0)
			break;

		delta = evict_folios(nr_to_scan, lruvec, sc, swappiness);
		if (!delta)
			break;

		scanned += delta;
		if (scanned >= nr_to_scan)
			break;

		if (should_abort_scan(lruvec, sc))
			break;

		cond_resched();
	}

	/*
	 * If too many file cache in the coldest generation can't be evicted
	 * due to being dirty, wake up the flusher.
	 */
	if (sc->nr.unqueued_dirty && sc->nr.unqueued_dirty == sc->nr.file_taken)
		wakeup_flusher_threads(WB_REASON_VMSCAN);

	/* whether this lruvec should be rotated */
	return nr_to_scan < 0;
}

static int shrink_one(struct lruvec *lruvec, struct scan_control *sc)
{
	bool success;
	unsigned long scanned = sc->nr_scanned;
	unsigned long reclaimed = sc->nr_reclaimed;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	/* lru_gen_age_node() called mem_cgroup_calculate_protection() */
	if (mem_cgroup_below_min(NULL, memcg))
		return MEMCG_LRU_YOUNG;

	if (mem_cgroup_below_low(NULL, memcg)) {
		/* see the comment on MEMCG_NR_GENS */
		if (READ_ONCE(lruvec->lrugen.seg) != MEMCG_LRU_TAIL)
			return MEMCG_LRU_TAIL;

		memcg_memory_event(memcg, MEMCG_LOW);
	}

	success = try_to_shrink_lruvec(lruvec, sc);

	shrink_slab(sc->gfp_mask, pgdat->node_id, memcg, sc->priority);

	if (!sc->proactive)
		vmpressure(sc->gfp_mask, memcg, false, sc->nr_scanned - scanned,
			   sc->nr_reclaimed - reclaimed);

	flush_reclaim_state(sc);

	if (success && mem_cgroup_online(memcg))
		return MEMCG_LRU_YOUNG;

	if (!success && lruvec_is_sizable(lruvec, sc))
		return 0;

	/* one retry if offlined or too small */
	return READ_ONCE(lruvec->lrugen.seg) != MEMCG_LRU_TAIL ?
	       MEMCG_LRU_TAIL : MEMCG_LRU_YOUNG;
}

static void shrink_many(struct pglist_data *pgdat, struct scan_control *sc)
{
	int op;
	int gen;
	int bin;
	int first_bin;
	struct lruvec *lruvec;
	struct lru_gen_folio *lrugen;
	struct mem_cgroup *memcg;
	struct hlist_nulls_node *pos;

	gen = get_memcg_gen(READ_ONCE(pgdat->memcg_lru.seq));
	bin = first_bin = get_random_u32_below(MEMCG_NR_BINS);
restart:
	op = 0;
	memcg = NULL;

	rcu_read_lock();

	hlist_nulls_for_each_entry_rcu(lrugen, pos, &pgdat->memcg_lru.fifo[gen][bin], list) {
		if (op) {
			lru_gen_rotate_memcg(lruvec, op);
			op = 0;
		}

		mem_cgroup_put(memcg);
		memcg = NULL;

		if (gen != READ_ONCE(lrugen->gen))
			continue;

		lruvec = container_of(lrugen, struct lruvec, lrugen);
		memcg = lruvec_memcg(lruvec);

		if (!mem_cgroup_tryget(memcg)) {
			lru_gen_release_memcg(memcg);
			memcg = NULL;
			continue;
		}

		rcu_read_unlock();

		op = shrink_one(lruvec, sc);

		rcu_read_lock();

		if (should_abort_scan(lruvec, sc))
			break;
	}

	rcu_read_unlock();

	if (op)
		lru_gen_rotate_memcg(lruvec, op);

	mem_cgroup_put(memcg);

	if (!is_a_nulls(pos))
		return;

	/* restart if raced with lru_gen_rotate_memcg() */
	if (gen != get_nulls_value(pos))
		goto restart;

	/* try the rest of the bins of the current generation */
	bin = get_memcg_bin(bin + 1);
	if (bin != first_bin)
		goto restart;
}

void lru_gen_shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
	struct blk_plug plug;

	VM_WARN_ON_ONCE(root_reclaim(sc));
	VM_WARN_ON_ONCE(!sc->may_writepage || !sc->may_unmap);

	lru_add_drain();

	blk_start_plug(&plug);

	set_mm_walk(NULL, sc->proactive);

	if (try_to_shrink_lruvec(lruvec, sc))
		lru_gen_rotate_memcg(lruvec, MEMCG_LRU_YOUNG);

	clear_mm_walk();

	blk_finish_plug(&plug);
}

void lru_gen_shrink_node(struct pglist_data *pgdat, struct scan_control *sc)
{
	struct blk_plug plug;
	unsigned long reclaimed = sc->nr_reclaimed;

	VM_WARN_ON_ONCE(!root_reclaim(sc));

	/*
	 * Unmapped clean folios are already prioritized. Scanning for more of
	 * them is likely futile and can cause high reclaim latency when there
	 * is a large number of memcgs.
	 */
	if (!sc->may_writepage || !sc->may_unmap)
		goto done;

	lru_add_drain();

	blk_start_plug(&plug);

	set_mm_walk(pgdat, sc->proactive);

	set_initial_priority(pgdat, sc);

	if (current_is_kswapd())
		sc->nr_reclaimed = 0;

	if (mem_cgroup_disabled())
		shrink_one(&pgdat->__lruvec, sc);
	else
		shrink_many(pgdat, sc);

	if (current_is_kswapd())
		sc->nr_reclaimed += reclaimed;

	clear_mm_walk();

	blk_finish_plug(&plug);
done:
	if (sc->nr_reclaimed > reclaimed)
		kswapd_try_clear_hopeless(pgdat, sc->order, sc->reclaim_idx);
}

/******************************************************************************
 *                          state change
 ******************************************************************************/

static bool __maybe_unused state_is_valid(struct lruvec *lruvec)
{
	struct lru_gen_folio *lrugen = &lruvec->lrugen;

	if (lrugen->enabled) {
		enum lru_list lru;

		for_each_evictable_lru(lru) {
			if (!list_empty(&lruvec->lists[lru]))
				return false;
		}
	} else {
		int gen, type, zone;

		for_each_gen_type_zone(gen, type, zone) {
			if (!list_empty(&lrugen->folios[gen][type][zone]))
				return false;
		}
	}

	return true;
}

static bool fill_evictable(struct lruvec *lruvec)
{
	enum lru_list lru;
	int remaining = MAX_LRU_BATCH;

	for_each_evictable_lru(lru) {
		int type = is_file_lru(lru);
		bool active = is_active_lru(lru);
		struct list_head *head = &lruvec->lists[lru];

		while (!list_empty(head)) {
			bool success;
			struct folio *folio = lru_to_folio(head);

			VM_WARN_ON_ONCE_FOLIO(folio_test_unevictable(folio), folio);
			VM_WARN_ON_ONCE_FOLIO(folio_test_active(folio) != active, folio);
			VM_WARN_ON_ONCE_FOLIO(folio_is_file_lru(folio) != type, folio);
			VM_WARN_ON_ONCE_FOLIO(folio_lru_gen(folio) != -1, folio);

			lruvec_del_folio(lruvec, folio);
			success = lru_gen_add_folio(lruvec, folio, false);
			VM_WARN_ON_ONCE(!success);

			if (!--remaining)
				return false;
		}
	}

	return true;
}

static bool drain_evictable(struct lruvec *lruvec)
{
	int gen, type, zone;
	int remaining = MAX_LRU_BATCH;

	for_each_gen_type_zone(gen, type, zone) {
		struct list_head *head = &lruvec->lrugen.folios[gen][type][zone];

		while (!list_empty(head)) {
			bool success;
			struct folio *folio = lru_to_folio(head);

			VM_WARN_ON_ONCE_FOLIO(folio_test_unevictable(folio), folio);
			VM_WARN_ON_ONCE_FOLIO(folio_test_active(folio), folio);
			VM_WARN_ON_ONCE_FOLIO(folio_is_file_lru(folio) != type, folio);
			VM_WARN_ON_ONCE_FOLIO(folio_zonenum(folio) != zone, folio);

			success = lru_gen_del_folio(lruvec, folio, false);
			VM_WARN_ON_ONCE(!success);
			lruvec_add_folio(lruvec, folio);

			if (!--remaining)
				return false;
		}
	}

	return true;
}

static void lru_gen_change_state(bool enabled)
{
	static DEFINE_MUTEX(state_mutex);

	struct mem_cgroup *memcg;

	cgroup_lock();
	cpus_read_lock();
	get_online_mems();
	mutex_lock(&state_mutex);

	if (enabled == lru_gen_enabled())
		goto unlock;

	if (enabled)
		static_branch_enable_cpuslocked(&lru_gen_caps[LRU_GEN_CORE]);
	else
		static_branch_disable_cpuslocked(&lru_gen_caps[LRU_GEN_CORE]);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		int nid;

		for_each_node(nid) {
			struct lruvec *lruvec = get_lruvec(memcg, nid);

			spin_lock_irq(&lruvec->lru_lock);

			VM_WARN_ON_ONCE(!seq_is_valid(lruvec));
			VM_WARN_ON_ONCE(!state_is_valid(lruvec));

			lruvec->lrugen.enabled = enabled;

			while (!(enabled ? fill_evictable(lruvec) : drain_evictable(lruvec))) {
				spin_unlock_irq(&lruvec->lru_lock);
				cond_resched();
				spin_lock_irq(&lruvec->lru_lock);
			}

			spin_unlock_irq(&lruvec->lru_lock);
		}

		cond_resched();
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));
unlock:
	mutex_unlock(&state_mutex);
	put_online_mems();
	cpus_read_unlock();
	cgroup_unlock();
}

/******************************************************************************
 *                          sysfs interface
 ******************************************************************************/

static ssize_t min_ttl_ms_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", jiffies_to_msecs(READ_ONCE(lru_gen_min_ttl)));
}

/* see Documentation/admin-guide/mm/multigen_lru.rst for details */
static ssize_t min_ttl_ms_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t len)
{
	unsigned int msecs;

	if (kstrtouint(buf, 0, &msecs))
		return -EINVAL;

	WRITE_ONCE(lru_gen_min_ttl, msecs_to_jiffies(msecs));

	return len;
}

static struct kobj_attribute lru_gen_min_ttl_attr = __ATTR_RW(min_ttl_ms);

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned int caps = 0;

	if (get_cap(LRU_GEN_CORE))
		caps |= BIT(LRU_GEN_CORE);

	if (should_walk_mmu())
		caps |= BIT(LRU_GEN_MM_WALK);

	if (should_clear_pmd_young())
		caps |= BIT(LRU_GEN_NONLEAF_YOUNG);

	return sysfs_emit(buf, "0x%04x\n", caps);
}

/* see Documentation/admin-guide/mm/multigen_lru.rst for details */
static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t len)
{
	int i;
	unsigned int caps;

	if (tolower(*buf) == 'n')
		caps = 0;
	else if (tolower(*buf) == 'y')
		caps = -1;
	else if (kstrtouint(buf, 0, &caps))
		return -EINVAL;

	for (i = 0; i < NR_LRU_GEN_CAPS; i++) {
		bool enabled = caps & BIT(i);

		if (i == LRU_GEN_CORE)
			lru_gen_change_state(enabled);
		else if (enabled)
			static_branch_enable(&lru_gen_caps[i]);
		else
			static_branch_disable(&lru_gen_caps[i]);
	}

	return len;
}

static struct kobj_attribute lru_gen_enabled_attr = __ATTR_RW(enabled);

static struct attribute *lru_gen_attrs[] = {
	&lru_gen_min_ttl_attr.attr,
	&lru_gen_enabled_attr.attr,
	NULL
};

static const struct attribute_group lru_gen_attr_group = {
	.name = "lru_gen",
	.attrs = lru_gen_attrs,
};

/******************************************************************************
 *                          debugfs interface
 ******************************************************************************/

static void *lru_gen_seq_start(struct seq_file *m, loff_t *pos)
{
	struct mem_cgroup *memcg;
	loff_t nr_to_skip = *pos;

	m->private = kvmalloc(PATH_MAX, GFP_KERNEL);
	if (!m->private)
		return ERR_PTR(-ENOMEM);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		int nid;

		for_each_node_state(nid, N_MEMORY) {
			if (!nr_to_skip--)
				return get_lruvec(memcg, nid);
		}
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));

	return NULL;
}

static void lru_gen_seq_stop(struct seq_file *m, void *v)
{
	if (!IS_ERR_OR_NULL(v))
		mem_cgroup_iter_break(NULL, lruvec_memcg(v));

	kvfree(m->private);
	m->private = NULL;
}

static void *lru_gen_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	int nid = lruvec_pgdat(v)->node_id;
	struct mem_cgroup *memcg = lruvec_memcg(v);

	++*pos;

	nid = next_memory_node(nid);
	if (nid == MAX_NUMNODES) {
		memcg = mem_cgroup_iter(NULL, memcg, NULL);
		if (!memcg)
			return NULL;

		nid = first_memory_node;
	}

	return get_lruvec(memcg, nid);
}

static void lru_gen_seq_show_full(struct seq_file *m, struct lruvec *lruvec,
				  unsigned long max_seq, unsigned long *min_seq,
				  unsigned long seq)
{
	int i;
	int type, tier;
	int hist = lru_hist_from_seq(seq);
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	struct lru_gen_mm_state *mm_state = get_mm_state(lruvec);

	for (tier = 0; tier < MAX_NR_TIERS; tier++) {
		seq_printf(m, "            %10d", tier);
		for (type = 0; type < ANON_AND_FILE; type++) {
			const char *s = "xxx";
			unsigned long n[3] = {};

			if (seq == max_seq) {
				s = "RTx";
				n[0] = READ_ONCE(lrugen->avg_refaulted[type][tier]);
				n[1] = READ_ONCE(lrugen->avg_total[type][tier]);
			} else if (seq == min_seq[type] || NR_HIST_GENS > 1) {
				s = "rep";
				n[0] = atomic_long_read(&lrugen->refaulted[hist][type][tier]);
				n[1] = atomic_long_read(&lrugen->evicted[hist][type][tier]);
				n[2] = READ_ONCE(lrugen->protected[hist][type][tier]);
			}

			for (i = 0; i < 3; i++)
				seq_printf(m, " %10lu%c", n[i], s[i]);
		}
		seq_putc(m, '\n');
	}

	if (!mm_state)
		return;

	seq_puts(m, "                      ");
	for (i = 0; i < NR_MM_STATS; i++) {
		const char *s = "xxxx";
		unsigned long n = 0;

		if (seq == max_seq && NR_HIST_GENS == 1) {
			s = "TYFA";
			n = READ_ONCE(mm_state->stats[hist][i]);
		} else if (seq != max_seq && NR_HIST_GENS > 1) {
			s = "tyfa";
			n = READ_ONCE(mm_state->stats[hist][i]);
		}

		seq_printf(m, " %10lu%c", n, s[i]);
	}
	seq_putc(m, '\n');
}

/* see Documentation/admin-guide/mm/multigen_lru.rst for details */
static int lru_gen_seq_show(struct seq_file *m, void *v)
{
	unsigned long seq;
	bool full = debugfs_get_aux_num(m->file);
	struct lruvec *lruvec = v;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	int nid = lruvec_pgdat(lruvec)->node_id;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	if (nid == first_memory_node) {
		const char *path = memcg ? m->private : "";

#ifdef CONFIG_MEMCG
		if (memcg)
			cgroup_path(memcg->css.cgroup, m->private, PATH_MAX);
#endif
		seq_printf(m, "memcg %llu %s\n", mem_cgroup_id(memcg), path);
	}

	seq_printf(m, " node %5d\n", nid);

	if (!full)
		seq = evictable_min_seq(min_seq, MAX_SWAPPINESS / 2);
	else if (max_seq >= MAX_NR_GENS)
		seq = max_seq - MAX_NR_GENS + 1;
	else
		seq = 0;

	for (; seq <= max_seq; seq++) {
		int type, zone;
		int gen = lru_gen_from_seq(seq);
		unsigned long birth = READ_ONCE(lruvec->lrugen.timestamps[gen]);

		seq_printf(m, " %10lu %10u", seq, jiffies_to_msecs(jiffies - birth));

		for (type = 0; type < ANON_AND_FILE; type++) {
			unsigned long size = 0;
			char mark = full && seq < min_seq[type] ? 'x' : ' ';

			for (zone = 0; zone < MAX_NR_ZONES; zone++)
				size += max(READ_ONCE(lrugen->nr_pages[gen][type][zone]), 0L);

			seq_printf(m, " %10lu%c", size, mark);
		}

		seq_putc(m, '\n');

		if (full)
			lru_gen_seq_show_full(m, lruvec, max_seq, min_seq, seq);
	}

	return 0;
}

static const struct seq_operations lru_gen_seq_ops = {
	.start = lru_gen_seq_start,
	.stop = lru_gen_seq_stop,
	.next = lru_gen_seq_next,
	.show = lru_gen_seq_show,
};

static int run_aging(struct lruvec *lruvec, unsigned long seq,
		     int swappiness, bool force_scan)
{
	DEFINE_MAX_SEQ(lruvec);

	if (seq > max_seq)
		return -EINVAL;

	return try_to_inc_max_seq(lruvec, max_seq, swappiness, force_scan) ? 0 : -EEXIST;
}

static int run_eviction(struct lruvec *lruvec, unsigned long seq, struct scan_control *sc,
			int swappiness, unsigned long nr_to_reclaim)
{
	DEFINE_MAX_SEQ(lruvec);

	if (seq + MIN_NR_GENS > max_seq)
		return -EINVAL;

	sc->nr_reclaimed = 0;

	while (!signal_pending(current)) {
		DEFINE_MIN_SEQ(lruvec);

		if (seq < evictable_min_seq(min_seq, swappiness))
			return 0;

		if (sc->nr_reclaimed >= nr_to_reclaim)
			return 0;

		if (!evict_folios(nr_to_reclaim - sc->nr_reclaimed, lruvec, sc,
				  swappiness))
			return 0;

		cond_resched();
	}

	return -EINTR;
}

static int run_cmd(char cmd, u64 memcg_id, int nid, unsigned long seq,
		   struct scan_control *sc, int swappiness, unsigned long opt)
{
	struct lruvec *lruvec;
	int err = -EINVAL;
	struct mem_cgroup *memcg = NULL;

	if (nid < 0 || nid >= MAX_NUMNODES || !node_state(nid, N_MEMORY))
		return -EINVAL;

	if (!mem_cgroup_disabled()) {
		memcg = mem_cgroup_get_from_id(memcg_id);
		if (!memcg)
			return -EINVAL;
	}

	if (memcg_id != mem_cgroup_id(memcg))
		goto done;

	sc->target_mem_cgroup = memcg;
	lruvec = get_lruvec(memcg, nid);

	if (swappiness < MIN_SWAPPINESS)
		swappiness = get_swappiness(lruvec, sc);
	else if (swappiness > SWAPPINESS_ANON_ONLY)
		goto done;

	switch (cmd) {
	case '+':
		err = run_aging(lruvec, seq, swappiness, opt);
		break;
	case '-':
		err = run_eviction(lruvec, seq, sc, swappiness, opt);
		break;
	}
done:
	mem_cgroup_put(memcg);

	return err;
}

/* see Documentation/admin-guide/mm/multigen_lru.rst for details */
static ssize_t lru_gen_seq_write(struct file *file, const char __user *src,
				 size_t len, loff_t *pos)
{
	void *buf;
	char *cur, *next;
	unsigned int flags;
	struct blk_plug plug;
	int err = -EINVAL;
	struct scan_control sc = {
		.may_writepage = true,
		.may_unmap = true,
		.may_swap = true,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.gfp_mask = GFP_KERNEL,
		.proactive = true,
	};

	buf = kvmalloc(len + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, src, len)) {
		kvfree(buf);
		return -EFAULT;
	}

	set_task_reclaim_state(current, &sc.reclaim_state);
	flags = memalloc_noreclaim_save();
	blk_start_plug(&plug);
	if (!set_mm_walk(NULL, true)) {
		err = -ENOMEM;
		goto done;
	}

	next = buf;
	next[len] = '\0';

	while ((cur = strsep(&next, ",;\n"))) {
		int n;
		int end;
		char cmd, swap_string[5];
		u64 memcg_id;
		unsigned int nid;
		unsigned long seq;
		unsigned int swappiness;
		unsigned long opt = -1;

		cur = skip_spaces(cur);
		if (!*cur)
			continue;

		n = sscanf(cur, "%c %llu %u %lu %n %4s %n %lu %n", &cmd, &memcg_id, &nid,
			   &seq, &end, swap_string, &end, &opt, &end);
		if (n < 4 || cur[end]) {
			err = -EINVAL;
			break;
		}

		if (n == 4) {
			swappiness = -1;
		} else if (!strcmp("max", swap_string)) {
			/* set by userspace for anonymous memory only */
			swappiness = SWAPPINESS_ANON_ONLY;
		} else {
			err = kstrtouint(swap_string, 0, &swappiness);
			if (err)
				break;
		}

		err = run_cmd(cmd, memcg_id, nid, seq, &sc, swappiness, opt);
		if (err)
			break;
	}
done:
	clear_mm_walk();
	blk_finish_plug(&plug);
	memalloc_noreclaim_restore(flags);
	set_task_reclaim_state(current, NULL);

	kvfree(buf);

	return err ? : len;
}

static int lru_gen_seq_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &lru_gen_seq_ops);
}

static const struct file_operations lru_gen_rw_fops = {
	.open = lru_gen_seq_open,
	.read = seq_read,
	.write = lru_gen_seq_write,
	.llseek = seq_lseek,
	.release = seq_release,
};

static const struct file_operations lru_gen_ro_fops = {
	.open = lru_gen_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

/******************************************************************************
 *                          initialization
 ******************************************************************************/

void lru_gen_init_pgdat(struct pglist_data *pgdat)
{
	int i, j;

	spin_lock_init(&pgdat->memcg_lru.lock);

	for (i = 0; i < MEMCG_NR_GENS; i++) {
		for (j = 0; j < MEMCG_NR_BINS; j++)
			INIT_HLIST_NULLS_HEAD(&pgdat->memcg_lru.fifo[i][j], i);
	}
}

void lru_gen_init_lruvec(struct lruvec *lruvec)
{
	int i;
	int gen, type, zone;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	struct lru_gen_mm_state *mm_state = get_mm_state(lruvec);

	lrugen->max_seq = MIN_NR_GENS + 1;
	lrugen->enabled = lru_gen_enabled();

	for (i = 0; i <= MIN_NR_GENS + 1; i++)
		lrugen->timestamps[i] = jiffies;

	for_each_gen_type_zone(gen, type, zone)
		INIT_LIST_HEAD(&lrugen->folios[gen][type][zone]);

	if (mm_state)
		mm_state->seq = MIN_NR_GENS;
}

#ifdef CONFIG_MEMCG

void lru_gen_init_memcg(struct mem_cgroup *memcg)
{
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);

	if (!mm_list)
		return;

	INIT_LIST_HEAD(&mm_list->fifo);
	spin_lock_init(&mm_list->lock);
}

void lru_gen_exit_memcg(struct mem_cgroup *memcg)
{
	int i;
	int nid;
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);

	VM_WARN_ON_ONCE(mm_list && !list_empty(&mm_list->fifo));

	for_each_node(nid) {
		struct lruvec *lruvec = get_lruvec(memcg, nid);
		struct lru_gen_mm_state *mm_state = get_mm_state(lruvec);

		VM_WARN_ON_ONCE(memchr_inv(lruvec->lrugen.nr_pages, 0,
					   sizeof(lruvec->lrugen.nr_pages)));

		lruvec->lrugen.list.next = LIST_POISON1;

		if (!mm_state)
			continue;

		for (i = 0; i < NR_BLOOM_FILTERS; i++) {
			bitmap_free(mm_state->filters[i]);
			mm_state->filters[i] = NULL;
		}
	}
}

#endif /* CONFIG_MEMCG */

static int __init init_lru_gen(void)
{
	BUILD_BUG_ON(MIN_NR_GENS + 1 >= MAX_NR_GENS);
	BUILD_BUG_ON(BIT(LRU_GEN_WIDTH) <= MAX_NR_GENS);

	if (sysfs_create_group(mm_kobj, &lru_gen_attr_group))
		pr_err("lru_gen: failed to create sysfs group\n");

	debugfs_create_file_aux_num("lru_gen", 0644, NULL, NULL, false,
				    &lru_gen_rw_fops);
	debugfs_create_file_aux_num("lru_gen_full", 0444, NULL, NULL, true,
				    &lru_gen_ro_fops);

	return 0;
};
late_initcall(init_lru_gen);
