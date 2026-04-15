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

#ifdef ARCH_HAS_PREFETCHW
#define prefetchw_prev_lru_folio(_folio, _base, _field)			\
	do {								\
		if ((_folio)->lru.prev != _base) {			\
			struct folio *prev;				\
									\
			prev = lru_to_folio(&(_folio->lru));		\
			prefetchw(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetchw_prev_lru_folio(_folio, _base, _field) do { } while (0)
#endif

/**
 * lruvec_lru_size -  Returns the number of pages on the given LRU list.
 * @lruvec: lru vector
 * @lru: lru to use
 * @zone_idx: zones to consider (use MAX_NR_ZONES - 1 for the whole LRU list)
 */
static unsigned long lruvec_lru_size(struct lruvec *lruvec, enum lru_list lru,
				     int zone_idx)
{
	unsigned long size = 0;
	int zid;
	struct zone *zone;

	for_each_managed_zone_pgdat(zone, lruvec_pgdat(lruvec), zid, zone_idx) {
		if (!mem_cgroup_disabled())
			size += mem_cgroup_get_zone_lru_size(lruvec, lru, zid);
		else
			size += zone_page_state(zone, NR_ZONE_LRU_BASE + lru);
	}
	return size;
}

enum folio_references lru_folio_check_references(struct folio *folio,
						  struct scan_control *sc)
{
	int referenced_ptes, referenced_folio;
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

	referenced_folio = folio_test_clear_referenced(folio);

	if (referenced_ptes) {
		/*
		 * All mapped folios start out with page table
		 * references from the instantiating fault, so we need
		 * to look twice if a mapped file/anon folio is used more
		 * than once.
		 *
		 * Mark it and spare it for another trip around the
		 * inactive list.  Another page table reference will
		 * lead to its activation.
		 *
		 * Note: the mark is set for activated folios as well
		 * so that recently deactivated but used folios are
		 * quickly recovered.
		 */
		folio_set_referenced(folio);

		if (referenced_folio || referenced_ptes > 1)
			return FOLIOREF_ACTIVATE;

		/*
		 * Activate file-backed executable folios after first usage.
		 */
		if ((vm_flags & VM_EXEC) && folio_is_file_lru(folio))
			return FOLIOREF_ACTIVATE;

		return FOLIOREF_KEEP;
	}

	/* Reclaim if clean, defer dirty folios to writeback */
	if (referenced_folio && folio_is_file_lru(folio))
		return FOLIOREF_RECLAIM_CLEAN;

	return FOLIOREF_RECLAIM;
}

/*
 * Update LRU sizes after isolating pages. The LRU size updates must
 * be complete before mem_cgroup_update_lru_size due to a sanity check.
 */
static __always_inline void update_lru_sizes(struct lruvec *lruvec,
			enum lru_list lru, unsigned long *nr_zone_taken)
{
	int zid;

	for (zid = 0; zid < MAX_NR_ZONES; zid++) {
		if (!nr_zone_taken[zid])
			continue;

		update_lru_size(lruvec, lru, zid, -nr_zone_taken[zid]);
	}

}

/*
 * Isolating page from the lruvec to fill in @dst list by nr_to_scan times.
 *
 * lruvec->lru_lock is heavily contended.  Some of the functions that
 * shrink the lists perform better by taking out a batch of pages
 * and working on them outside the LRU lock.
 *
 * For pagecache intensive workloads, this function is the hottest
 * spot in the kernel (apart from copy_*_user functions).
 *
 * Lru_lock must be held before calling this function.
 *
 * @nr_to_scan:	The number of eligible pages to look through on the list.
 * @lruvec:	The LRU vector to pull pages from.
 * @dst:	The temp list to put pages on to.
 * @nr_scanned:	The number of pages that were scanned.
 * @sc:		The scan_control struct for this reclaim session
 * @lru:	LRU list id for isolating
 *
 * returns how many pages were moved onto *@dst.
 */
static unsigned long isolate_lru_folios(unsigned long nr_to_scan,
		struct lruvec *lruvec, struct list_head *dst,
		unsigned long *nr_scanned, struct scan_control *sc,
		enum lru_list lru)
{
	struct list_head *src = &lruvec->lists[lru];
	unsigned long nr_taken = 0;
	unsigned long nr_zone_taken[MAX_NR_ZONES] = { 0 };
	unsigned long nr_skipped[MAX_NR_ZONES] = { 0, };
	unsigned long skipped = 0, total_scan = 0, scan = 0;
	unsigned long nr_pages;
	unsigned long max_nr_skipped = 0;
	LIST_HEAD(folios_skipped);

	while (scan < nr_to_scan && !list_empty(src)) {
		struct list_head *move_to = src;
		struct folio *folio;

		folio = lru_to_folio(src);
		prefetchw_prev_lru_folio(folio, src, flags);

		nr_pages = folio_nr_pages(folio);
		total_scan += nr_pages;

		/* Using max_nr_skipped to prevent hard LOCKUP*/
		if (max_nr_skipped < SWAP_CLUSTER_MAX_SKIPPED &&
		    (folio_zonenum(folio) > sc->reclaim_idx)) {
			nr_skipped[folio_zonenum(folio)] += nr_pages;
			move_to = &folios_skipped;
			max_nr_skipped++;
			goto move;
		}

		/*
		 * Do not count skipped folios because that makes the function
		 * return with no isolated folios if the LRU mostly contains
		 * ineligible folios.  This causes the VM to not reclaim any
		 * folios, triggering a premature OOM.
		 * Account all pages in a folio.
		 */
		scan += nr_pages;

		if (!folio_test_lru(folio))
			goto move;
		if (!sc->may_unmap && folio_mapped(folio))
			goto move;

		/*
		 * Be careful not to clear the lru flag until after we're
		 * sure the folio is not being freed elsewhere -- the
		 * folio release code relies on it.
		 */
		if (unlikely(!folio_try_get(folio)))
			goto move;

		if (!folio_test_clear_lru(folio)) {
			/* Another thread is already isolating this folio */
			folio_put(folio);
			goto move;
		}

		nr_taken += nr_pages;
		nr_zone_taken[folio_zonenum(folio)] += nr_pages;
		move_to = dst;
move:
		list_move(&folio->lru, move_to);
	}

	/*
	 * Splice any skipped folios to the start of the LRU list. Note that
	 * this disrupts the LRU order when reclaiming for lower zones but
	 * we cannot splice to the tail. If we did then the SWAP_CLUSTER_MAX
	 * scanning would soon rescan the same folios to skip and waste lots
	 * of cpu cycles.
	 */
	if (!list_empty(&folios_skipped)) {
		int zid;

		list_splice(&folios_skipped, src);
		for (zid = 0; zid < MAX_NR_ZONES; zid++) {
			if (!nr_skipped[zid])
				continue;

			__count_zid_vm_events(PGSCAN_SKIP, zid, nr_skipped[zid]);
			skipped += nr_skipped[zid];
		}
	}
	*nr_scanned = total_scan;
	trace_mm_vmscan_lru_isolate(sc->reclaim_idx, sc->order, nr_to_scan,
				    total_scan, skipped, nr_taken, lru);
	update_lru_sizes(lruvec, lru, nr_zone_taken);
	return nr_taken;
}

static bool too_many_isolated(struct pglist_data *pgdat, int file,
		struct scan_control *sc)
{
	unsigned long inactive, isolated;
	bool too_many;

	if (current_is_kswapd())
		return false;

	if (!writeback_throttling_sane(sc))
		return false;

	if (file) {
		inactive = node_page_state(pgdat, NR_INACTIVE_FILE);
		isolated = node_page_state(pgdat, NR_ISOLATED_FILE);
	} else {
		inactive = node_page_state(pgdat, NR_INACTIVE_ANON);
		isolated = node_page_state(pgdat, NR_ISOLATED_ANON);
	}

	/*
	 * GFP_NOIO/GFP_NOFS callers are allowed to isolate more pages, so they
	 * won't get blocked by normal direct-reclaimers, forming a circular
	 * deadlock.
	 */
	if (gfp_has_io_fs(sc->gfp_mask))
		inactive >>= 3;

	too_many = isolated > inactive;

	/* Wake up tasks throttled due to too_many_isolated. */
	if (!too_many)
		wake_throttle_isolated(pgdat);

	return too_many;
}

/*
 * If a kernel thread (such as nfsd for loop-back mounts) services a backing
 * device by writing to the page cache it sets PF_LOCAL_THROTTLE. In this case
 * we should not throttle.  Otherwise it is safe to do so.
 */
static int current_may_throttle(void)
{
	return !(current->flags & PF_LOCAL_THROTTLE);
}

/*
 * shrink_inactive_list() is a helper for shrink_node().  It returns the number
 * of reclaimed pages
 */
static unsigned long shrink_inactive_list(unsigned long nr_to_scan,
		struct lruvec *lruvec, struct scan_control *sc,
		enum lru_list lru)
{
	LIST_HEAD(folio_list);
	unsigned long nr_scanned;
	unsigned int nr_reclaimed = 0;
	unsigned long nr_taken;
	struct reclaim_stat stat;
	bool file = is_file_lru(lru);
	enum vm_event_item item;
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	bool stalled = false;

	while (unlikely(too_many_isolated(pgdat, file, sc))) {
		if (stalled)
			return 0;

		/* wait a bit for the reclaimer. */
		stalled = true;
		reclaim_throttle(pgdat, VMSCAN_THROTTLE_ISOLATED);

		/* We are about to die and free our memory. Return now. */
		if (fatal_signal_pending(current))
			return SWAP_CLUSTER_MAX;
	}

	lru_add_drain();

	spin_lock_irq(&lruvec->lru_lock);

	nr_taken = isolate_lru_folios(nr_to_scan, lruvec, &folio_list,
				     &nr_scanned, sc, lru);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);
	item = PGSCAN_KSWAPD + reclaimer_offset(sc);
	if (!cgroup_reclaim(sc))
		__count_vm_events(item, nr_scanned);
	count_memcg_events(lruvec_memcg(lruvec), item, nr_scanned);
	__count_vm_events(PGSCAN_ANON + file, nr_scanned);

	spin_unlock_irq(&lruvec->lru_lock);

	if (nr_taken == 0)
		return 0;

	nr_reclaimed = shrink_folio_list(&folio_list, pgdat, sc, &stat, false,
					 lruvec_memcg(lruvec));

	spin_lock_irq(&lruvec->lru_lock);
	move_folios_to_lru(lruvec, &folio_list);

	mod_lruvec_state(lruvec, PGDEMOTE_KSWAPD + reclaimer_offset(sc),
					stat.nr_demoted);
	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
	item = PGSTEAL_KSWAPD + reclaimer_offset(sc);
	if (!cgroup_reclaim(sc))
		__count_vm_events(item, nr_reclaimed);
	count_memcg_events(lruvec_memcg(lruvec), item, nr_reclaimed);
	__count_vm_events(PGSTEAL_ANON + file, nr_reclaimed);

	lru_note_cost_unlock_irq(lruvec, file, stat.nr_pageout,
					nr_scanned - nr_reclaimed);

	/*
	 * If dirty folios are scanned that are not queued for IO, it
	 * implies that flushers are not doing their job. This can
	 * happen when memory pressure pushes dirty folios to the end of
	 * the LRU before the dirty limits are breached and the dirty
	 * data has expired. It can also happen when the proportion of
	 * dirty folios grows not through writes but through memory
	 * pressure reclaiming all the clean cache. And in some cases,
	 * the flushers simply cannot keep up with the allocation
	 * rate. Nudge the flusher threads in case they are asleep.
	 */
	if (stat.nr_unqueued_dirty == nr_taken) {
		wakeup_flusher_threads(WB_REASON_VMSCAN);
		/*
		 * For cgroupv1 dirty throttling is achieved by waking up
		 * the kernel flusher here and later waiting on folios
		 * which are in writeback to finish (see shrink_folio_list()).
		 *
		 * Flusher may not be able to issue writeback quickly
		 * enough for cgroupv1 writeback throttling to work
		 * on a large system.
		 */
		if (!writeback_throttling_sane(sc))
			reclaim_throttle(pgdat, VMSCAN_THROTTLE_WRITEBACK);
	}

	sc->nr.dirty += stat.nr_dirty;
	sc->nr.congested += stat.nr_congested;
	sc->nr.unqueued_dirty += stat.nr_unqueued_dirty;
	sc->nr.writeback += stat.nr_writeback;
	sc->nr.immediate += stat.nr_immediate;
	sc->nr.taken += nr_taken;
	if (file)
		sc->nr.file_taken += nr_taken;

	trace_mm_vmscan_lru_shrink_inactive(pgdat->node_id,
			nr_scanned, nr_reclaimed, &stat, sc->priority, file);
	return nr_reclaimed;
}

/*
 * shrink_active_list() moves folios from the active LRU to the inactive LRU.
 *
 * We move them the other way if the folio is referenced by one or more
 * processes.
 *
 * If the folios are mostly unmapped, the processing is fast and it is
 * appropriate to hold lru_lock across the whole operation.  But if
 * the folios are mapped, the processing is slow (folio_referenced()), so
 * we should drop lru_lock around each folio.  It's impossible to balance
 * this, so instead we remove the folios from the LRU while processing them.
 * It is safe to rely on the active flag against the non-LRU folios in here
 * because nobody will play with that bit on a non-LRU folio.
 *
 * The downside is that we have to touch folio->_refcount against each folio.
 * But we had to alter folio->flags anyway.
 */
static void shrink_active_list(unsigned long nr_to_scan,
			       struct lruvec *lruvec,
			       struct scan_control *sc,
			       enum lru_list lru)
{
	unsigned long nr_taken;
	unsigned long nr_scanned;
	vm_flags_t vm_flags;
	LIST_HEAD(l_hold);	/* The folios which were snipped off */
	LIST_HEAD(l_active);
	LIST_HEAD(l_inactive);
	unsigned nr_deactivate, nr_activate;
	unsigned nr_rotated = 0;
	bool file = is_file_lru(lru);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	lru_add_drain();

	spin_lock_irq(&lruvec->lru_lock);

	nr_taken = isolate_lru_folios(nr_to_scan, lruvec, &l_hold,
				     &nr_scanned, sc, lru);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);

	if (!cgroup_reclaim(sc))
		__count_vm_events(PGREFILL, nr_scanned);
	count_memcg_events(lruvec_memcg(lruvec), PGREFILL, nr_scanned);

	spin_unlock_irq(&lruvec->lru_lock);

	while (!list_empty(&l_hold)) {
		struct folio *folio;

		cond_resched();
		folio = lru_to_folio(&l_hold);
		list_del(&folio->lru);

		if (unlikely(!folio_evictable(folio))) {
			folio_putback_lru(folio);
			continue;
		}

		if (unlikely(buffer_heads_over_limit)) {
			if (folio_needs_release(folio) &&
			    folio_trylock(folio)) {
				filemap_release_folio(folio, 0);
				folio_unlock(folio);
			}
		}

		/* Referenced or rmap lock contention: rotate */
		if (folio_referenced(folio, 0, sc->target_mem_cgroup,
				     &vm_flags) != 0) {
			/*
			 * Identify referenced, file-backed active folios and
			 * give them one more trip around the active list. So
			 * that executable code get better chances to stay in
			 * memory under moderate memory pressure.  Anon folios
			 * are not likely to be evicted by use-once streaming
			 * IO, plus JVM can create lots of anon VM_EXEC folios,
			 * so we ignore them here.
			 */
			if ((vm_flags & VM_EXEC) && folio_is_file_lru(folio)) {
				nr_rotated += folio_nr_pages(folio);
				list_add(&folio->lru, &l_active);
				continue;
			}
		}

		folio_clear_active(folio);	/* we are de-activating */
		folio_set_workingset(folio);
		list_add(&folio->lru, &l_inactive);
	}

	/*
	 * Move folios back to the lru list.
	 */
	spin_lock_irq(&lruvec->lru_lock);

	nr_activate = move_folios_to_lru(lruvec, &l_active);
	nr_deactivate = move_folios_to_lru(lruvec, &l_inactive);

	__count_vm_events(PGDEACTIVATE, nr_deactivate);
	count_memcg_events(lruvec_memcg(lruvec), PGDEACTIVATE, nr_deactivate);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);

	lru_note_cost_unlock_irq(lruvec, file, 0, nr_rotated);
	trace_mm_vmscan_lru_shrink_active(pgdat->node_id, nr_taken, nr_activate,
			nr_deactivate, nr_rotated, sc->priority, file);
}

static unsigned long shrink_list(enum lru_list lru, unsigned long nr_to_scan,
				 struct lruvec *lruvec, struct scan_control *sc)
{
	if (is_active_lru(lru)) {
		if (sc->may_deactivate & (1 << is_file_lru(lru)))
			shrink_active_list(nr_to_scan, lruvec, sc, lru);
		else
			sc->skipped_deactivate = 1;
		return 0;
	}

	return shrink_inactive_list(nr_to_scan, lruvec, sc, lru);
}

/*
 * The inactive anon list should be small enough that the VM never has
 * to do too much work.
 *
 * The inactive file list should be small enough to leave most memory
 * to the established workingset on the scan-resistant active list,
 * but large enough to avoid thrashing the aggregate readahead window.
 *
 * Both inactive lists should also be large enough that each inactive
 * folio has a chance to be referenced again before it is reclaimed.
 *
 * If that fails and refaulting is observed, the inactive list grows.
 *
 * The inactive_ratio is the target ratio of ACTIVE to INACTIVE folios
 * on this LRU, maintained by the pageout code. An inactive_ratio
 * of 3 means 3:1 or 25% of the folios are kept on the inactive list.
 *
 * total     target    max
 * memory    ratio     inactive
 * -------------------------------------
 *   10MB       1         5MB
 *  100MB       1        50MB
 *    1GB       3       250MB
 *   10GB      10       0.9GB
 *  100GB      31         3GB
 *    1TB     101        10GB
 *   10TB     320        32GB
 */
static bool inactive_is_low(struct lruvec *lruvec, enum lru_list inactive_lru)
{
	enum lru_list active_lru = inactive_lru + LRU_ACTIVE;
	unsigned long inactive, active;
	unsigned long inactive_ratio;
	unsigned long gb;

	inactive = lruvec_page_state(lruvec, NR_LRU_BASE + inactive_lru);
	active = lruvec_page_state(lruvec, NR_LRU_BASE + active_lru);

	gb = (inactive + active) >> (30 - PAGE_SHIFT);
	if (gb)
		inactive_ratio = int_sqrt(10 * gb);
	else
		inactive_ratio = 1;

	return inactive * inactive_ratio < active;
}

enum scan_balance {
	SCAN_EQUAL,
	SCAN_FRACT,
	SCAN_ANON,
	SCAN_FILE,
};

static void lru_prepare_scan_control(pg_data_t *pgdat, struct scan_control *sc)
{
	unsigned long file;
	struct lruvec *target_lruvec;

	target_lruvec = mem_cgroup_lruvec(sc->target_mem_cgroup, pgdat);

	/*
	 * Flush the memory cgroup stats in rate-limited way as we don't need
	 * most accurate stats here. We may switch to regular stats flushing
	 * in the future once it is cheap enough.
	 */
	mem_cgroup_flush_stats_ratelimited(sc->target_mem_cgroup);

	/*
	 * Determine the scan balance between anon and file LRUs.
	 */
	spin_lock_irq(&target_lruvec->lru_lock);
	sc->anon_cost = target_lruvec->anon_cost;
	sc->file_cost = target_lruvec->file_cost;
	spin_unlock_irq(&target_lruvec->lru_lock);

	/*
	 * Target desirable inactive:active list ratios for the anon
	 * and file LRU lists.
	 */
	if (!sc->force_deactivate) {
		unsigned long refaults;

		/*
		 * When refaults are being observed, it means a new
		 * workingset is being established. Deactivate to get
		 * rid of any stale active pages quickly.
		 */
		refaults = lruvec_page_state(target_lruvec,
				WORKINGSET_ACTIVATE_ANON);
		if (refaults != target_lruvec->refaults[WORKINGSET_ANON] ||
			inactive_is_low(target_lruvec, LRU_INACTIVE_ANON))
			sc->may_deactivate |= DEACTIVATE_ANON;
		else
			sc->may_deactivate &= ~DEACTIVATE_ANON;

		refaults = lruvec_page_state(target_lruvec,
				WORKINGSET_ACTIVATE_FILE);
		if (refaults != target_lruvec->refaults[WORKINGSET_FILE] ||
		    inactive_is_low(target_lruvec, LRU_INACTIVE_FILE))
			sc->may_deactivate |= DEACTIVATE_FILE;
		else
			sc->may_deactivate &= ~DEACTIVATE_FILE;
	} else
		sc->may_deactivate = DEACTIVATE_ANON | DEACTIVATE_FILE;

	/*
	 * If we have plenty of inactive file pages that aren't
	 * thrashing, try to reclaim those first before touching
	 * anonymous pages.
	 */
	file = lruvec_page_state(target_lruvec, NR_INACTIVE_FILE);
	if (file >> sc->priority && !(sc->may_deactivate & DEACTIVATE_FILE) &&
	    !sc->no_cache_trim_mode)
		sc->cache_trim_mode = 1;
	else
		sc->cache_trim_mode = 0;

	/*
	 * Prevent the reclaimer from falling into the cache trap: as
	 * cache pages start out inactive, every cache fault will tip
	 * the scan balance towards the file LRU.  And as the file LRU
	 * shrinks, so does the window for rotation from references.
	 * This means we have a runaway feedback loop where a tiny
	 * thrashing file LRU becomes infinitely more attractive than
	 * anon pages.  Try to detect this based on file LRU size.
	 */
	if (!cgroup_reclaim(sc)) {
		unsigned long total_high_wmark = 0;
		unsigned long free, anon;
		int z;
		struct zone *zone;

		free = sum_zone_node_page_state(pgdat->node_id, NR_FREE_PAGES);
		file = node_page_state(pgdat, NR_ACTIVE_FILE) +
			   node_page_state(pgdat, NR_INACTIVE_FILE);

		for_each_managed_zone_pgdat(zone, pgdat, z, MAX_NR_ZONES - 1) {
			total_high_wmark += high_wmark_pages(zone);
		}

		/*
		 * Consider anon: if that's low too, this isn't a
		 * runaway file reclaim problem, but rather just
		 * extreme pressure. Reclaim as per usual then.
		 */
		anon = node_page_state(pgdat, NR_INACTIVE_ANON);

		sc->file_is_tiny =
			file + free <= total_high_wmark &&
			!(sc->may_deactivate & DEACTIVATE_ANON) &&
			anon >> sc->priority;
	}
}

static inline void calculate_pressure_balance(struct scan_control *sc,
			int swappiness, u64 *fraction, u64 *denominator)
{
	unsigned long anon_cost, file_cost, total_cost;
	unsigned long ap, fp;

	/*
	 * Calculate the pressure balance between anon and file pages.
	 *
	 * The amount of pressure we put on each LRU is inversely
	 * proportional to the cost of reclaiming each list, as
	 * determined by the share of pages that are refaulting, times
	 * the relative IO cost of bringing back a swapped out
	 * anonymous page vs reloading a filesystem page (swappiness).
	 *
	 * Although we limit that influence to ensure no list gets
	 * left behind completely: at least a third of the pressure is
	 * applied, before swappiness.
	 *
	 * With swappiness at 100, anon and file have equal IO cost.
	 */
	total_cost = sc->anon_cost + sc->file_cost;
	anon_cost = total_cost + sc->anon_cost;
	file_cost = total_cost + sc->file_cost;
	total_cost = anon_cost + file_cost;

	ap = swappiness * (total_cost + 1);
	ap /= anon_cost + 1;

	fp = (MAX_SWAPPINESS - swappiness) * (total_cost + 1);
	fp /= file_cost + 1;

	fraction[WORKINGSET_ANON] = ap;
	fraction[WORKINGSET_FILE] = fp;
	*denominator = ap + fp;
}

/*
 * Determine how aggressively the anon and file LRU lists should be
 * scanned.
 *
 * nr[0] = anon inactive folios to scan; nr[1] = anon active folios to scan
 * nr[2] = file inactive folios to scan; nr[3] = file active folios to scan
 */
static void get_scan_count(struct lruvec *lruvec, struct scan_control *sc,
			   unsigned long *nr)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	int swappiness = sc_swappiness(sc, memcg);
	u64 fraction[ANON_AND_FILE];
	u64 denominator = 0;	/* gcc */
	enum scan_balance scan_balance;
	enum lru_list lru;

	/* If we have no swap space, do not bother scanning anon folios. */
	if (!sc->may_swap || !can_reclaim_anon_pages(memcg, pgdat->node_id, sc)) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	/*
	 * Global reclaim will swap to prevent OOM even with no
	 * swappiness, but memcg users want to use this knob to
	 * disable swapping for individual groups completely when
	 * using the memory controller's swap limit feature would be
	 * too expensive.
	 */
	if (cgroup_reclaim(sc) && !swappiness) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	/* Proactive reclaim initiated by userspace for anonymous memory only */
	if (swappiness == SWAPPINESS_ANON_ONLY) {
		WARN_ON_ONCE(!sc->proactive);
		scan_balance = SCAN_ANON;
		goto out;
	}

	/*
	 * Do not apply any pressure balancing cleverness when the
	 * system is close to OOM, scan both anon and file equally
	 * (unless the swappiness setting disagrees with swapping).
	 */
	if (!sc->priority && swappiness) {
		scan_balance = SCAN_EQUAL;
		goto out;
	}

	/*
	 * If the system is almost out of file pages, force-scan anon.
	 */
	if (sc->file_is_tiny) {
		scan_balance = SCAN_ANON;
		goto out;
	}

	/*
	 * If there is enough inactive page cache, we do not reclaim
	 * anything from the anonymous working right now to make sure
         * a streaming file access pattern doesn't cause swapping.
	 */
	if (sc->cache_trim_mode) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	scan_balance = SCAN_FRACT;
	calculate_pressure_balance(sc, swappiness, fraction, &denominator);

out:
	for_each_evictable_lru(lru) {
		bool file = is_file_lru(lru);
		unsigned long lruvec_size;
		unsigned long scan;

		lruvec_size = lruvec_lru_size(lruvec, lru, sc->reclaim_idx);
		scan = apply_proportional_protection(memcg, sc, lruvec_size);
		scan >>= sc->priority;

		/*
		 * If the cgroup's already been deleted, make sure to
		 * scrape out the remaining cache.
		 */
		if (!scan && !mem_cgroup_online(memcg))
			scan = min(lruvec_size, SWAP_CLUSTER_MAX);

		switch (scan_balance) {
		case SCAN_EQUAL:
			/* Scan lists relative to size */
			break;
		case SCAN_FRACT:
			/*
			 * Scan types proportional to swappiness and
			 * their relative recent reclaim efficiency.
			 * Make sure we don't miss the last page on
			 * the offlined memory cgroups because of a
			 * round-off error.
			 */
			scan = mem_cgroup_online(memcg) ?
			       div64_u64(scan * fraction[file], denominator) :
			       DIV64_U64_ROUND_UP(scan * fraction[file],
						  denominator);
			break;
		case SCAN_FILE:
		case SCAN_ANON:
			/* Scan one type exclusively */
			if ((scan_balance == SCAN_FILE) != file)
				scan = 0;
			break;
		default:
			/* Look ma, no brain */
			BUG();
		}

		nr[lru] = scan;
	}
}

/*
 * Anonymous LRU management is a waste if there is
 * ultimately no way to reclaim the memory.
 */
static bool can_age_anon_pages(struct lruvec *lruvec,
			       struct scan_control *sc)
{
	/* Aging the anon LRU is valuable if swap is present: */
	if (total_swap_pages > 0)
		return true;

	/* Also valuable if anon pages can be demoted: */
	return can_demote(lruvec_pgdat(lruvec)->node_id, sc,
			  lruvec_memcg(lruvec));
}

void lru_shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
	unsigned long nr[NR_LRU_LISTS];
	unsigned long targets[NR_LRU_LISTS];
	unsigned long nr_to_scan;
	enum lru_list lru;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_to_reclaim = sc->nr_to_reclaim;
	bool proportional_reclaim;
	struct blk_plug plug;

	get_scan_count(lruvec, sc, nr);

	/* Record the original scan target for proportional adjustments later */
	memcpy(targets, nr, sizeof(nr));

	/*
	 * Global reclaiming within direct reclaim at DEF_PRIORITY is a normal
	 * event that can occur when there is little memory pressure e.g.
	 * multiple streaming readers/writers. Hence, we do not abort scanning
	 * when the requested number of pages are reclaimed when scanning at
	 * DEF_PRIORITY on the assumption that the fact we are direct
	 * reclaiming implies that kswapd is not keeping up and it is best to
	 * do a batch of work at once. For memcg reclaim one check is made to
	 * abort proportional reclaim if either the file or anon lru has already
	 * dropped to zero at the first pass.
	 */
	proportional_reclaim = (!cgroup_reclaim(sc) && !current_is_kswapd() &&
				sc->priority == DEF_PRIORITY);

	blk_start_plug(&plug);
	while (nr[LRU_INACTIVE_ANON] || nr[LRU_ACTIVE_FILE] ||
					nr[LRU_INACTIVE_FILE]) {
		unsigned long nr_anon, nr_file, percentage;
		unsigned long nr_scanned;

		for_each_evictable_lru(lru) {
			if (nr[lru]) {
				nr_to_scan = min(nr[lru], SWAP_CLUSTER_MAX);
				nr[lru] -= nr_to_scan;

				nr_reclaimed += shrink_list(lru, nr_to_scan,
							    lruvec, sc);
			}
		}

		cond_resched();

		if (nr_reclaimed < nr_to_reclaim || proportional_reclaim)
			continue;

		/*
		 * For kswapd and memcg, reclaim at least the number of pages
		 * requested. Ensure that the anon and file LRUs are scanned
		 * proportionally what was requested by get_scan_count(). We
		 * stop reclaiming one LRU and reduce the amount scanning
		 * proportional to the original scan target.
		 */
		nr_file = nr[LRU_INACTIVE_FILE] + nr[LRU_ACTIVE_FILE];
		nr_anon = nr[LRU_INACTIVE_ANON] + nr[LRU_ACTIVE_ANON];

		/*
		 * It's just vindictive to attack the larger once the smaller
		 * has gone to zero.  And given the way we stop scanning the
		 * smaller below, this makes sure that we only make one nudge
		 * towards proportionality once we've got nr_to_reclaim.
		 */
		if (!nr_file || !nr_anon)
			break;

		if (nr_file > nr_anon) {
			unsigned long scan_target = targets[LRU_INACTIVE_ANON] +
						targets[LRU_ACTIVE_ANON] + 1;
			lru = LRU_BASE;
			percentage = nr_anon * 100 / scan_target;
		} else {
			unsigned long scan_target = targets[LRU_INACTIVE_FILE] +
						targets[LRU_ACTIVE_FILE] + 1;
			lru = LRU_FILE;
			percentage = nr_file * 100 / scan_target;
		}

		/* Stop scanning the smaller of the LRU */
		nr[lru] = 0;
		nr[lru + LRU_ACTIVE] = 0;

		/*
		 * Recalculate the other LRU scan count based on its original
		 * scan target and the percentage scanning already complete
		 */
		lru = (lru == LRU_FILE) ? LRU_BASE : LRU_FILE;
		nr_scanned = targets[lru] - nr[lru];
		nr[lru] = targets[lru] * (100 - percentage) / 100;
		nr[lru] -= min(nr[lru], nr_scanned);

		lru += LRU_ACTIVE;
		nr_scanned = targets[lru] - nr[lru];
		nr[lru] = targets[lru] * (100 - percentage) / 100;
		nr[lru] -= min(nr[lru], nr_scanned);
	}
	blk_finish_plug(&plug);
	sc->nr_reclaimed += nr_reclaimed;

	/*
	 * Even if we did not try to evict anon pages at all, we want to
	 * rebalance the anon lru active/inactive ratio.
	 */
	if (can_age_anon_pages(lruvec, sc) &&
	    inactive_is_low(lruvec, LRU_INACTIVE_ANON))
		shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
				   sc, LRU_ACTIVE_ANON);
}

/* Use reclaim/compaction for costly allocs or under memory pressure */
static bool in_reclaim_compaction(struct scan_control *sc)
{
	if (gfp_compaction_allowed(sc->gfp_mask) && sc->order &&
			(sc->order > PAGE_ALLOC_COSTLY_ORDER ||
			 sc->priority < DEF_PRIORITY - 2))
		return true;

	return false;
}

/*
 * Reclaim/compaction is used for high-order allocation requests. It reclaims
 * order-0 pages before compacting the zone. should_continue_reclaim() returns
 * true if more pages should be reclaimed such that when the page allocator
 * calls try_to_compact_pages() that it will have enough free pages to succeed.
 * It will give up earlier than that if there is difficulty reclaiming pages.
 */
static inline bool should_continue_reclaim(struct pglist_data *pgdat,
					unsigned long nr_reclaimed,
					struct scan_control *sc)
{
	unsigned long pages_for_compaction;
	unsigned long inactive_lru_pages;
	int z;
	struct zone *zone;

	/* If not in reclaim/compaction mode, stop */
	if (!in_reclaim_compaction(sc))
		return false;

	/*
	 * Stop if we failed to reclaim any pages from the last SWAP_CLUSTER_MAX
	 * number of pages that were scanned. This will return to the caller
	 * with the risk reclaim/compaction and the resulting allocation attempt
	 * fails. In the past we have tried harder for __GFP_RETRY_MAYFAIL
	 * allocations through requiring that the full LRU list has been scanned
	 * first, by assuming that zero delta of sc->nr_scanned means full LRU
	 * scan, but that approximation was wrong, and there were corner cases
	 * where always a non-zero amount of pages were scanned.
	 */
	if (!nr_reclaimed)
		return false;

	/* If compaction would go ahead or the allocation would succeed, stop */
	for_each_managed_zone_pgdat(zone, pgdat, z, sc->reclaim_idx) {
		unsigned long watermark = min_wmark_pages(zone);

		/* Allocation can already succeed, nothing to do */
		if (zone_watermark_ok(zone, sc->order, watermark,
				      sc->reclaim_idx, 0))
			return false;

		if (compaction_suitable(zone, sc->order, watermark,
					sc->reclaim_idx))
			return false;
	}

	/*
	 * If we have not reclaimed enough pages for compaction and the
	 * inactive lists are large enough, continue reclaiming
	 */
	pages_for_compaction = compact_gap(sc->order);
	inactive_lru_pages = node_page_state(pgdat, NR_INACTIVE_FILE);
	if (can_reclaim_anon_pages(NULL, pgdat->node_id, sc))
		inactive_lru_pages += node_page_state(pgdat, NR_INACTIVE_ANON);

	return inactive_lru_pages > pages_for_compaction;
}

void lru_shrink_node(pg_data_t *pgdat, struct scan_control *sc)
{
	unsigned long nr_reclaimed, nr_scanned, nr_node_reclaimed;
	struct lruvec *target_lruvec;
	bool reclaimable = false;


	target_lruvec = mem_cgroup_lruvec(sc->target_mem_cgroup, pgdat);

again:
	memset(&sc->nr, 0, sizeof(sc->nr));

	nr_reclaimed = sc->nr_reclaimed;
	nr_scanned = sc->nr_scanned;

	if (!lru_gen_enabled())
		lru_prepare_scan_control(pgdat, sc);

	shrink_node_memcgs(pgdat, sc);

	flush_reclaim_state(sc);

	nr_node_reclaimed = sc->nr_reclaimed - nr_reclaimed;

	/* Record the subtree's reclaim efficiency */
	if (!sc->proactive)
		vmpressure(sc->gfp_mask, sc->target_mem_cgroup, true,
			   sc->nr_scanned - nr_scanned, nr_node_reclaimed);

	if (nr_node_reclaimed)
		reclaimable = true;

	if (current_is_kswapd()) {
		/*
		 * If reclaim is isolating dirty pages under writeback,
		 * it implies that the long-lived page allocation rate
		 * is exceeding the page laundering rate. Either the
		 * global limits are not being effective at throttling
		 * processes due to the page distribution throughout
		 * zones or there is heavy usage of a slow backing
		 * device. The only option is to throttle from reclaim
		 * context which is not ideal as there is no guarantee
		 * the dirtying process is throttled in the same way
		 * balance_dirty_pages() manages.
		 *
		 * Once a node is flagged PGDAT_WRITEBACK, kswapd will
		 * count the number of pages under pages flagged for
		 * immediate reclaim and stall if any are encountered
		 * in the nr_immediate check below.
		 */
		if (sc->nr.writeback && sc->nr.writeback == sc->nr.taken)
			set_bit(PGDAT_WRITEBACK, &pgdat->flags);

		/*
		 * If kswapd scans pages marked for immediate
		 * reclaim and under writeback (nr_immediate), it
		 * implies that pages are cycling through the LRU
		 * faster than they are written so forcibly stall
		 * until some pages complete writeback.
		 */
		if (sc->nr.immediate)
			reclaim_throttle(pgdat, VMSCAN_THROTTLE_WRITEBACK);
	}

	/*
	 * Tag a node/memcg as congested if all the dirty pages were marked
	 * for writeback and immediate reclaim (counted in nr.congested).
	 *
	 * Legacy memcg will stall in page writeback so avoid forcibly
	 * stalling in reclaim_throttle().
	 */
	if (sc->nr.dirty && sc->nr.dirty == sc->nr.congested) {
		if (cgroup_reclaim(sc) && writeback_throttling_sane(sc))
			set_bit(LRUVEC_CGROUP_CONGESTED, &target_lruvec->flags);

		if (current_is_kswapd())
			set_bit(LRUVEC_NODE_CONGESTED, &target_lruvec->flags);
	}

	/*
	 * Stall direct reclaim for IO completions if the lruvec is
	 * node is congested. Allow kswapd to continue until it
	 * starts encountering unqueued dirty pages or cycling through
	 * the LRU too quickly.
	 */
	if (!current_is_kswapd() && current_may_throttle() &&
	    !sc->hibernation_mode &&
	    (test_bit(LRUVEC_CGROUP_CONGESTED, &target_lruvec->flags) ||
	     test_bit(LRUVEC_NODE_CONGESTED, &target_lruvec->flags)))
		reclaim_throttle(pgdat, VMSCAN_THROTTLE_CONGESTED);

	if (should_continue_reclaim(pgdat, nr_node_reclaimed, sc))
		goto again;

	/*
	 * Kswapd gives up on balancing particular nodes after too
	 * many failures to reclaim anything from them and goes to
	 * sleep. On reclaim progress, reset the failure counter. A
	 * successful direct reclaim run will revive a dormant kswapd.
	 */
	if (reclaimable)
		kswapd_try_clear_hopeless(pgdat, sc->order, sc->reclaim_idx);
	else if (sc->cache_trim_mode)
		sc->cache_trim_mode_failed = 1;
}

void lru_snapshot_refaults(struct mem_cgroup *target_memcg, pg_data_t *pgdat)
{
	struct lruvec *target_lruvec;
	unsigned long refaults;

	target_lruvec = mem_cgroup_lruvec(target_memcg, pgdat);
	refaults = lruvec_page_state(target_lruvec, WORKINGSET_ACTIVATE_ANON);
	target_lruvec->refaults[WORKINGSET_ANON] = refaults;
	refaults = lruvec_page_state(target_lruvec, WORKINGSET_ACTIVATE_FILE);
	target_lruvec->refaults[WORKINGSET_FILE] = refaults;
}

void lru_age_node(struct pglist_data *pgdat, struct scan_control *sc)
{
	struct mem_cgroup *memcg;
	struct lruvec *lruvec;

	lruvec = mem_cgroup_lruvec(NULL, pgdat);
	if (!can_age_anon_pages(lruvec, sc))
		return;

	if (!inactive_is_low(lruvec, LRU_INACTIVE_ANON))
		return;

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		lruvec = mem_cgroup_lruvec(memcg, pgdat);
		shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
				   sc, LRU_ACTIVE_ANON);
		memcg = mem_cgroup_iter(NULL, memcg, NULL);
	} while (memcg);
}
