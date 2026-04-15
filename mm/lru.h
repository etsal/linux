#pragma once

struct scan_control {
	/* How many pages shrink_list() should reclaim */
	unsigned long nr_to_reclaim;

	/*
	 * Nodemask of nodes allowed by the caller. If NULL, all nodes
	 * are scanned.
	 */
	nodemask_t	*nodemask;

	/*
	 * The memory cgroup that hit its limit and as a result is the
	 * primary target of this reclaim invocation.
	 */
	struct mem_cgroup *target_mem_cgroup;

	/*
	 * Scan pressure balancing between anon and file LRUs
	 */
	unsigned long	anon_cost;
	unsigned long	file_cost;

	/* Swappiness value for proactive reclaim. Always use sc_swappiness()! */
	int *proactive_swappiness;

	/* Can active folios be deactivated as part of reclaim? */
#define DEACTIVATE_ANON 1
#define DEACTIVATE_FILE 2
	unsigned int may_deactivate:2;
	unsigned int force_deactivate:1;
	unsigned int skipped_deactivate:1;

	/* zone_reclaim_mode, boost reclaim */
	unsigned int may_writepage:1;

	/* zone_reclaim_mode */
	unsigned int may_unmap:1;

	/* zome_reclaim_mode, boost reclaim, cgroup restrictions */
	unsigned int may_swap:1;

	/* Not allow cache_trim_mode to be turned on as part of reclaim? */
	unsigned int no_cache_trim_mode:1;

	/* Has cache_trim_mode failed at least once? */
	unsigned int cache_trim_mode_failed:1;

	/* Proactive reclaim invoked by userspace */
	unsigned int proactive:1;

	/*
	 * Cgroup memory below memory.low is protected as long as we
	 * don't threaten to OOM. If any cgroup is reclaimed at
	 * reduced force or passed over entirely due to its memory.low
	 * setting (memcg_low_skipped), and nothing is reclaimed as a
	 * result, then go back for one more cycle that reclaims the protected
	 * memory (memcg_low_reclaim) to avert OOM.
	 */
	unsigned int memcg_low_reclaim:1;
	unsigned int memcg_low_skipped:1;

	/* Shared cgroup tree walk failed, rescan the whole tree */
	unsigned int memcg_full_walk:1;

	unsigned int hibernation_mode:1;

	/* One of the zones is ready for compaction */
	unsigned int compaction_ready:1;

	/* There is easily reclaimable cold cache in the current node */
	unsigned int cache_trim_mode:1;

	/* The file folios on the current node are dangerously low */
	unsigned int file_is_tiny:1;

	/* Always discard instead of demoting to lower tier memory */
	unsigned int no_demotion:1;

	/* Allocation order */
	s8 order;

	/* Scan (total_size >> priority) pages at once */
	s8 priority;

	/* The highest zone to isolate folios for reclaim from */
	s8 reclaim_idx;

	/* This context's GFP mask */
	gfp_t gfp_mask;

	/* Incremented by the number of inactive pages that were scanned */
	unsigned long nr_scanned;

	/* Number of pages freed so far during a call to shrink_zones() */
	unsigned long nr_reclaimed;

	struct {
		unsigned int dirty;
		unsigned int unqueued_dirty;
		unsigned int congested;
		unsigned int writeback;
		unsigned int immediate;
		unsigned int file_taken;
		unsigned int taken;
	} nr;

	/* for recording the reclaimed slab by now */
	struct reclaim_state reclaim_state;
};

extern int vm_swappiness;

#ifdef CONFIG_LRU_GEN
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
#else
static bool lru_gen_set_refs(struct folio *folio)
{
	return false;
}
#endif /* CONFIG_LRU_GEN */

void lru_gen_age_node(struct pglist_data *pgdat, struct scan_control *sc);
void lru_gen_shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc);
void lru_gen_shrink_node(struct pglist_data *pgdat, struct scan_control *sc);


/* COMMON */

void shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc);
bool can_demote(int nid, struct scan_control *sc,
		       struct mem_cgroup *memcg);

static inline bool can_reclaim_anon_pages(struct mem_cgroup *memcg,
					  int nid,
					  struct scan_control *sc)
{
	if (memcg == NULL) {
		/*
		 * For non-memcg reclaim, is there
		 * space in any swap device?
		 */
		if (get_nr_swap_pages() > 0)
			return true;
	} else {
		/* Is the memcg below its swap limit? */
		if (mem_cgroup_get_nr_swap_pages(memcg) > 0)
			return true;
	}

	/*
	 * The page can not be swapped.
	 *
	 * Can it be reclaimed from this node via demotion?
	 */
	return can_demote(nid, sc, memcg);
}

#ifdef CONFIG_MEMCG
/* Returns true for reclaim through cgroup limits or cgroup interfaces. */
static bool cgroup_reclaim(struct scan_control *sc)
{
	return sc->target_mem_cgroup;
}

/*
 * Returns true for reclaim on the root cgroup. This is true for direct
 * allocator reclaim and reclaim through cgroup interfaces on the root cgroup.
 */
static bool root_reclaim(struct scan_control *sc)
{
	return !sc->target_mem_cgroup || mem_cgroup_is_root(sc->target_mem_cgroup);
}

static int sc_swappiness(struct scan_control *sc, struct mem_cgroup *memcg)
{
	if (sc->proactive && sc->proactive_swappiness)
		return *sc->proactive_swappiness;
	return mem_cgroup_swappiness(memcg);
}
#else
static bool cgroup_reclaim(struct scan_control *sc)
{
	return false;
}

static bool root_reclaim(struct scan_control *sc)
{
	return true;
}

static int sc_swappiness(struct scan_control *sc, struct mem_cgroup *memcg)
{
	return READ_ONCE(vm_swappiness);
}
#endif

int reclaimer_offset(struct scan_control *sc);
unsigned long apply_proportional_protection(struct mem_cgroup *memcg,
		struct scan_control *sc, unsigned long scan);
unsigned int move_folios_to_lru(struct lruvec *lruvec,
		struct list_head *list);
void flush_reclaim_state(struct scan_control *sc);
bool try_to_shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc);
unsigned int shrink_folio_list(struct list_head *folio_list,
		struct pglist_data *pgdat, struct scan_control *sc,
		struct reclaim_stat *stat, bool ignore_references,
		struct mem_cgroup *memcg);
void set_task_reclaim_state(struct task_struct *task,
				   struct reclaim_state *rs);
