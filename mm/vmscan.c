// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, Stephen Tweedie.
 *  kswapd added: 7.1.96  sct
 *  Removed kswapd_ctl limits, and swap out as many pages as needed
 *  to bring the system back to freepages.high: 2.4.97, Rik van Riel.
 *  Zone aware kswapd started 02/00, Kanoj Sarcar (kanoj@sgi.com).
 *  Multiqueue VM started 5.8.00, Rik van Riel.
 */

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

#define CREATE_TRACE_POINTS
#include <trace/events/vmscan.h>

/*
 * From 0 .. MAX_SWAPPINESS.  Higher means more swappy.
 */
int vm_swappiness = 60;

void set_task_reclaim_state(struct task_struct *task,
				   struct reclaim_state *rs)
{
	/* Check for an overwrite */
	WARN_ON_ONCE(rs && task->reclaim_state);

	/* Check for the nulling of an already-nulled member */
	WARN_ON_ONCE(!rs && !task->reclaim_state);

	task->reclaim_state = rs;
}

/*
 * flush_reclaim_state(): add pages reclaimed outside of LRU-based reclaim to
 * scan_control->nr_reclaimed.
 */
void flush_reclaim_state(struct scan_control *sc)
{
	/*
	 * Currently, reclaim_state->reclaimed includes three types of pages
	 * freed outside of vmscan:
	 * (1) Slab pages.
	 * (2) Clean file pages from pruned inodes (on highmem systems).
	 * (3) XFS freed buffer pages.
	 *
	 * For all of these cases, we cannot universally link the pages to a
	 * single memcg. For example, a memcg-aware shrinker can free one object
	 * charged to the target memcg, causing an entire page to be freed.
	 * If we count the entire page as reclaimed from the memcg, we end up
	 * overestimating the reclaimed amount (potentially under-reclaiming).
	 *
	 * Only count such pages for global reclaim to prevent under-reclaiming
	 * from the target memcg; preventing unnecessary retries during memcg
	 * charging and false positives from proactive reclaim.
	 *
	 * For uncommon cases where the freed pages were actually mostly
	 * charged to the target memcg, we end up underestimating the reclaimed
	 * amount. This should be fine. The freed pages will be uncharged
	 * anyway, even if they are not counted here properly, and we will be
	 * able to make forward progress in charging (which is usually in a
	 * retry loop).
	 *
	 * We can go one step further, and report the uncharged objcg pages in
	 * memcg reclaim, to make reporting more accurate and reduce
	 * underestimation, but it's probably not worth the complexity for now.
	 */
	if (current->reclaim_state && root_reclaim(sc)) {
		sc->nr_reclaimed += current->reclaim_state->reclaimed;
		current->reclaim_state->reclaimed = 0;
	}
}

bool can_demote(int nid, struct scan_control *sc,
		       struct mem_cgroup *memcg)
{
	struct pglist_data *pgdat = NODE_DATA(nid);
	nodemask_t allowed_mask;

	if (!pgdat || !numa_demotion_enabled)
		return false;
	if (sc && sc->no_demotion)
		return false;

	node_get_allowed_targets(pgdat, &allowed_mask);
	if (nodes_empty(allowed_mask))
		return false;

	/* Filter out nodes that are not in cgroup's mems_allowed. */
	mem_cgroup_node_filter_allowed(memcg, &allowed_mask);
	return !nodes_empty(allowed_mask);
}

/*
 * This misses isolated folios which are not accounted for to save counters.
 * As the data only determines if reclaim or compaction continues, it is
 * not expected that isolated folios will be a dominating factor.
 */
unsigned long zone_reclaimable_pages(struct zone *zone)
{
	unsigned long nr;

	nr = zone_page_state_snapshot(zone, NR_ZONE_INACTIVE_FILE) +
		zone_page_state_snapshot(zone, NR_ZONE_ACTIVE_FILE);
	if (can_reclaim_anon_pages(NULL, zone_to_nid(zone), NULL))
		nr += zone_page_state_snapshot(zone, NR_ZONE_INACTIVE_ANON) +
			zone_page_state_snapshot(zone, NR_ZONE_ACTIVE_ANON);

	return nr;
}

static unsigned long drop_slab_node(int nid)
{
	unsigned long freed = 0;
	struct mem_cgroup *memcg = NULL;

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		freed += shrink_slab(GFP_KERNEL, nid, memcg, 0);
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)) != NULL);

	return freed;
}

void drop_slab(void)
{
	int nid;
	int shift = 0;
	unsigned long freed;

	do {
		freed = 0;
		for_each_online_node(nid) {
			if (fatal_signal_pending(current))
				return;

			freed += drop_slab_node(nid);
		}
	} while ((freed >> shift++) > 1);
}

#define CHECK_RECLAIMER_OFFSET(type)					\
	do {								\
		BUILD_BUG_ON(PGSTEAL_##type - PGSTEAL_KSWAPD !=		\
			     PGDEMOTE_##type - PGDEMOTE_KSWAPD);	\
		BUILD_BUG_ON(PGSTEAL_##type - PGSTEAL_KSWAPD !=		\
			     PGSCAN_##type - PGSCAN_KSWAPD);		\
	} while (0)

int reclaimer_offset(struct scan_control *sc)
{
	CHECK_RECLAIMER_OFFSET(DIRECT);
	CHECK_RECLAIMER_OFFSET(KHUGEPAGED);
	CHECK_RECLAIMER_OFFSET(PROACTIVE);

	if (current_is_kswapd())
		return 0;
	if (current_is_khugepaged())
		return PGSTEAL_KHUGEPAGED - PGSTEAL_KSWAPD;
	if (sc->proactive)
		return PGSTEAL_PROACTIVE - PGSTEAL_KSWAPD;
	return PGSTEAL_DIRECT - PGSTEAL_KSWAPD;
}

/*
 * We detected a synchronous write error writing a folio out.  Probably
 * -ENOSPC.  We need to propagate that into the address_space for a subsequent
 * fsync(), msync() or close().
 *
 * The tricky part is that after writepage we cannot touch the mapping: nothing
 * prevents it from being freed up.  But we have a ref on the folio and once
 * that folio is locked, the mapping is pinned.
 *
 * We're allowed to run sleeping folio_lock() here because we know the caller has
 * __GFP_FS.
 */
static void handle_write_error(struct address_space *mapping,
				struct folio *folio, int error)
{
	folio_lock(folio);
	if (folio_mapping(folio) == mapping)
		mapping_set_error(mapping, error);
	folio_unlock(folio);
}

static bool skip_throttle_noprogress(pg_data_t *pgdat)
{
	int reclaimable = 0, write_pending = 0;
	int i;
	struct zone *zone;
	/*
	 * If kswapd is disabled, reschedule if necessary but do not
	 * throttle as the system is likely near OOM.
	 */
	if (kswapd_test_hopeless(pgdat))
		return true;

	/*
	 * If there are a lot of dirty/writeback folios then do not
	 * throttle as throttling will occur when the folios cycle
	 * towards the end of the LRU if still under writeback.
	 */
	for_each_managed_zone_pgdat(zone, pgdat, i, MAX_NR_ZONES - 1) {
		reclaimable += zone_reclaimable_pages(zone);
		write_pending += zone_page_state_snapshot(zone,
						  NR_ZONE_WRITE_PENDING);
	}
	if (2 * write_pending <= reclaimable)
		return true;

	return false;
}

void reclaim_throttle(pg_data_t *pgdat, enum vmscan_throttle_state reason)
{
	wait_queue_head_t *wqh = &pgdat->reclaim_wait[reason];
	long timeout, ret;
	DEFINE_WAIT(wait);

	/*
	 * Do not throttle user workers, kthreads other than kswapd or
	 * workqueues. They may be required for reclaim to make
	 * forward progress (e.g. journalling workqueues or kthreads).
	 */
	if (!current_is_kswapd() &&
	    current->flags & (PF_USER_WORKER|PF_KTHREAD)) {
		cond_resched();
		return;
	}

	/*
	 * These figures are pulled out of thin air.
	 * VMSCAN_THROTTLE_ISOLATED is a transient condition based on too many
	 * parallel reclaimers which is a short-lived event so the timeout is
	 * short. Failing to make progress or waiting on writeback are
	 * potentially long-lived events so use a longer timeout. This is shaky
	 * logic as a failure to make progress could be due to anything from
	 * writeback to a slow device to excessive referenced folios at the tail
	 * of the inactive LRU.
	 */
	switch(reason) {
	case VMSCAN_THROTTLE_WRITEBACK:
		timeout = HZ/10;

		if (atomic_inc_return(&pgdat->nr_writeback_throttled) == 1) {
			WRITE_ONCE(pgdat->nr_reclaim_start,
				node_page_state(pgdat, NR_THROTTLED_WRITTEN));
		}

		break;
	case VMSCAN_THROTTLE_CONGESTED:
		fallthrough;
	case VMSCAN_THROTTLE_NOPROGRESS:
		if (skip_throttle_noprogress(pgdat)) {
			cond_resched();
			return;
		}

		timeout = 1;

		break;
	case VMSCAN_THROTTLE_ISOLATED:
		timeout = HZ/50;
		break;
	default:
		WARN_ON_ONCE(1);
		timeout = HZ;
		break;
	}

	prepare_to_wait(wqh, &wait, TASK_UNINTERRUPTIBLE);
	ret = schedule_timeout(timeout);
	finish_wait(wqh, &wait);

	if (reason == VMSCAN_THROTTLE_WRITEBACK)
		atomic_dec(&pgdat->nr_writeback_throttled);

	trace_mm_vmscan_throttled(pgdat->node_id, jiffies_to_usecs(timeout),
				jiffies_to_usecs(timeout - ret),
				reason);
}

/*
 * Account for folios written if tasks are throttled waiting on dirty
 * folios to clean. If enough folios have been cleaned since throttling
 * started then wakeup the throttled tasks.
 */
void __acct_reclaim_writeback(pg_data_t *pgdat, struct folio *folio,
							int nr_throttled)
{
	unsigned long nr_written;

	node_stat_add_folio(folio, NR_THROTTLED_WRITTEN);

	/*
	 * This is an inaccurate read as the per-cpu deltas may not
	 * be synchronised. However, given that the system is
	 * writeback throttled, it is not worth taking the penalty
	 * of getting an accurate count. At worst, the throttle
	 * timeout guarantees forward progress.
	 */
	nr_written = node_page_state(pgdat, NR_THROTTLED_WRITTEN) -
		READ_ONCE(pgdat->nr_reclaim_start);

	if (nr_written > SWAP_CLUSTER_MAX * nr_throttled)
		wake_up(&pgdat->reclaim_wait[VMSCAN_THROTTLE_WRITEBACK]);
}

/* possible outcome of pageout() */
typedef enum {
	/* failed to write folio out, folio is locked */
	PAGE_KEEP,
	/* move folio to the active list, folio is locked */
	PAGE_ACTIVATE,
	/* folio has been sent to the disk successfully, folio is unlocked */
	PAGE_SUCCESS,
	/* folio is clean and locked */
	PAGE_CLEAN,
} pageout_t;

static pageout_t writeout(struct folio *folio, struct address_space *mapping,
		struct swap_iocb **plug, struct list_head *folio_list)
{
	int res;

	folio_set_reclaim(folio);

	/*
	 * The large shmem folio can be split if CONFIG_THP_SWAP is not enabled
	 * or we failed to allocate contiguous swap entries, in which case
	 * the split out folios get added back to folio_list.
	 */
	if (shmem_mapping(mapping))
		res = shmem_writeout(folio, plug, folio_list);
	else
		res = swap_writeout(folio, plug);

	if (res < 0)
		handle_write_error(mapping, folio, res);
	if (res == AOP_WRITEPAGE_ACTIVATE) {
		folio_clear_reclaim(folio);
		return PAGE_ACTIVATE;
	}

	/* synchronous write? */
	if (!folio_test_writeback(folio))
		folio_clear_reclaim(folio);

	trace_mm_vmscan_write_folio(folio);
	node_stat_add_folio(folio, NR_VMSCAN_WRITE);
	return PAGE_SUCCESS;
}

/*
 * pageout is called by shrink_folio_list() for each dirty folio.
 */
static pageout_t pageout(struct folio *folio, struct address_space *mapping,
			 struct swap_iocb **plug, struct list_head *folio_list)
{
	/*
	 * We no longer attempt to writeback filesystem folios here, other
	 * than tmpfs/shmem.  That's taken care of in page-writeback.
	 * If we find a dirty filesystem folio at the end of the LRU list,
	 * typically that means the filesystem is saturating the storage
	 * with contiguous writes and telling it to write a folio here
	 * would only make the situation worse by injecting an element
	 * of random access.
	 *
	 * If the folio is swapcache, write it back even if that would
	 * block, for some throttling. This happens by accident, because
	 * swap_backing_dev_info is bust: it doesn't reflect the
	 * congestion state of the swapdevs.  Easy to fix, if needed.
	 *
	 * A freeable shmem or swapcache folio is referenced only by the
	 * caller that isolated the folio and the page cache.
	 */
	if (folio_ref_count(folio) != 1 + folio_nr_pages(folio) || !mapping)
		return PAGE_KEEP;
	if (!shmem_mapping(mapping) && !folio_test_anon(folio))
		return PAGE_ACTIVATE;
	if (!folio_clear_dirty_for_io(folio))
		return PAGE_CLEAN;
	return writeout(folio, mapping, plug, folio_list);
}

/*
 * Same as remove_mapping, but if the folio is removed from the mapping, it
 * gets returned with a refcount of 0.
 */
static int __remove_mapping(struct address_space *mapping, struct folio *folio,
			    bool reclaimed, struct mem_cgroup *target_memcg)
{
	int refcount;
	void *shadow = NULL;
	struct swap_cluster_info *ci;

	BUG_ON(!folio_test_locked(folio));
	BUG_ON(mapping != folio_mapping(folio));

	if (folio_test_swapcache(folio)) {
		ci = swap_cluster_get_and_lock_irq(folio);
	} else {
		spin_lock(&mapping->host->i_lock);
		xa_lock_irq(&mapping->i_pages);
	}

	/*
	 * The non racy check for a busy folio.
	 *
	 * Must be careful with the order of the tests. When someone has
	 * a ref to the folio, it may be possible that they dirty it then
	 * drop the reference. So if the dirty flag is tested before the
	 * refcount here, then the following race may occur:
	 *
	 * get_user_pages(&page);
	 * [user mapping goes away]
	 * write_to(page);
	 *				!folio_test_dirty(folio)    [good]
	 * folio_set_dirty(folio);
	 * folio_put(folio);
	 *				!refcount(folio)   [good, discard it]
	 *
	 * [oops, our write_to data is lost]
	 *
	 * Reversing the order of the tests ensures such a situation cannot
	 * escape unnoticed. The smp_rmb is needed to ensure the folio->flags
	 * load is not satisfied before that of folio->_refcount.
	 *
	 * Note that if the dirty flag is always set via folio_mark_dirty,
	 * and thus under the i_pages lock, then this ordering is not required.
	 */
	refcount = 1 + folio_nr_pages(folio);
	if (!folio_ref_freeze(folio, refcount))
		goto cannot_free;
	/* note: atomic_cmpxchg in folio_ref_freeze provides the smp_rmb */
	if (unlikely(folio_test_dirty(folio))) {
		folio_ref_unfreeze(folio, refcount);
		goto cannot_free;
	}

	if (folio_test_swapcache(folio)) {
		swp_entry_t swap = folio->swap;

		if (reclaimed && !mapping_exiting(mapping))
			shadow = workingset_eviction(folio, target_memcg);
		memcg1_swapout(folio, swap);
		__swap_cache_del_folio(ci, folio, swap, shadow);
		swap_cluster_unlock_irq(ci);
	} else {
		void (*free_folio)(struct folio *);

		free_folio = mapping->a_ops->free_folio;
		/*
		 * Remember a shadow entry for reclaimed file cache in
		 * order to detect refaults, thus thrashing, later on.
		 *
		 * But don't store shadows in an address space that is
		 * already exiting.  This is not just an optimization,
		 * inode reclaim needs to empty out the radix tree or
		 * the nodes are lost.  Don't plant shadows behind its
		 * back.
		 *
		 * We also don't store shadows for DAX mappings because the
		 * only page cache folios found in these are zero pages
		 * covering holes, and because we don't want to mix DAX
		 * exceptional entries and shadow exceptional entries in the
		 * same address_space.
		 */
		if (reclaimed && folio_is_file_lru(folio) &&
		    !mapping_exiting(mapping) && !dax_mapping(mapping))
			shadow = workingset_eviction(folio, target_memcg);
		__filemap_remove_folio(folio, shadow);
		xa_unlock_irq(&mapping->i_pages);
		if (mapping_shrinkable(mapping))
			inode_lru_list_add(mapping->host);
		spin_unlock(&mapping->host->i_lock);

		if (free_folio)
			free_folio(folio);
	}

	return 1;

cannot_free:
	if (folio_test_swapcache(folio)) {
		swap_cluster_unlock_irq(ci);
	} else {
		xa_unlock_irq(&mapping->i_pages);
		spin_unlock(&mapping->host->i_lock);
	}
	return 0;
}

/**
 * remove_mapping() - Attempt to remove a folio from its mapping.
 * @mapping: The address space.
 * @folio: The folio to remove.
 *
 * If the folio is dirty, under writeback or if someone else has a ref
 * on it, removal will fail.
 * Return: The number of pages removed from the mapping.  0 if the folio
 * could not be removed.
 * Context: The caller should have a single refcount on the folio and
 * hold its lock.
 */
long remove_mapping(struct address_space *mapping, struct folio *folio)
{
	if (__remove_mapping(mapping, folio, false, NULL)) {
		/*
		 * Unfreezing the refcount with 1 effectively
		 * drops the pagecache ref for us without requiring another
		 * atomic operation.
		 */
		folio_ref_unfreeze(folio, 1);
		return folio_nr_pages(folio);
	}
	return 0;
}

/**
 * folio_putback_lru - Put previously isolated folio onto appropriate LRU list.
 * @folio: Folio to be returned to an LRU list.
 *
 * Add previously isolated @folio to appropriate LRU list.
 * The folio may still be unevictable for other reasons.
 *
 * Context: lru_lock must not be held, interrupts must be enabled.
 */
void folio_putback_lru(struct folio *folio)
{
	folio_add_lru(folio);
	folio_put(folio);		/* drop ref from isolate */
}

/* Check if a folio is dirty or under writeback */
static void folio_check_dirty_writeback(struct folio *folio,
				       bool *dirty, bool *writeback)
{
	struct address_space *mapping;

	/*
	 * Anonymous folios are not handled by flushers and must be written
	 * from reclaim context. Do not stall reclaim based on them.
	 * MADV_FREE anonymous folios are put into inactive file list too.
	 * They could be mistakenly treated as file lru. So further anon
	 * test is needed.
	 */
	if (!folio_is_file_lru(folio) ||
	    (folio_test_anon(folio) && !folio_test_swapbacked(folio))) {
		*dirty = false;
		*writeback = false;
		return;
	}

	/* By default assume that the folio flags are accurate */
	*dirty = folio_test_dirty(folio);
	*writeback = folio_test_writeback(folio);

	/* Verify dirty/writeback state if the filesystem supports it */
	if (!folio_test_private(folio))
		return;

	mapping = folio_mapping(folio);
	if (mapping && mapping->a_ops->is_dirty_writeback)
		mapping->a_ops->is_dirty_writeback(folio, dirty, writeback);
}

static struct folio *alloc_demote_folio(struct folio *src,
		unsigned long private)
{
	struct folio *dst;
	nodemask_t *allowed_mask;
	struct migration_target_control *mtc;

	mtc = (struct migration_target_control *)private;

	allowed_mask = mtc->nmask;
	/*
	 * make sure we allocate from the target node first also trying to
	 * demote or reclaim pages from the target node via kswapd if we are
	 * low on free memory on target node. If we don't do this and if
	 * we have free memory on the slower(lower) memtier, we would start
	 * allocating pages from slower(lower) memory tiers without even forcing
	 * a demotion of cold pages from the target memtier. This can result
	 * in the kernel placing hot pages in slower(lower) memory tiers.
	 */
	mtc->nmask = NULL;
	mtc->gfp_mask |= __GFP_THISNODE;
	dst = alloc_migration_target(src, (unsigned long)mtc);
	if (dst)
		return dst;

	mtc->gfp_mask &= ~__GFP_THISNODE;
	mtc->nmask = allowed_mask;

	return alloc_migration_target(src, (unsigned long)mtc);
}

/*
 * Take folios on @demote_folios and attempt to demote them to another node.
 * Folios which are not demoted are left on @demote_folios.
 */
static unsigned int demote_folio_list(struct list_head *demote_folios,
				      struct pglist_data *pgdat,
				      struct mem_cgroup *memcg)
{
	int target_nid;
	unsigned int nr_succeeded;
	nodemask_t allowed_mask;

	struct migration_target_control mtc = {
		/*
		 * Allocate from 'node', or fail quickly and quietly.
		 * When this happens, 'page' will likely just be discarded
		 * instead of migrated.
		 */
		.gfp_mask = (GFP_HIGHUSER_MOVABLE & ~__GFP_RECLAIM) |
			__GFP_NOMEMALLOC | GFP_NOWAIT,
		.nmask = &allowed_mask,
		.reason = MR_DEMOTION,
	};

	if (list_empty(demote_folios))
		return 0;

	node_get_allowed_targets(pgdat, &allowed_mask);
	mem_cgroup_node_filter_allowed(memcg, &allowed_mask);
	if (nodes_empty(allowed_mask))
		return 0;

	target_nid = next_demotion_node(pgdat->node_id, &allowed_mask);
	if (target_nid == NUMA_NO_NODE)
		/* No lower-tier nodes or nodes were hot-unplugged. */
		return 0;

	mtc.nid = target_nid;

	/* Demotion ignores all cpuset and mempolicy settings */
	migrate_pages(demote_folios, alloc_demote_folio, NULL,
		      (unsigned long)&mtc, MIGRATE_ASYNC, MR_DEMOTION,
		      &nr_succeeded);

	return nr_succeeded;
}

static bool may_enter_fs(struct folio *folio, gfp_t gfp_mask)
{
	if (gfp_mask & __GFP_FS)
		return true;
	if (!folio_test_swapcache(folio) || !(gfp_mask & __GFP_IO))
		return false;
	/*
	 * We can "enter_fs" for swap-cache with only __GFP_IO
	 * providing this isn't SWP_FS_OPS.
	 * ->flags can be updated non-atomically (scan_swap_map_slots),
	 * but that will never affect SWP_FS_OPS, so the data_race
	 * is safe.
	 */
	return !data_race(folio_swap_flags(folio) & SWP_FS_OPS);
}

/*
 * shrink_folio_list() returns the number of reclaimed pages
 */
unsigned int shrink_folio_list(struct list_head *folio_list,
		struct pglist_data *pgdat, struct scan_control *sc,
		struct reclaim_stat *stat, bool ignore_references,
		struct mem_cgroup *memcg)
{
	struct folio_batch free_folios;
	LIST_HEAD(ret_folios);
	LIST_HEAD(demote_folios);
	unsigned int nr_reclaimed = 0, nr_demoted = 0;
	unsigned int pgactivate = 0;
	bool do_demote_pass;
	struct swap_iocb *plug = NULL;

	folio_batch_init(&free_folios);
	memset(stat, 0, sizeof(*stat));
	cond_resched();
	do_demote_pass = can_demote(pgdat->node_id, sc, memcg);

retry:
	while (!list_empty(folio_list)) {
		struct address_space *mapping;
		struct folio *folio;
		enum folio_references references = FOLIOREF_RECLAIM;
		bool dirty, writeback;
		unsigned int nr_pages;

		cond_resched();

		folio = lru_to_folio(folio_list);
		list_del(&folio->lru);

		if (!folio_trylock(folio))
			goto keep;

		if (folio_contain_hwpoisoned_page(folio)) {
			/*
			 * unmap_poisoned_folio() can't handle large
			 * folio, just skip it. memory_failure() will
			 * handle it if the UCE is triggered again.
			 */
			if (folio_test_large(folio))
				goto keep_locked;

			unmap_poisoned_folio(folio, folio_pfn(folio), false);
			folio_unlock(folio);
			folio_put(folio);
			continue;
		}

		VM_BUG_ON_FOLIO(folio_test_active(folio), folio);

		nr_pages = folio_nr_pages(folio);

		/* Account the number of base pages */
		sc->nr_scanned += nr_pages;

		if (unlikely(!folio_evictable(folio)))
			goto activate_locked;

		if (!sc->may_unmap && folio_mapped(folio))
			goto keep_locked;

		/*
		 * The number of dirty pages determines if a node is marked
		 * reclaim_congested. kswapd will stall and start writing
		 * folios if the tail of the LRU is all dirty unqueued folios.
		 */
		folio_check_dirty_writeback(folio, &dirty, &writeback);
		if (dirty || writeback)
			stat->nr_dirty += nr_pages;

		if (dirty && !writeback)
			stat->nr_unqueued_dirty += nr_pages;

		/*
		 * Treat this folio as congested if folios are cycling
		 * through the LRU so quickly that the folios marked
		 * for immediate reclaim are making it to the end of
		 * the LRU a second time.
		 */
		if (writeback && folio_test_reclaim(folio))
			stat->nr_congested += nr_pages;

		/*
		 * If a folio at the tail of the LRU is under writeback, there
		 * are three cases to consider.
		 *
		 * 1) If reclaim is encountering an excessive number
		 *    of folios under writeback and this folio has both
		 *    the writeback and reclaim flags set, then it
		 *    indicates that folios are being queued for I/O but
		 *    are being recycled through the LRU before the I/O
		 *    can complete. Waiting on the folio itself risks an
		 *    indefinite stall if it is impossible to writeback
		 *    the folio due to I/O error or disconnected storage
		 *    so instead note that the LRU is being scanned too
		 *    quickly and the caller can stall after the folio
		 *    list has been processed.
		 *
		 * 2) Global or new memcg reclaim encounters a folio that is
		 *    not marked for immediate reclaim, or the caller does not
		 *    have __GFP_FS (or __GFP_IO if it's simply going to swap,
		 *    not to fs), or the folio belongs to a mapping where
		 *    waiting on writeback during reclaim may lead to a deadlock.
		 *    In this case mark the folio for immediate reclaim and
		 *    continue scanning.
		 *
		 *    Require may_enter_fs() because we would wait on fs, which
		 *    may not have submitted I/O yet. And the loop driver might
		 *    enter reclaim, and deadlock if it waits on a folio for
		 *    which it is needed to do the write (loop masks off
		 *    __GFP_IO|__GFP_FS for this reason); but more thought
		 *    would probably show more reasons.
		 *
		 * 3) Legacy memcg encounters a folio that already has the
		 *    reclaim flag set. memcg does not have any dirty folio
		 *    throttling so we could easily OOM just because too many
		 *    folios are in writeback and there is nothing else to
		 *    reclaim. Wait for the writeback to complete.
		 *
		 * In cases 1) and 2) we activate the folios to get them out of
		 * the way while we continue scanning for clean folios on the
		 * inactive list and refilling from the active list. The
		 * observation here is that waiting for disk writes is more
		 * expensive than potentially causing reloads down the line.
		 * Since they're marked for immediate reclaim, they won't put
		 * memory pressure on the cache working set any longer than it
		 * takes to write them to disk.
		 */
		if (folio_test_writeback(folio)) {
			mapping = folio_mapping(folio);

			/* Case 1 above */
			if (current_is_kswapd() &&
			    folio_test_reclaim(folio) &&
			    test_bit(PGDAT_WRITEBACK, &pgdat->flags)) {
				stat->nr_immediate += nr_pages;
				goto activate_locked;

			/* Case 2 above */
			} else if (writeback_throttling_sane(sc) ||
			    !folio_test_reclaim(folio) ||
			    !may_enter_fs(folio, sc->gfp_mask) ||
			    (mapping &&
			     mapping_writeback_may_deadlock_on_reclaim(mapping))) {
				/*
				 * This is slightly racy -
				 * folio_end_writeback() might have
				 * just cleared the reclaim flag, then
				 * setting the reclaim flag here ends up
				 * interpreted as the readahead flag - but
				 * that does not matter enough to care.
				 * What we do want is for this folio to
				 * have the reclaim flag set next time
				 * memcg reclaim reaches the tests above,
				 * so it will then wait for writeback to
				 * avoid OOM; and it's also appropriate
				 * in global reclaim.
				 */
				folio_set_reclaim(folio);
				stat->nr_writeback += nr_pages;
				goto activate_locked;

			/* Case 3 above */
			} else {
				folio_unlock(folio);
				folio_wait_writeback(folio);
				/* then go back and try same folio again */
				list_add_tail(&folio->lru, folio_list);
				continue;
			}
		}

		if (ignore_references)
			goto ignore_refcheck;

		if (lru_gen_enabled())
			references = lru_gen_folio_check_references(folio, sc);
		else
			references = lru_folio_check_references(folio, sc);

ignore_refcheck:
		switch (references) {
		case FOLIOREF_ACTIVATE:
			goto activate_locked;
		case FOLIOREF_KEEP:
			stat->nr_ref_keep += nr_pages;
			goto keep_locked;
		case FOLIOREF_RECLAIM:
		case FOLIOREF_RECLAIM_CLEAN:
			; /* try to reclaim the folio below */
		}

		/*
		 * Before reclaiming the folio, try to relocate
		 * its contents to another node.
		 */
		if (do_demote_pass &&
		    (thp_migration_supported() || !folio_test_large(folio))) {
			list_add(&folio->lru, &demote_folios);
			folio_unlock(folio);
			continue;
		}

		/*
		 * Anonymous process memory has backing store?
		 * Try to allocate it some swap space here.
		 * Lazyfree folio could be freed directly
		 */
		if (folio_test_anon(folio) && folio_test_swapbacked(folio) &&
				!folio_test_swapcache(folio)) {
			if (!(sc->gfp_mask & __GFP_IO))
				goto keep_locked;
			if (folio_maybe_dma_pinned(folio))
				goto keep_locked;
			if (folio_test_large(folio)) {
				/* cannot split folio, skip it */
				if (folio_expected_ref_count(folio) !=
				    folio_ref_count(folio) - 1)
					goto activate_locked;
				/*
				 * Split partially mapped folios right away.
				 * We can free the unmapped pages without IO.
				 */
				if (data_race(!list_empty(&folio->_deferred_list) &&
				    folio_test_partially_mapped(folio)) &&
				    split_folio_to_list(folio, folio_list))
					goto activate_locked;
			}
			if (folio_alloc_swap(folio)) {
				int __maybe_unused order = folio_order(folio);

				if (!folio_test_large(folio))
					goto activate_locked_split;
				/* Fallback to swap normal pages */
				if (split_folio_to_list(folio, folio_list))
					goto activate_locked;
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
				if (nr_pages >= HPAGE_PMD_NR) {
					count_memcg_folio_events(folio,
						THP_SWPOUT_FALLBACK, 1);
					count_vm_event(THP_SWPOUT_FALLBACK);
				}
#endif
				count_mthp_stat(order, MTHP_STAT_SWPOUT_FALLBACK);
				if (folio_alloc_swap(folio))
					goto activate_locked_split;
			}
			/*
			 * Normally the folio will be dirtied in unmap because
			 * its pte should be dirty. A special case is MADV_FREE
			 * page. The page's pte could have dirty bit cleared but
			 * the folio's SwapBacked flag is still set because
			 * clearing the dirty bit and SwapBacked flag has no
			 * lock protected. For such folio, unmap will not set
			 * dirty bit for it, so folio reclaim will not write the
			 * folio out. This can cause data corruption when the
			 * folio is swapped in later. Always setting the dirty
			 * flag for the folio solves the problem.
			 */
			folio_mark_dirty(folio);
		}

		/*
		 * If the folio was split above, the tail pages will make
		 * their own pass through this function and be accounted
		 * then.
		 */
		if ((nr_pages > 1) && !folio_test_large(folio)) {
			sc->nr_scanned -= (nr_pages - 1);
			nr_pages = 1;
		}

		/*
		 * The folio is mapped into the page tables of one or more
		 * processes. Try to unmap it here.
		 */
		if (folio_mapped(folio)) {
			enum ttu_flags flags = TTU_BATCH_FLUSH;
			bool was_swapbacked = folio_test_swapbacked(folio);

			if (folio_test_pmd_mappable(folio))
				flags |= TTU_SPLIT_HUGE_PMD;
			/*
			 * Without TTU_SYNC, try_to_unmap will only begin to
			 * hold PTL from the first present PTE within a large
			 * folio. Some initial PTEs might be skipped due to
			 * races with parallel PTE writes in which PTEs can be
			 * cleared temporarily before being written new present
			 * values. This will lead to a large folio is still
			 * mapped while some subpages have been partially
			 * unmapped after try_to_unmap; TTU_SYNC helps
			 * try_to_unmap acquire PTL from the first PTE,
			 * eliminating the influence of temporary PTE values.
			 */
			if (folio_test_large(folio))
				flags |= TTU_SYNC;

			try_to_unmap(folio, flags);
			if (folio_mapped(folio)) {
				stat->nr_unmap_fail += nr_pages;
				if (!was_swapbacked &&
				    folio_test_swapbacked(folio))
					stat->nr_lazyfree_fail += nr_pages;
				goto activate_locked;
			}
		}

		/*
		 * Folio is unmapped now so it cannot be newly pinned anymore.
		 * No point in trying to reclaim folio if it is pinned.
		 * Furthermore we don't want to reclaim underlying fs metadata
		 * if the folio is pinned and thus potentially modified by the
		 * pinning process as that may upset the filesystem.
		 */
		if (folio_maybe_dma_pinned(folio))
			goto activate_locked;

		mapping = folio_mapping(folio);
		if (folio_test_dirty(folio)) {
			if (folio_is_file_lru(folio)) {
				/*
				 * Immediately reclaim when written back.
				 * Similar in principle to folio_deactivate()
				 * except we already have the folio isolated
				 * and know it's dirty
				 */
				node_stat_mod_folio(folio, NR_VMSCAN_IMMEDIATE,
						nr_pages);
				if (!folio_test_reclaim(folio))
					folio_set_reclaim(folio);

				goto activate_locked;
			}

			if (references == FOLIOREF_RECLAIM_CLEAN)
				goto keep_locked;
			if (!may_enter_fs(folio, sc->gfp_mask))
				goto keep_locked;
			if (!sc->may_writepage)
				goto keep_locked;

			/*
			 * Folio is dirty. Flush the TLB if a writable entry
			 * potentially exists to avoid CPU writes after I/O
			 * starts and then write it out here.
			 */
			try_to_unmap_flush_dirty();
			switch (pageout(folio, mapping, &plug, folio_list)) {
			case PAGE_KEEP:
				goto keep_locked;
			case PAGE_ACTIVATE:
				/*
				 * If shmem folio is split when writeback to swap,
				 * the tail pages will make their own pass through
				 * this function and be accounted then.
				 */
				if (nr_pages > 1 && !folio_test_large(folio)) {
					sc->nr_scanned -= (nr_pages - 1);
					nr_pages = 1;
				}
				goto activate_locked;
			case PAGE_SUCCESS:
				if (nr_pages > 1 && !folio_test_large(folio)) {
					sc->nr_scanned -= (nr_pages - 1);
					nr_pages = 1;
				}
				stat->nr_pageout += nr_pages;

				if (folio_test_writeback(folio))
					goto keep;
				if (folio_test_dirty(folio))
					goto keep;

				/*
				 * A synchronous write - probably a ramdisk.  Go
				 * ahead and try to reclaim the folio.
				 */
				if (!folio_trylock(folio))
					goto keep;
				if (folio_test_dirty(folio) ||
				    folio_test_writeback(folio))
					goto keep_locked;
				mapping = folio_mapping(folio);
				fallthrough;
			case PAGE_CLEAN:
				; /* try to free the folio below */
			}
		}

		/*
		 * If the folio has buffers, try to free the buffer
		 * mappings associated with this folio. If we succeed
		 * we try to free the folio as well.
		 *
		 * We do this even if the folio is dirty.
		 * filemap_release_folio() does not perform I/O, but it
		 * is possible for a folio to have the dirty flag set,
		 * but it is actually clean (all its buffers are clean).
		 * This happens if the buffers were written out directly,
		 * with submit_bh(). ext3 will do this, as well as
		 * the blockdev mapping.  filemap_release_folio() will
		 * discover that cleanness and will drop the buffers
		 * and mark the folio clean - it can be freed.
		 *
		 * Rarely, folios can have buffers and no ->mapping.
		 * These are the folios which were not successfully
		 * invalidated in truncate_cleanup_folio().  We try to
		 * drop those buffers here and if that worked, and the
		 * folio is no longer mapped into process address space
		 * (refcount == 1) it can be freed.  Otherwise, leave
		 * the folio on the LRU so it is swappable.
		 */
		if (folio_needs_release(folio)) {
			if (!filemap_release_folio(folio, sc->gfp_mask))
				goto activate_locked;
			if (!mapping && folio_ref_count(folio) == 1) {
				folio_unlock(folio);
				if (folio_put_testzero(folio))
					goto free_it;
				else {
					/*
					 * rare race with speculative reference.
					 * the speculative reference will free
					 * this folio shortly, so we may
					 * increment nr_reclaimed here (and
					 * leave it off the LRU).
					 */
					nr_reclaimed += nr_pages;
					continue;
				}
			}
		}

		if (folio_test_anon(folio) && !folio_test_swapbacked(folio)) {
			/* follow __remove_mapping for reference */
			if (!folio_ref_freeze(folio, 1))
				goto keep_locked;
			/*
			 * The folio has only one reference left, which is
			 * from the isolation. After the caller puts the
			 * folio back on the lru and drops the reference, the
			 * folio will be freed anyway. It doesn't matter
			 * which lru it goes on. So we don't bother checking
			 * the dirty flag here.
			 */
			count_vm_events(PGLAZYFREED, nr_pages);
			count_memcg_folio_events(folio, PGLAZYFREED, nr_pages);
		} else if (!mapping || !__remove_mapping(mapping, folio, true,
							 sc->target_mem_cgroup))
			goto keep_locked;

		folio_unlock(folio);
free_it:
		/*
		 * Folio may get swapped out as a whole, need to account
		 * all pages in it.
		 */
		nr_reclaimed += nr_pages;

		folio_unqueue_deferred_split(folio);
		if (folio_batch_add(&free_folios, folio) == 0) {
			mem_cgroup_uncharge_folios(&free_folios);
			try_to_unmap_flush();
			free_unref_folios(&free_folios);
		}
		continue;

activate_locked_split:
		/*
		 * The tail pages that are failed to add into swap cache
		 * reach here.  Fixup nr_scanned and nr_pages.
		 */
		if (nr_pages > 1) {
			sc->nr_scanned -= (nr_pages - 1);
			nr_pages = 1;
		}
activate_locked:
		/* Not a candidate for swapping, so reclaim swap space. */
		if (folio_test_swapcache(folio) &&
		    (mem_cgroup_swap_full(folio) || folio_test_mlocked(folio)))
			folio_free_swap(folio);
		VM_BUG_ON_FOLIO(folio_test_active(folio), folio);
		if (!folio_test_mlocked(folio)) {
			int type = folio_is_file_lru(folio);
			folio_set_active(folio);
			stat->nr_activate[type] += nr_pages;
			count_memcg_folio_events(folio, PGACTIVATE, nr_pages);
		}
keep_locked:
		folio_unlock(folio);
keep:
		list_add(&folio->lru, &ret_folios);
		VM_BUG_ON_FOLIO(folio_test_lru(folio) ||
				folio_test_unevictable(folio), folio);
	}
	/* 'folio_list' is always empty here */

	/* Migrate folios selected for demotion */
	nr_demoted = demote_folio_list(&demote_folios, pgdat, memcg);
	nr_reclaimed += nr_demoted;
	stat->nr_demoted += nr_demoted;
	/* Folios that could not be demoted are still in @demote_folios */
	if (!list_empty(&demote_folios)) {
		/* Folios which weren't demoted go back on @folio_list */
		list_splice_init(&demote_folios, folio_list);

		/*
		 * goto retry to reclaim the undemoted folios in folio_list if
		 * desired.
		 *
		 * Reclaiming directly from top tier nodes is not often desired
		 * due to it breaking the LRU ordering: in general memory
		 * should be reclaimed from lower tier nodes and demoted from
		 * top tier nodes.
		 *
		 * However, disabling reclaim from top tier nodes entirely
		 * would cause ooms in edge scenarios where lower tier memory
		 * is unreclaimable for whatever reason, eg memory being
		 * mlocked or too hot to reclaim. We can disable reclaim
		 * from top tier nodes in proactive reclaim though as that is
		 * not real memory pressure.
		 */
		if (!sc->proactive) {
			do_demote_pass = false;
			goto retry;
		}
	}

	pgactivate = stat->nr_activate[0] + stat->nr_activate[1];

	mem_cgroup_uncharge_folios(&free_folios);
	try_to_unmap_flush();
	free_unref_folios(&free_folios);

	list_splice(&ret_folios, folio_list);
	count_vm_events(PGACTIVATE, pgactivate);

	if (plug)
		swap_write_unplug(plug);
	return nr_reclaimed;
}

unsigned int reclaim_clean_pages_from_list(struct zone *zone,
					   struct list_head *folio_list)
{
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.may_unmap = 1,
	};
	struct reclaim_stat stat;
	unsigned int nr_reclaimed;
	struct folio *folio, *next;
	LIST_HEAD(clean_folios);
	unsigned int noreclaim_flag;

	list_for_each_entry_safe(folio, next, folio_list, lru) {
		/* TODO: these pages should not even appear in this list. */
		if (page_has_movable_ops(&folio->page))
			continue;
		if (!folio_test_hugetlb(folio) && folio_is_file_lru(folio) &&
		    !folio_test_dirty(folio) && !folio_test_unevictable(folio)) {
			folio_clear_active(folio);
			list_move(&folio->lru, &clean_folios);
		}
	}

	/*
	 * We should be safe here since we are only dealing with file pages and
	 * we are not kswapd and therefore cannot write dirty file pages. But
	 * call memalloc_noreclaim_save() anyway, just in case these conditions
	 * change in the future.
	 */
	noreclaim_flag = memalloc_noreclaim_save();
	nr_reclaimed = shrink_folio_list(&clean_folios, zone->zone_pgdat, &sc,
					&stat, true, NULL);
	memalloc_noreclaim_restore(noreclaim_flag);

	list_splice(&clean_folios, folio_list);
	mod_node_page_state(zone->zone_pgdat, NR_ISOLATED_FILE,
			    -(long)nr_reclaimed);
	/*
	 * Since lazyfree pages are isolated from file LRU from the beginning,
	 * they will rotate back to anonymous LRU in the end if it failed to
	 * discard so isolated count will be mismatched.
	 * Compensate the isolated count for both LRU lists.
	 */
	mod_node_page_state(zone->zone_pgdat, NR_ISOLATED_ANON,
			    stat.nr_lazyfree_fail);
	mod_node_page_state(zone->zone_pgdat, NR_ISOLATED_FILE,
			    -(long)stat.nr_lazyfree_fail);
	return nr_reclaimed;
}

/**
 * folio_isolate_lru() - Try to isolate a folio from its LRU list.
 * @folio: Folio to isolate from its LRU list.
 *
 * Isolate a @folio from an LRU list and adjust the vmstat statistic
 * corresponding to whatever LRU list the folio was on.
 *
 * The folio will have its LRU flag cleared.  If it was found on the
 * active list, it will have the Active flag set.  If it was found on the
 * unevictable list, it will have the Unevictable flag set.  These flags
 * may need to be cleared by the caller before letting the page go.
 *
 * Context:
 *
 * (1) Must be called with an elevated refcount on the folio. This is a
 *     fundamental difference from isolate_lru_folios() (which is called
 *     without a stable reference).
 * (2) The lru_lock must not be held.
 * (3) Interrupts must be enabled.
 *
 * Return: true if the folio was removed from an LRU list.
 * false if the folio was not on an LRU list.
 */
bool folio_isolate_lru(struct folio *folio)
{
	bool ret = false;

	VM_BUG_ON_FOLIO(!folio_ref_count(folio), folio);

	if (folio_test_clear_lru(folio)) {
		struct lruvec *lruvec;

		folio_get(folio);
		lruvec = folio_lruvec_lock_irq(folio);
		lruvec_del_folio(lruvec, folio);
		unlock_page_lruvec_irq(lruvec);
		ret = true;
	}

	return ret;
}

/*
 * move_folios_to_lru() moves folios from private @list to appropriate LRU list.
 *
 * Returns the number of pages moved to the given lruvec.
 */
unsigned int move_folios_to_lru(struct lruvec *lruvec,
		struct list_head *list)
{
	int nr_pages, nr_moved = 0;
	struct folio_batch free_folios;

	folio_batch_init(&free_folios);
	while (!list_empty(list)) {
		struct folio *folio = lru_to_folio(list);

		VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);
		list_del(&folio->lru);
		if (unlikely(!folio_evictable(folio))) {
			spin_unlock_irq(&lruvec->lru_lock);
			folio_putback_lru(folio);
			spin_lock_irq(&lruvec->lru_lock);
			continue;
		}

		/*
		 * The folio_set_lru needs to be kept here for list integrity.
		 * Otherwise:
		 *   #0 move_folios_to_lru             #1 release_pages
		 *   if (!folio_put_testzero())
		 *				      if (folio_put_testzero())
		 *				        !lru //skip lru_lock
		 *     folio_set_lru()
		 *     list_add(&folio->lru,)
		 *                                        list_add(&folio->lru,)
		 */
		folio_set_lru(folio);

		if (unlikely(folio_put_testzero(folio))) {
			__folio_clear_lru_flags(folio);

			folio_unqueue_deferred_split(folio);
			if (folio_batch_add(&free_folios, folio) == 0) {
				spin_unlock_irq(&lruvec->lru_lock);
				mem_cgroup_uncharge_folios(&free_folios);
				free_unref_folios(&free_folios);
				spin_lock_irq(&lruvec->lru_lock);
			}

			continue;
		}

		/*
		 * All pages were isolated from the same lruvec (and isolation
		 * inhibits memcg migration).
		 */
		VM_BUG_ON_FOLIO(!folio_matches_lruvec(folio, lruvec), folio);
		lruvec_add_folio(lruvec, folio);
		nr_pages = folio_nr_pages(folio);
		nr_moved += nr_pages;
		if (folio_test_active(folio))
			workingset_age_nonresident(lruvec, nr_pages);
	}

	if (free_folios.nr) {
		spin_unlock_irq(&lruvec->lru_lock);
		mem_cgroup_uncharge_folios(&free_folios);
		free_unref_folios(&free_folios);
		spin_lock_irq(&lruvec->lru_lock);
	}

	return nr_moved;
}

static unsigned int reclaim_folio_list(struct list_head *folio_list,
				      struct pglist_data *pgdat)
{
	struct reclaim_stat stat;
	unsigned int nr_reclaimed;
	struct folio *folio;
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
		.no_demotion = 1,
	};

	nr_reclaimed = shrink_folio_list(folio_list, pgdat, &sc, &stat, true, NULL);
	while (!list_empty(folio_list)) {
		folio = lru_to_folio(folio_list);
		list_del(&folio->lru);
		folio_putback_lru(folio);
	}
	trace_mm_vmscan_reclaim_pages(pgdat->node_id, sc.nr_scanned, nr_reclaimed, &stat);

	return nr_reclaimed;
}

unsigned long reclaim_pages(struct list_head *folio_list)
{
	int nid;
	unsigned int nr_reclaimed = 0;
	LIST_HEAD(node_folio_list);
	unsigned int noreclaim_flag;

	if (list_empty(folio_list))
		return nr_reclaimed;

	noreclaim_flag = memalloc_noreclaim_save();

	nid = folio_nid(lru_to_folio(folio_list));
	do {
		struct folio *folio = lru_to_folio(folio_list);

		if (nid == folio_nid(folio)) {
			folio_clear_active(folio);
			list_move(&folio->lru, &node_folio_list);
			continue;
		}

		nr_reclaimed += reclaim_folio_list(&node_folio_list, NODE_DATA(nid));
		nid = folio_nid(lru_to_folio(folio_list));
	} while (!list_empty(folio_list));

	nr_reclaimed += reclaim_folio_list(&node_folio_list, NODE_DATA(nid));

	memalloc_noreclaim_restore(noreclaim_flag);

	return nr_reclaimed;
}


unsigned long apply_proportional_protection(struct mem_cgroup *memcg,
		struct scan_control *sc, unsigned long scan)
{
	unsigned long min, low, usage;

	mem_cgroup_protection(sc->target_mem_cgroup, memcg, &min, &low, &usage);

	if (min || low) {
		/*
		 * Scale a cgroup's reclaim pressure by proportioning
		 * its current usage to its memory.low or memory.min
		 * setting.
		 *
		 * This is important, as otherwise scanning aggression
		 * becomes extremely binary -- from nothing as we
		 * approach the memory protection threshold, to totally
		 * nominal as we exceed it.  This results in requiring
		 * setting extremely liberal protection thresholds. It
		 * also means we simply get no protection at all if we
		 * set it too low, which is not ideal.
		 *
		 * If there is any protection in place, we reduce scan
		 * pressure by how much of the total memory used is
		 * within protection thresholds.
		 *
		 * There is one special case: in the first reclaim pass,
		 * we skip over all groups that are within their low
		 * protection. If that fails to reclaim enough pages to
		 * satisfy the reclaim goal, we come back and override
		 * the best-effort low protection. However, we still
		 * ideally want to honor how well-behaved groups are in
		 * that case instead of simply punishing them all
		 * equally. As such, we reclaim them based on how much
		 * memory they are using, reducing the scan pressure
		 * again by how much of the total memory used is under
		 * hard protection.
		 */
		unsigned long protection;

		/* memory.low scaling, make sure we retry before OOM */
		if (!sc->memcg_low_reclaim && low > min) {
			protection = low;
			sc->memcg_low_skipped = 1;
		} else {
			protection = min;
		}

		/* Avoid TOCTOU with earlier protection check */
		usage = max(usage, protection);

		scan -= scan * protection / (usage + 1);

		/*
		 * Minimally target SWAP_CLUSTER_MAX pages to keep
		 * reclaim moving forwards, avoiding decrementing
		 * sc->priority further than desirable.
		 */
		scan = max(scan, SWAP_CLUSTER_MAX);
	}
	return scan;
}

static void shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
	if (lru_gen_enabled() && !root_reclaim(sc))
		lru_gen_shrink_lruvec(lruvec, sc);
	else
		lru_shrink_lruvec(lruvec, sc);
}

void shrink_node_memcgs(pg_data_t *pgdat, struct scan_control *sc)
{
	struct mem_cgroup *target_memcg = sc->target_mem_cgroup;
	struct mem_cgroup_reclaim_cookie reclaim = {
		.pgdat = pgdat,
	};
	struct mem_cgroup_reclaim_cookie *partial = &reclaim;
	struct mem_cgroup *memcg;

	/*
	 * In most cases, direct reclaimers can do partial walks
	 * through the cgroup tree, using an iterator state that
	 * persists across invocations. This strikes a balance between
	 * fairness and allocation latency.
	 *
	 * For kswapd, reliable forward progress is more important
	 * than a quick return to idle. Always do full walks.
	 */
	if (current_is_kswapd() || sc->memcg_full_walk)
		partial = NULL;

	memcg = mem_cgroup_iter(target_memcg, NULL, partial);
	do {
		struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
		unsigned long reclaimed;
		unsigned long scanned;

		/*
		 * This loop can become CPU-bound when target memcgs
		 * aren't eligible for reclaim - either because they
		 * don't have any reclaimable pages, or because their
		 * memory is explicitly protected. Avoid soft lockups.
		 */
		cond_resched();

		mem_cgroup_calculate_protection(target_memcg, memcg);

		if (mem_cgroup_below_min(target_memcg, memcg)) {
			/*
			 * Hard protection.
			 * If there is no reclaimable memory, OOM.
			 */
			continue;
		} else if (mem_cgroup_below_low(target_memcg, memcg)) {
			/*
			 * Soft protection.
			 * Respect the protection only as long as
			 * there is an unprotected supply
			 * of reclaimable memory from other cgroups.
			 */
			if (!sc->memcg_low_reclaim) {
				sc->memcg_low_skipped = 1;
				continue;
			}
			memcg_memory_event(memcg, MEMCG_LOW);
		}

		reclaimed = sc->nr_reclaimed;
		scanned = sc->nr_scanned;

		shrink_lruvec(lruvec, sc);

		shrink_slab(sc->gfp_mask, pgdat->node_id, memcg,
			    sc->priority);

		/* Record the group's reclaim efficiency */
		if (!sc->proactive)
			vmpressure(sc->gfp_mask, memcg, false,
				   sc->nr_scanned - scanned,
				   sc->nr_reclaimed - reclaimed);

		/* If partial walks are allowed, bail once goal is reached */
		if (partial && sc->nr_reclaimed >= sc->nr_to_reclaim) {
			mem_cgroup_iter_break(target_memcg, memcg);
			break;
		}
	} while ((memcg = mem_cgroup_iter(target_memcg, memcg, partial)));
}

static void shrink_node(pg_data_t *pgdat, struct scan_control *sc)
{
	if (lru_gen_enabled() && root_reclaim(sc)) {
		memset(&sc->nr, 0, sizeof(sc->nr));
		lru_gen_shrink_node(pgdat, sc);
	} else {
		lru_shrink_node(pgdat, sc);
	}
}

/*
 * Returns true if compaction should go ahead for a costly-order request, or
 * the allocation would already succeed without compaction. Return false if we
 * should reclaim first.
 */
static inline bool compaction_ready(struct zone *zone, struct scan_control *sc)
{
	unsigned long watermark;

	if (!gfp_compaction_allowed(sc->gfp_mask))
		return false;

	/* Allocation can already succeed, nothing to do */
	if (zone_watermark_ok(zone, sc->order, min_wmark_pages(zone),
			      sc->reclaim_idx, 0))
		return true;

	/*
	 * Direct reclaim usually targets the min watermark, but compaction
	 * takes time to run and there are potentially other callers using the
	 * pages just freed. So target a higher buffer to give compaction a
	 * reasonable chance of completing and allocating the pages.
	 *
	 * Note that we won't actually reclaim the whole buffer in one attempt
	 * as the target watermark in should_continue_reclaim() is lower. But if
	 * we are already above the high+gap watermark, don't reclaim at all.
	 */
	watermark = high_wmark_pages(zone);
	if (compaction_suitable(zone, sc->order, watermark, sc->reclaim_idx))
		return true;

	return false;
}

static void consider_reclaim_throttle(pg_data_t *pgdat, struct scan_control *sc)
{
	/*
	 * If reclaim is making progress greater than 12% efficiency then
	 * wake all the NOPROGRESS throttled tasks.
	 */
	if (sc->nr_reclaimed > (sc->nr_scanned >> 3)) {
		wait_queue_head_t *wqh;

		wqh = &pgdat->reclaim_wait[VMSCAN_THROTTLE_NOPROGRESS];
		if (waitqueue_active(wqh))
			wake_up(wqh);

		return;
	}

	/*
	 * Do not throttle kswapd or cgroup reclaim on NOPROGRESS as it will
	 * throttle on VMSCAN_THROTTLE_WRITEBACK if there are too many pages
	 * under writeback and marked for immediate reclaim at the tail of the
	 * LRU.
	 */
	if (current_is_kswapd() || cgroup_reclaim(sc))
		return;

	/* Throttle if making no progress at high prioities. */
	if (sc->priority == 1 && !sc->nr_reclaimed)
		reclaim_throttle(pgdat, VMSCAN_THROTTLE_NOPROGRESS);
}

/*
 * This is the direct reclaim path, for page-allocating processes.  We only
 * try to reclaim pages from zones which will satisfy the caller's allocation
 * request.
 *
 * If a zone is deemed to be full of pinned pages then just give it a light
 * scan then give up on it.
 */
static void shrink_zones(struct zonelist *zonelist, struct scan_control *sc)
{
	struct zoneref *z;
	struct zone *zone;
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	gfp_t orig_mask;
	pg_data_t *last_pgdat = NULL;
	pg_data_t *first_pgdat = NULL;

	/*
	 * If the number of buffer_heads in the machine exceeds the maximum
	 * allowed level, force direct reclaim to scan the highmem zone as
	 * highmem pages could be pinning lowmem pages storing buffer_heads
	 */
	orig_mask = sc->gfp_mask;
	if (buffer_heads_over_limit) {
		sc->gfp_mask |= __GFP_HIGHMEM;
		sc->reclaim_idx = gfp_zone(sc->gfp_mask);
	}

	for_each_zone_zonelist_nodemask(zone, z, zonelist,
					sc->reclaim_idx, sc->nodemask) {
		/*
		 * Take care memory controller reclaiming has small influence
		 * to global LRU.
		 */
		if (!cgroup_reclaim(sc)) {
			if (!cpuset_zone_allowed(zone,
						 GFP_KERNEL | __GFP_HARDWALL))
				continue;

			/*
			 * If we already have plenty of memory free for
			 * compaction in this zone, don't free any more.
			 * Even though compaction is invoked for any
			 * non-zero order, only frequent costly order
			 * reclamation is disruptive enough to become a
			 * noticeable problem, like transparent huge
			 * page allocations.
			 */
			if (IS_ENABLED(CONFIG_COMPACTION) &&
			    sc->order > PAGE_ALLOC_COSTLY_ORDER &&
			    compaction_ready(zone, sc)) {
				sc->compaction_ready = true;
				continue;
			}

			/*
			 * Shrink each node in the zonelist once. If the
			 * zonelist is ordered by zone (not the default) then a
			 * node may be shrunk multiple times but in that case
			 * the user prefers lower zones being preserved.
			 */
			if (zone->zone_pgdat == last_pgdat)
				continue;

			/*
			 * This steals pages from memory cgroups over softlimit
			 * and returns the number of reclaimed pages and
			 * scanned pages. This works for global memory pressure
			 * and balancing, not for a memcg's limit.
			 */
			nr_soft_scanned = 0;
			nr_soft_reclaimed = memcg1_soft_limit_reclaim(zone->zone_pgdat,
								      sc->order, sc->gfp_mask,
								      &nr_soft_scanned);
			sc->nr_reclaimed += nr_soft_reclaimed;
			sc->nr_scanned += nr_soft_scanned;
			/* need some check for avoid more shrink_zone() */
		}

		if (!first_pgdat)
			first_pgdat = zone->zone_pgdat;

		/* See comment about same check for global reclaim above */
		if (zone->zone_pgdat == last_pgdat)
			continue;
		last_pgdat = zone->zone_pgdat;
		shrink_node(zone->zone_pgdat, sc);
	}

	if (first_pgdat)
		consider_reclaim_throttle(first_pgdat, sc);

	/*
	 * Restore to original mask to avoid the impact on the caller if we
	 * promoted it to __GFP_HIGHMEM.
	 */
	sc->gfp_mask = orig_mask;
}

/*
 * This is the main entry point to direct page reclaim.
 *
 * If a full scan of the inactive list fails to free enough memory then we
 * are "out of memory" and something needs to be killed.
 *
 * If the caller is !__GFP_FS then the probability of a failure is reasonably
 * high - the zone may be full of dirty or under-writeback pages, which this
 * caller can't do much about.  We kick the writeback threads and take explicit
 * naps in the hope that some of these pages can be written.  But if the
 * allocating task holds filesystem locks which prevent writeout this might not
 * work, and the allocation attempt will fail.
 *
 * returns:	0, if no pages reclaimed
 * 		else, the number of pages reclaimed
 */
static unsigned long do_try_to_free_pages(struct zonelist *zonelist,
					  struct scan_control *sc)
{
	int initial_priority = sc->priority;
	pg_data_t *last_pgdat;
	struct zoneref *z;
	struct zone *zone;
retry:
	delayacct_freepages_start();

	if (!cgroup_reclaim(sc))
		__count_zid_vm_events(ALLOCSTALL, sc->reclaim_idx, 1);

	do {
		if (!sc->proactive)
			vmpressure_prio(sc->gfp_mask, sc->target_mem_cgroup,
					sc->priority);
		sc->nr_scanned = 0;
		shrink_zones(zonelist, sc);

		if (sc->nr_reclaimed >= sc->nr_to_reclaim)
			break;

		if (sc->compaction_ready)
			break;
	} while (--sc->priority >= 0);

	last_pgdat = NULL;
	for_each_zone_zonelist_nodemask(zone, z, zonelist, sc->reclaim_idx,
					sc->nodemask) {
		if (zone->zone_pgdat == last_pgdat)
			continue;
		last_pgdat = zone->zone_pgdat;


		if (!lru_gen_enabled())
			lru_snapshot_refaults(sc->target_mem_cgroup, zone->zone_pgdat);

		if (cgroup_reclaim(sc)) {
			struct lruvec *lruvec;

			lruvec = mem_cgroup_lruvec(sc->target_mem_cgroup,
						   zone->zone_pgdat);
			clear_bit(LRUVEC_CGROUP_CONGESTED, &lruvec->flags);
		}
	}

	delayacct_freepages_end();

	if (sc->nr_reclaimed)
		return sc->nr_reclaimed;

	/* Aborted reclaim to try compaction? don't OOM, then */
	if (sc->compaction_ready)
		return 1;

	/*
	 * In most cases, direct reclaimers can do partial walks
	 * through the cgroup tree to meet the reclaim goal while
	 * keeping latency low. Since the iterator state is shared
	 * among all direct reclaim invocations (to retain fairness
	 * among cgroups), though, high concurrency can result in
	 * individual threads not seeing enough cgroups to make
	 * meaningful forward progress. Avoid false OOMs in this case.
	 */
	if (!sc->memcg_full_walk) {
		sc->priority = initial_priority;
		sc->memcg_full_walk = 1;
		goto retry;
	}

	/*
	 * We make inactive:active ratio decisions based on the node's
	 * composition of memory, but a restrictive reclaim_idx or a
	 * memory.low cgroup setting can exempt large amounts of
	 * memory from reclaim. Neither of which are very common, so
	 * instead of doing costly eligibility calculations of the
	 * entire cgroup subtree up front, we assume the estimates are
	 * good, and retry with forcible deactivation if that fails.
	 */
	if (sc->skipped_deactivate) {
		sc->priority = initial_priority;
		sc->force_deactivate = 1;
		sc->skipped_deactivate = 0;
		goto retry;
	}

	/* Untapped cgroup reserves?  Don't OOM, retry. */
	if (sc->memcg_low_skipped) {
		sc->priority = initial_priority;
		sc->force_deactivate = 0;
		sc->memcg_low_reclaim = 1;
		sc->memcg_low_skipped = 0;
		goto retry;
	}

	return 0;
}

static bool allow_direct_reclaim(pg_data_t *pgdat)
{
	struct zone *zone;
	unsigned long pfmemalloc_reserve = 0;
	unsigned long free_pages = 0;
	int i;
	bool wmark_ok;

	if (kswapd_test_hopeless(pgdat))
		return true;

	for_each_managed_zone_pgdat(zone, pgdat, i, ZONE_NORMAL) {
		if (!zone_reclaimable_pages(zone) && zone_page_state_snapshot(zone, NR_FREE_PAGES))
			continue;

		pfmemalloc_reserve += min_wmark_pages(zone);
		free_pages += zone_page_state_snapshot(zone, NR_FREE_PAGES);
	}

	/* If there are no reserves (unexpected config) then do not throttle */
	if (!pfmemalloc_reserve)
		return true;

	wmark_ok = free_pages > pfmemalloc_reserve / 2;

	/* kswapd must be awake if processes are being throttled */
	if (!wmark_ok && waitqueue_active(&pgdat->kswapd_wait)) {
		if (READ_ONCE(pgdat->kswapd_highest_zoneidx) > ZONE_NORMAL)
			WRITE_ONCE(pgdat->kswapd_highest_zoneidx, ZONE_NORMAL);

		wake_up_interruptible(&pgdat->kswapd_wait);
	}

	return wmark_ok;
}

/*
 * Throttle direct reclaimers if backing storage is backed by the network
 * and the PFMEMALLOC reserve for the preferred node is getting dangerously
 * depleted. kswapd will continue to make progress and wake the processes
 * when the low watermark is reached.
 *
 * Returns true if a fatal signal was delivered during throttling. If this
 * happens, the page allocator should not consider triggering the OOM killer.
 */
static bool throttle_direct_reclaim(gfp_t gfp_mask, struct zonelist *zonelist,
					nodemask_t *nodemask)
{
	struct zoneref *z;
	struct zone *zone;
	pg_data_t *pgdat = NULL;

	/*
	 * Kernel threads should not be throttled as they may be indirectly
	 * responsible for cleaning pages necessary for reclaim to make forward
	 * progress. kjournald for example may enter direct reclaim while
	 * committing a transaction where throttling it could forcing other
	 * processes to block on log_wait_commit().
	 */
	if (current->flags & PF_KTHREAD)
		goto out;

	/*
	 * If a fatal signal is pending, this process should not throttle.
	 * It should return quickly so it can exit and free its memory
	 */
	if (fatal_signal_pending(current))
		goto out;

	/*
	 * Check if the pfmemalloc reserves are ok by finding the first node
	 * with a usable ZONE_NORMAL or lower zone. The expectation is that
	 * GFP_KERNEL will be required for allocating network buffers when
	 * swapping over the network so ZONE_HIGHMEM is unusable.
	 *
	 * Throttling is based on the first usable node and throttled processes
	 * wait on a queue until kswapd makes progress and wakes them. There
	 * is an affinity then between processes waking up and where reclaim
	 * progress has been made assuming the process wakes on the same node.
	 * More importantly, processes running on remote nodes will not compete
	 * for remote pfmemalloc reserves and processes on different nodes
	 * should make reasonable progress.
	 */
	for_each_zone_zonelist_nodemask(zone, z, zonelist,
					gfp_zone(gfp_mask), nodemask) {
		if (zone_idx(zone) > ZONE_NORMAL)
			continue;

		/* Throttle based on the first usable node */
		pgdat = zone->zone_pgdat;
		if (allow_direct_reclaim(pgdat))
			goto out;
		break;
	}

	/* If no zone was usable by the allocation flags then do not throttle */
	if (!pgdat)
		goto out;

	/* Account for the throttling */
	count_vm_event(PGSCAN_DIRECT_THROTTLE);

	/*
	 * If the caller cannot enter the filesystem, it's possible that it
	 * is due to the caller holding an FS lock or performing a journal
	 * transaction in the case of a filesystem like ext[3|4]. In this case,
	 * it is not safe to block on pfmemalloc_wait as kswapd could be
	 * blocked waiting on the same lock. Instead, throttle for up to a
	 * second before continuing.
	 */
	if (!(gfp_mask & __GFP_FS))
		wait_event_interruptible_timeout(pgdat->pfmemalloc_wait,
			allow_direct_reclaim(pgdat), HZ);
	else
		/* Throttle until kswapd wakes the process */
		wait_event_killable(zone->zone_pgdat->pfmemalloc_wait,
			allow_direct_reclaim(pgdat));

	if (fatal_signal_pending(current))
		return true;

out:
	return false;
}

unsigned long try_to_free_pages(struct zonelist *zonelist, int order,
				gfp_t gfp_mask, nodemask_t *nodemask)
{
	unsigned long nr_reclaimed;
	struct scan_control sc = {
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.gfp_mask = current_gfp_context(gfp_mask),
		.reclaim_idx = gfp_zone(gfp_mask),
		.order = order,
		.nodemask = nodemask,
		.priority = DEF_PRIORITY,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
	};

	/*
	 * scan_control uses s8 fields for order, priority, and reclaim_idx.
	 * Confirm they are large enough for max values.
	 */
	BUILD_BUG_ON(MAX_PAGE_ORDER >= S8_MAX);
	BUILD_BUG_ON(DEF_PRIORITY > S8_MAX);
	BUILD_BUG_ON(MAX_NR_ZONES > S8_MAX);

	/*
	 * Do not enter reclaim if fatal signal was delivered while throttled.
	 * 1 is returned so that the page allocator does not OOM kill at this
	 * point.
	 */
	if (throttle_direct_reclaim(sc.gfp_mask, zonelist, nodemask))
		return 1;

	set_task_reclaim_state(current, &sc.reclaim_state);
	trace_mm_vmscan_direct_reclaim_begin(order, sc.gfp_mask);

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	trace_mm_vmscan_direct_reclaim_end(nr_reclaimed);
	set_task_reclaim_state(current, NULL);

	return nr_reclaimed;
}

#ifdef CONFIG_MEMCG

/* Only used by soft limit reclaim. Do not reuse for anything else. */
unsigned long mem_cgroup_shrink_node(struct mem_cgroup *memcg,
						gfp_t gfp_mask, bool noswap,
						pg_data_t *pgdat,
						unsigned long *nr_scanned)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
	struct scan_control sc = {
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.target_mem_cgroup = memcg,
		.may_writepage = 1,
		.may_unmap = 1,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.may_swap = !noswap,
	};

	WARN_ON_ONCE(!current->reclaim_state);

	sc.gfp_mask = (gfp_mask & GFP_RECLAIM_MASK) |
			(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK);

	trace_mm_vmscan_memcg_softlimit_reclaim_begin(sc.order,
						      sc.gfp_mask);

	/*
	 * NOTE: Although we can get the priority field, using it
	 * here is not a good idea, since it limits the pages we can scan.
	 * if we don't reclaim here, the shrink_node from balance_pgdat
	 * will pick up pages from other mem cgroup's as well. We hack
	 * the priority and make it zero.
	 */
	shrink_lruvec(lruvec, &sc);

	trace_mm_vmscan_memcg_softlimit_reclaim_end(sc.nr_reclaimed);

	*nr_scanned = sc.nr_scanned;

	return sc.nr_reclaimed;
}

unsigned long try_to_free_mem_cgroup_pages(struct mem_cgroup *memcg,
					   unsigned long nr_pages,
					   gfp_t gfp_mask,
					   unsigned int reclaim_options,
					   int *swappiness)
{
	unsigned long nr_reclaimed;
	unsigned int noreclaim_flag;
	struct scan_control sc = {
		.nr_to_reclaim = max(nr_pages, SWAP_CLUSTER_MAX),
		.proactive_swappiness = swappiness,
		.gfp_mask = (current_gfp_context(gfp_mask) & GFP_RECLAIM_MASK) |
				(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK),
		.reclaim_idx = MAX_NR_ZONES - 1,
		.target_mem_cgroup = memcg,
		.priority = DEF_PRIORITY,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = !!(reclaim_options & MEMCG_RECLAIM_MAY_SWAP),
		.proactive = !!(reclaim_options & MEMCG_RECLAIM_PROACTIVE),
	};
	/*
	 * Traverse the ZONELIST_FALLBACK zonelist of the current node to put
	 * equal pressure on all the nodes. This is based on the assumption that
	 * the reclaim does not bail out early.
	 */
	struct zonelist *zonelist = node_zonelist(numa_node_id(), sc.gfp_mask);

	set_task_reclaim_state(current, &sc.reclaim_state);
	trace_mm_vmscan_memcg_reclaim_begin(0, sc.gfp_mask);
	noreclaim_flag = memalloc_noreclaim_save();

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	memalloc_noreclaim_restore(noreclaim_flag);
	trace_mm_vmscan_memcg_reclaim_end(nr_reclaimed);
	set_task_reclaim_state(current, NULL);

	return nr_reclaimed;
}
#else
unsigned long try_to_free_mem_cgroup_pages(struct mem_cgroup *memcg,
					   unsigned long nr_pages,
					   gfp_t gfp_mask,
					   unsigned int reclaim_options,
					   int *swappiness)
{
	return 0;
}
#endif

static bool pgdat_watermark_boosted(pg_data_t *pgdat, int highest_zoneidx)
{
	int i;
	struct zone *zone;

	/*
	 * Check for watermark boosts top-down as the higher zones
	 * are more likely to be boosted. Both watermarks and boosts
	 * should not be checked at the same time as reclaim would
	 * start prematurely when there is no boosting and a lower
	 * zone is balanced.
	 */
	for (i = highest_zoneidx; i >= 0; i--) {
		zone = pgdat->node_zones + i;
		if (!managed_zone(zone))
			continue;

		if (zone->watermark_boost)
			return true;
	}

	return false;
}

/*
 * Returns true if there is an eligible zone balanced for the request order
 * and highest_zoneidx
 */
static bool pgdat_balanced(pg_data_t *pgdat, int order, int highest_zoneidx)
{
	int i;
	unsigned long mark = -1;
	struct zone *zone;

	/*
	 * Check watermarks bottom-up as lower zones are more likely to
	 * meet watermarks.
	 */
	for_each_managed_zone_pgdat(zone, pgdat, i, highest_zoneidx) {
		enum zone_stat_item item;
		unsigned long free_pages;

		if (sysctl_numa_balancing_mode & NUMA_BALANCING_MEMORY_TIERING)
			mark = promo_wmark_pages(zone);
		else
			mark = high_wmark_pages(zone);

		/*
		 * In defrag_mode, watermarks must be met in whole
		 * blocks to avoid polluting allocator fallbacks.
		 *
		 * However, kswapd usually cannot accomplish this on
		 * its own and needs kcompactd support. Once it's
		 * reclaimed a compaction gap, and kswapd_shrink_node
		 * has dropped order, simply ensure there are enough
		 * base pages for compaction, wake kcompactd & sleep.
		 */
		if (defrag_mode && order)
			item = NR_FREE_PAGES_BLOCKS;
		else
			item = NR_FREE_PAGES;

		/*
		 * When there is a high number of CPUs in the system,
		 * the cumulative error from the vmstat per-cpu cache
		 * can blur the line between the watermarks. In that
		 * case, be safe and get an accurate snapshot.
		 *
		 * TODO: NR_FREE_PAGES_BLOCKS moves in steps of
		 * pageblock_nr_pages, while the vmstat pcp threshold
		 * is limited to 125. On many configurations that
		 * counter won't actually be per-cpu cached. But keep
		 * things simple for now; revisit when somebody cares.
		 */
		free_pages = zone_page_state(zone, item);
		if (zone->percpu_drift_mark && free_pages < zone->percpu_drift_mark)
			free_pages = zone_page_state_snapshot(zone, item);

		if (__zone_watermark_ok(zone, order, mark, highest_zoneidx,
					0, free_pages))
			return true;
	}

	/*
	 * If a node has no managed zone within highest_zoneidx, it does not
	 * need balancing by definition. This can happen if a zone-restricted
	 * allocation tries to wake a remote kswapd.
	 */
	if (mark == -1)
		return true;

	return false;
}

/* Clear pgdat state for congested, dirty or under writeback. */
static void clear_pgdat_congested(pg_data_t *pgdat)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(NULL, pgdat);

	clear_bit(LRUVEC_NODE_CONGESTED, &lruvec->flags);
	clear_bit(LRUVEC_CGROUP_CONGESTED, &lruvec->flags);
	clear_bit(PGDAT_WRITEBACK, &pgdat->flags);
}

/*
 * Prepare kswapd for sleeping. This verifies that there are no processes
 * waiting in throttle_direct_reclaim() and that watermarks have been met.
 *
 * Returns true if kswapd is ready to sleep
 */
static bool prepare_kswapd_sleep(pg_data_t *pgdat, int order,
				int highest_zoneidx)
{
	/*
	 * The throttled processes are normally woken up in balance_pgdat() as
	 * soon as allow_direct_reclaim() is true. But there is a potential
	 * race between when kswapd checks the watermarks and a process gets
	 * throttled. There is also a potential race if processes get
	 * throttled, kswapd wakes, a large process exits thereby balancing the
	 * zones, which causes kswapd to exit balance_pgdat() before reaching
	 * the wake up checks. If kswapd is going to sleep, no process should
	 * be sleeping on pfmemalloc_wait, so wake them now if necessary. If
	 * the wake up is premature, processes will wake kswapd and get
	 * throttled again. The difference from wake ups in balance_pgdat() is
	 * that here we are under prepare_to_wait().
	 */
	if (waitqueue_active(&pgdat->pfmemalloc_wait))
		wake_up_all(&pgdat->pfmemalloc_wait);

	/* Hopeless node, leave it to direct reclaim */
	if (kswapd_test_hopeless(pgdat))
		return true;

	if (pgdat_balanced(pgdat, order, highest_zoneidx)) {
		clear_pgdat_congested(pgdat);
		return true;
	}

	return false;
}

/*
 * kswapd shrinks a node of pages that are at or below the highest usable
 * zone that is currently unbalanced.
 *
 * Returns true if kswapd scanned at least the requested number of pages to
 * reclaim or if the lack of progress was due to pages under writeback.
 * This is used to determine if the scanning priority needs to be raised.
 */
static bool kswapd_shrink_node(pg_data_t *pgdat,
			       struct scan_control *sc)
{
	struct zone *zone;
	int z;
	unsigned long nr_reclaimed = sc->nr_reclaimed;

	/* Reclaim a number of pages proportional to the number of zones */
	sc->nr_to_reclaim = 0;
	for_each_managed_zone_pgdat(zone, pgdat, z, sc->reclaim_idx) {
		sc->nr_to_reclaim += max(high_wmark_pages(zone), SWAP_CLUSTER_MAX);
	}

	/*
	 * Historically care was taken to put equal pressure on all zones but
	 * now pressure is applied based on node LRU order.
	 */
	shrink_node(pgdat, sc);

	/*
	 * Fragmentation may mean that the system cannot be rebalanced for
	 * high-order allocations. If twice the allocation size has been
	 * reclaimed then recheck watermarks only at order-0 to prevent
	 * excessive reclaim. Assume that a process requested a high-order
	 * can direct reclaim/compact.
	 */
	if (sc->order && sc->nr_reclaimed >= compact_gap(sc->order))
		sc->order = 0;

	/* account for progress from mm_account_reclaimed_pages() */
	return max(sc->nr_scanned, sc->nr_reclaimed - nr_reclaimed) >= sc->nr_to_reclaim;
}

/* Page allocator PCP high watermark is lowered if reclaim is active. */
static inline void
update_reclaim_active(pg_data_t *pgdat, int highest_zoneidx, bool active)
{
	int i;
	struct zone *zone;

	for_each_managed_zone_pgdat(zone, pgdat, i, highest_zoneidx) {
		if (active)
			set_bit(ZONE_RECLAIM_ACTIVE, &zone->flags);
		else
			clear_bit(ZONE_RECLAIM_ACTIVE, &zone->flags);
	}
}

static inline void
set_reclaim_active(pg_data_t *pgdat, int highest_zoneidx)
{
	update_reclaim_active(pgdat, highest_zoneidx, true);
}

static inline void
clear_reclaim_active(pg_data_t *pgdat, int highest_zoneidx)
{
	update_reclaim_active(pgdat, highest_zoneidx, false);
}

/*
 * For kswapd, balance_pgdat() will reclaim pages across a node from zones
 * that are eligible for use by the caller until at least one zone is
 * balanced.
 *
 * Returns the order kswapd finished reclaiming at.
 *
 * kswapd scans the zones in the highmem->normal->dma direction.  It skips
 * zones which have free_pages > high_wmark_pages(zone), but once a zone is
 * found to have free_pages <= high_wmark_pages(zone), any page in that zone
 * or lower is eligible for reclaim until at least one usable zone is
 * balanced.
 */
static int balance_pgdat(pg_data_t *pgdat, int order, int highest_zoneidx)
{
	int i;
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	unsigned long pflags;
	unsigned long nr_boost_reclaim;
	unsigned long zone_boosts[MAX_NR_ZONES] = { 0, };
	bool boosted;
	struct zone *zone;
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.order = order,
		.may_unmap = 1,
	};

	set_task_reclaim_state(current, &sc.reclaim_state);
	psi_memstall_enter(&pflags);
	__fs_reclaim_acquire(_THIS_IP_);

	count_vm_event(PAGEOUTRUN);

	/*
	 * Account for the reclaim boost. Note that the zone boost is left in
	 * place so that parallel allocations that are near the watermark will
	 * stall or direct reclaim until kswapd is finished.
	 */
	nr_boost_reclaim = 0;
	for_each_managed_zone_pgdat(zone, pgdat, i, highest_zoneidx) {
		nr_boost_reclaim += zone->watermark_boost;
		zone_boosts[i] = zone->watermark_boost;
	}
	boosted = nr_boost_reclaim;

restart:
	set_reclaim_active(pgdat, highest_zoneidx);
	sc.priority = DEF_PRIORITY;
	do {
		unsigned long nr_reclaimed = sc.nr_reclaimed;
		bool raise_priority = true;
		bool balanced;
		bool ret;
		bool was_frozen;

		sc.reclaim_idx = highest_zoneidx;

		/*
		 * If the number of buffer_heads exceeds the maximum allowed
		 * then consider reclaiming from all zones. This has a dual
		 * purpose -- on 64-bit systems it is expected that
		 * buffer_heads are stripped during active rotation. On 32-bit
		 * systems, highmem pages can pin lowmem memory and shrinking
		 * buffers can relieve lowmem pressure. Reclaim may still not
		 * go ahead if all eligible zones for the original allocation
		 * request are balanced to avoid excessive reclaim from kswapd.
		 */
		if (buffer_heads_over_limit) {
			for (i = MAX_NR_ZONES - 1; i >= 0; i--) {
				zone = pgdat->node_zones + i;
				if (!managed_zone(zone))
					continue;

				sc.reclaim_idx = i;
				break;
			}
		}

		/*
		 * If the pgdat is imbalanced then ignore boosting and preserve
		 * the watermarks for a later time and restart. Note that the
		 * zone watermarks will be still reset at the end of balancing
		 * on the grounds that the normal reclaim should be enough to
		 * re-evaluate if boosting is required when kswapd next wakes.
		 */
		balanced = pgdat_balanced(pgdat, sc.order, highest_zoneidx);
		if (!balanced && nr_boost_reclaim) {
			nr_boost_reclaim = 0;
			goto restart;
		}

		/*
		 * If boosting is not active then only reclaim if there are no
		 * eligible zones. Note that sc.reclaim_idx is not used as
		 * buffer_heads_over_limit may have adjusted it.
		 */
		if (!nr_boost_reclaim && balanced)
			goto out;

		/* Limit the priority of boosting to avoid reclaim writeback */
		if (nr_boost_reclaim && sc.priority == DEF_PRIORITY - 2)
			raise_priority = false;

		/*
		 * Do not writeback or swap pages for boosted reclaim. The
		 * intent is to relieve pressure not issue sub-optimal IO
		 * from reclaim context. If no pages are reclaimed, the
		 * reclaim will be aborted.
		 */
		sc.may_writepage = !nr_boost_reclaim;
		sc.may_swap = !nr_boost_reclaim;

		/*
		 * Do some background aging, to give pages a chance to be
		 * referenced before reclaiming. All pages are rotated
		 * regardless of classzone as this is about consistent aging.
		 */
		if (lru_gen_enabled())
			lru_gen_age_node(pgdat, &sc);
		else
			lru_age_node(pgdat, &sc);

		/* Call soft limit reclaim before calling shrink_node. */
		sc.nr_scanned = 0;
		nr_soft_scanned = 0;
		nr_soft_reclaimed = memcg1_soft_limit_reclaim(pgdat, sc.order,
							      sc.gfp_mask, &nr_soft_scanned);
		sc.nr_reclaimed += nr_soft_reclaimed;

		/*
		 * There should be no need to raise the scanning priority if
		 * enough pages are already being scanned that that high
		 * watermark would be met at 100% efficiency.
		 */
		if (kswapd_shrink_node(pgdat, &sc))
			raise_priority = false;

		/*
		 * If the low watermark is met there is no need for processes
		 * to be throttled on pfmemalloc_wait as they should not be
		 * able to safely make forward progress. Wake them
		 */
		if (waitqueue_active(&pgdat->pfmemalloc_wait) &&
				allow_direct_reclaim(pgdat))
			wake_up_all(&pgdat->pfmemalloc_wait);

		/* Check if kswapd should be suspending */
		__fs_reclaim_release(_THIS_IP_);
		ret = kthread_freezable_should_stop(&was_frozen);
		__fs_reclaim_acquire(_THIS_IP_);
		if (was_frozen || ret)
			break;

		/*
		 * Raise priority if scanning rate is too low or there was no
		 * progress in reclaiming pages
		 */
		nr_reclaimed = sc.nr_reclaimed - nr_reclaimed;
		nr_boost_reclaim -= min(nr_boost_reclaim, nr_reclaimed);

		/*
		 * If reclaim made no progress for a boost, stop reclaim as
		 * IO cannot be queued and it could be an infinite loop in
		 * extreme circumstances.
		 */
		if (nr_boost_reclaim && !nr_reclaimed)
			break;

		if (raise_priority || !nr_reclaimed)
			sc.priority--;
	} while (sc.priority >= 1);

	/*
	 * Restart only if it went through the priority loop all the way,
	 * but cache_trim_mode didn't work.
	 */
	if (!sc.nr_reclaimed && sc.priority < 1 &&
	    !sc.no_cache_trim_mode && sc.cache_trim_mode_failed) {
		sc.no_cache_trim_mode = 1;
		goto restart;
	}

	/*
	 * If the reclaim was boosted, we might still be far from the
	 * watermark_high at this point. We need to avoid increasing the
	 * failure count to prevent the kswapd thread from stopping.
	 */
	if (!sc.nr_reclaimed && !boosted) {
		int fail_cnt = atomic_inc_return(&pgdat->kswapd_failures);
		/* kswapd context, low overhead to trace every failure */
		trace_mm_vmscan_kswapd_reclaim_fail(pgdat->node_id, fail_cnt);
	}

out:
	clear_reclaim_active(pgdat, highest_zoneidx);

	/* If reclaim was boosted, account for the reclaim done in this pass */
	if (boosted) {
		unsigned long flags;

		for (i = 0; i <= highest_zoneidx; i++) {
			if (!zone_boosts[i])
				continue;

			/* Increments are under the zone lock */
			zone = pgdat->node_zones + i;
			spin_lock_irqsave(&zone->lock, flags);
			zone->watermark_boost -= min(zone->watermark_boost, zone_boosts[i]);
			spin_unlock_irqrestore(&zone->lock, flags);
		}

		/*
		 * As there is now likely space, wakeup kcompact to defragment
		 * pageblocks.
		 */
		wakeup_kcompactd(pgdat, pageblock_order, highest_zoneidx);
	}

	if (!lru_gen_enabled())
		lru_snapshot_refaults(NULL, pgdat);

	__fs_reclaim_release(_THIS_IP_);
	psi_memstall_leave(&pflags);
	set_task_reclaim_state(current, NULL);

	/*
	 * Return the order kswapd stopped reclaiming at as
	 * prepare_kswapd_sleep() takes it into account. If another caller
	 * entered the allocator slow path while kswapd was awake, order will
	 * remain at the higher level.
	 */
	return sc.order;
}

/*
 * The pgdat->kswapd_highest_zoneidx is used to pass the highest zone index to
 * be reclaimed by kswapd from the waker. If the value is MAX_NR_ZONES which is
 * not a valid index then either kswapd runs for first time or kswapd couldn't
 * sleep after previous reclaim attempt (node is still unbalanced). In that
 * case return the zone index of the previous kswapd reclaim cycle.
 */
static enum zone_type kswapd_highest_zoneidx(pg_data_t *pgdat,
					   enum zone_type prev_highest_zoneidx)
{
	enum zone_type curr_idx = READ_ONCE(pgdat->kswapd_highest_zoneidx);

	return curr_idx == MAX_NR_ZONES ? prev_highest_zoneidx : curr_idx;
}

static void kswapd_try_to_sleep(pg_data_t *pgdat, int alloc_order, int reclaim_order,
				unsigned int highest_zoneidx)
{
	long remaining = 0;
	DEFINE_WAIT(wait);

	if (freezing(current) || kthread_should_stop())
		return;

	prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);

	/*
	 * Try to sleep for a short interval. Note that kcompactd will only be
	 * woken if it is possible to sleep for a short interval. This is
	 * deliberate on the assumption that if reclaim cannot keep an
	 * eligible zone balanced that it's also unlikely that compaction will
	 * succeed.
	 */
	if (prepare_kswapd_sleep(pgdat, reclaim_order, highest_zoneidx)) {
		/*
		 * Compaction records what page blocks it recently failed to
		 * isolate pages from and skips them in the future scanning.
		 * When kswapd is going to sleep, it is reasonable to assume
		 * that pages and compaction may succeed so reset the cache.
		 */
		reset_isolation_suitable(pgdat);

		/*
		 * We have freed the memory, now we should compact it to make
		 * allocation of the requested order possible.
		 */
		wakeup_kcompactd(pgdat, alloc_order, highest_zoneidx);

		remaining = schedule_timeout(HZ/10);

		/*
		 * If woken prematurely then reset kswapd_highest_zoneidx and
		 * order. The values will either be from a wakeup request or
		 * the previous request that slept prematurely.
		 */
		if (remaining) {
			WRITE_ONCE(pgdat->kswapd_highest_zoneidx,
					kswapd_highest_zoneidx(pgdat,
							highest_zoneidx));

			if (READ_ONCE(pgdat->kswapd_order) < reclaim_order)
				WRITE_ONCE(pgdat->kswapd_order, reclaim_order);
		}

		finish_wait(&pgdat->kswapd_wait, &wait);
		prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);
	}

	/*
	 * After a short sleep, check if it was a premature sleep. If not, then
	 * go fully to sleep until explicitly woken up.
	 */
	if (!remaining &&
	    prepare_kswapd_sleep(pgdat, reclaim_order, highest_zoneidx)) {
		trace_mm_vmscan_kswapd_sleep(pgdat->node_id);

		/*
		 * vmstat counters are not perfectly accurate and the estimated
		 * value for counters such as NR_FREE_PAGES can deviate from the
		 * true value by nr_online_cpus * threshold. To avoid the zone
		 * watermarks being breached while under pressure, we reduce the
		 * per-cpu vmstat threshold while kswapd is awake and restore
		 * them before going back to sleep.
		 */
		set_pgdat_percpu_threshold(pgdat, calculate_normal_threshold);

		if (!kthread_should_stop())
			schedule();

		set_pgdat_percpu_threshold(pgdat, calculate_pressure_threshold);
	} else {
		if (remaining)
			count_vm_event(KSWAPD_LOW_WMARK_HIT_QUICKLY);
		else
			count_vm_event(KSWAPD_HIGH_WMARK_HIT_QUICKLY);
	}
	finish_wait(&pgdat->kswapd_wait, &wait);
}

/*
 * The background pageout daemon, started as a kernel thread
 * from the init process.
 *
 * This basically trickles out pages so that we have _some_
 * free memory available even if there is no other activity
 * that frees anything up. This is needed for things like routing
 * etc, where we otherwise might have all activity going on in
 * asynchronous contexts that cannot page things out.
 *
 * If there are applications that are active memory-allocators
 * (most normal use), this basically shouldn't matter.
 */
static int kswapd(void *p)
{
	unsigned int alloc_order, reclaim_order;
	unsigned int highest_zoneidx = MAX_NR_ZONES - 1;
	pg_data_t *pgdat = (pg_data_t *)p;
	struct task_struct *tsk = current;

	/*
	 * Tell the memory management that we're a "memory allocator",
	 * and that if we need more memory we should get access to it
	 * regardless (see "__alloc_pages()"). "kswapd" should
	 * never get caught in the normal page freeing logic.
	 *
	 * (Kswapd normally doesn't need memory anyway, but sometimes
	 * you need a small amount of memory in order to be able to
	 * page out something else, and this flag essentially protects
	 * us from recursively trying to free more memory as we're
	 * trying to free the first piece of memory in the first place).
	 */
	tsk->flags |= PF_MEMALLOC | PF_KSWAPD;
	set_freezable();

	WRITE_ONCE(pgdat->kswapd_order, 0);
	WRITE_ONCE(pgdat->kswapd_highest_zoneidx, MAX_NR_ZONES);
	atomic_set(&pgdat->nr_writeback_throttled, 0);
	for ( ; ; ) {
		bool was_frozen;

		alloc_order = reclaim_order = READ_ONCE(pgdat->kswapd_order);
		highest_zoneidx = kswapd_highest_zoneidx(pgdat,
							highest_zoneidx);

kswapd_try_sleep:
		kswapd_try_to_sleep(pgdat, alloc_order, reclaim_order,
					highest_zoneidx);

		/* Read the new order and highest_zoneidx */
		alloc_order = READ_ONCE(pgdat->kswapd_order);
		highest_zoneidx = kswapd_highest_zoneidx(pgdat,
							highest_zoneidx);
		WRITE_ONCE(pgdat->kswapd_order, 0);
		WRITE_ONCE(pgdat->kswapd_highest_zoneidx, MAX_NR_ZONES);

		if (kthread_freezable_should_stop(&was_frozen))
			break;

		/*
		 * We can speed up thawing tasks if we don't call balance_pgdat
		 * after returning from the refrigerator
		 */
		if (was_frozen)
			continue;

		/*
		 * Reclaim begins at the requested order but if a high-order
		 * reclaim fails then kswapd falls back to reclaiming for
		 * order-0. If that happens, kswapd will consider sleeping
		 * for the order it finished reclaiming at (reclaim_order)
		 * but kcompactd is woken to compact for the original
		 * request (alloc_order).
		 */
		trace_mm_vmscan_kswapd_wake(pgdat->node_id, highest_zoneidx,
						alloc_order);
		reclaim_order = balance_pgdat(pgdat, alloc_order,
						highest_zoneidx);
		if (reclaim_order < alloc_order)
			goto kswapd_try_sleep;
	}

	tsk->flags &= ~(PF_MEMALLOC | PF_KSWAPD);

	return 0;
}

/*
 * A zone is low on free memory or too fragmented for high-order memory.  If
 * kswapd should reclaim (direct reclaim is deferred), wake it up for the zone's
 * pgdat.  It will wake up kcompactd after reclaiming memory.  If kswapd reclaim
 * has failed or is not needed, still wake up kcompactd if only compaction is
 * needed.
 */
void wakeup_kswapd(struct zone *zone, gfp_t gfp_flags, int order,
		   enum zone_type highest_zoneidx)
{
	pg_data_t *pgdat;
	enum zone_type curr_idx;

	if (!managed_zone(zone))
		return;

	if (!cpuset_zone_allowed(zone, gfp_flags))
		return;

	pgdat = zone->zone_pgdat;
	curr_idx = READ_ONCE(pgdat->kswapd_highest_zoneidx);

	if (curr_idx == MAX_NR_ZONES || curr_idx < highest_zoneidx)
		WRITE_ONCE(pgdat->kswapd_highest_zoneidx, highest_zoneidx);

	if (READ_ONCE(pgdat->kswapd_order) < order)
		WRITE_ONCE(pgdat->kswapd_order, order);

	if (!waitqueue_active(&pgdat->kswapd_wait))
		return;

	/* Hopeless node, leave it to direct reclaim if possible */
	if (kswapd_test_hopeless(pgdat) ||
	    (pgdat_balanced(pgdat, order, highest_zoneidx) &&
	     !pgdat_watermark_boosted(pgdat, highest_zoneidx))) {
		/*
		 * There may be plenty of free memory available, but it's too
		 * fragmented for high-order allocations.  Wake up kcompactd
		 * and rely on compaction_suitable() to determine if it's
		 * needed.  If it fails, it will defer subsequent attempts to
		 * ratelimit its work.
		 */
		if (!(gfp_flags & __GFP_DIRECT_RECLAIM))
			wakeup_kcompactd(pgdat, order, highest_zoneidx);
		return;
	}

	trace_mm_vmscan_wakeup_kswapd(pgdat->node_id, highest_zoneidx, order,
				      gfp_flags);
	wake_up_interruptible(&pgdat->kswapd_wait);
}

void kswapd_clear_hopeless(pg_data_t *pgdat, enum kswapd_clear_hopeless_reason reason)
{
	/* Only trace actual resets, not redundant zero-to-zero */
	if (atomic_xchg(&pgdat->kswapd_failures, 0))
		trace_mm_vmscan_kswapd_clear_hopeless(pgdat->node_id, reason);
}

/*
 * Reset kswapd_failures only when the node is balanced. Without this
 * check, successful direct reclaim (e.g., from cgroup memory.high
 * throttling) can keep resetting kswapd_failures even when the node
 * cannot be balanced, causing kswapd to run endlessly.
 */
void kswapd_try_clear_hopeless(struct pglist_data *pgdat,
			       unsigned int order, int highest_zoneidx)
{
	if (pgdat_balanced(pgdat, order, highest_zoneidx))
		kswapd_clear_hopeless(pgdat, current_is_kswapd() ?
			KSWAPD_CLEAR_HOPELESS_KSWAPD : KSWAPD_CLEAR_HOPELESS_DIRECT);
}

bool kswapd_test_hopeless(pg_data_t *pgdat)
{
	return atomic_read(&pgdat->kswapd_failures) >= MAX_RECLAIM_RETRIES;
}

#ifdef CONFIG_HIBERNATION
/*
 * Try to free `nr_to_reclaim' of memory, system-wide, and return the number of
 * freed pages.
 *
 * Rather than trying to age LRUs the aim is to preserve the overall
 * LRU order by reclaiming preferentially
 * inactive > active > active referenced > active mapped
 */
unsigned long shrink_all_memory(unsigned long nr_to_reclaim)
{
	struct scan_control sc = {
		.nr_to_reclaim = nr_to_reclaim,
		.gfp_mask = GFP_HIGHUSER_MOVABLE,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.priority = DEF_PRIORITY,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
		.hibernation_mode = 1,
	};
	struct zonelist *zonelist = node_zonelist(numa_node_id(), sc.gfp_mask);
	unsigned long nr_reclaimed;
	unsigned int noreclaim_flag;

	fs_reclaim_acquire(sc.gfp_mask);
	noreclaim_flag = memalloc_noreclaim_save();
	set_task_reclaim_state(current, &sc.reclaim_state);

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	set_task_reclaim_state(current, NULL);
	memalloc_noreclaim_restore(noreclaim_flag);
	fs_reclaim_release(sc.gfp_mask);

	return nr_reclaimed;
}
#endif /* CONFIG_HIBERNATION */

/*
 * This kswapd start function will be called by init and node-hot-add.
 */
void __meminit kswapd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);

	pgdat_kswapd_lock(pgdat);
	if (!pgdat->kswapd) {
		pgdat->kswapd = kthread_create_on_node(kswapd, pgdat, nid, "kswapd%d", nid);
		if (IS_ERR(pgdat->kswapd)) {
			/* failure at boot is fatal */
			pr_err("Failed to start kswapd on node %d, ret=%pe\n",
				   nid, pgdat->kswapd);
			BUG_ON(system_state < SYSTEM_RUNNING);
			pgdat->kswapd = NULL;
		} else {
			wake_up_process(pgdat->kswapd);
		}
	}
	pgdat_kswapd_unlock(pgdat);
}

/*
 * Called by memory hotplug when all memory in a node is offlined.  Caller must
 * be holding mem_hotplug_begin/done().
 */
void __meminit kswapd_stop(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	struct task_struct *kswapd;

	pgdat_kswapd_lock(pgdat);
	kswapd = pgdat->kswapd;
	if (kswapd) {
		kthread_stop(kswapd);
		pgdat->kswapd = NULL;
	}
	pgdat_kswapd_unlock(pgdat);
}

static const struct ctl_table vmscan_sysctl_table[] = {
	{
		.procname	= "swappiness",
		.data		= &vm_swappiness,
		.maxlen		= sizeof(vm_swappiness),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_TWO_HUNDRED,
	},
#ifdef CONFIG_NUMA
	{
		.procname	= "zone_reclaim_mode",
		.data		= &node_reclaim_mode,
		.maxlen		= sizeof(node_reclaim_mode),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
	}
#endif
};

static int __init kswapd_init(void)
{
	int nid;

	swap_setup();
	for_each_node_state(nid, N_MEMORY)
 		kswapd_run(nid);
	register_sysctl_init("vm", vmscan_sysctl_table);
	return 0;
}

module_init(kswapd_init)

#ifdef CONFIG_NUMA
/*
 * Node reclaim mode
 *
 * If non-zero call node_reclaim when the number of free pages falls below
 * the watermarks.
 */
int node_reclaim_mode __read_mostly;

/*
 * Priority for NODE_RECLAIM. This determines the fraction of pages
 * of a node considered for each zone_reclaim. 4 scans 1/16th of
 * a zone.
 */
#define NODE_RECLAIM_PRIORITY 4

/*
 * Percentage of pages in a zone that must be unmapped for node_reclaim to
 * occur.
 */
int sysctl_min_unmapped_ratio = 1;

/*
 * If the number of slab pages in a zone grows beyond this percentage then
 * slab reclaim needs to occur.
 */
int sysctl_min_slab_ratio = 5;

static inline unsigned long node_unmapped_file_pages(struct pglist_data *pgdat)
{
	unsigned long file_mapped = node_page_state(pgdat, NR_FILE_MAPPED);
	unsigned long file_lru = node_page_state(pgdat, NR_INACTIVE_FILE) +
		node_page_state(pgdat, NR_ACTIVE_FILE);

	/*
	 * It's possible for there to be more file mapped pages than
	 * accounted for by the pages on the file LRU lists because
	 * tmpfs pages accounted for as ANON can also be FILE_MAPPED
	 */
	return (file_lru > file_mapped) ? (file_lru - file_mapped) : 0;
}

/* Work out how many page cache pages we can reclaim in this reclaim_mode */
static unsigned long node_pagecache_reclaimable(struct pglist_data *pgdat)
{
	unsigned long nr_pagecache_reclaimable;
	unsigned long delta = 0;

	/*
	 * If RECLAIM_UNMAP is set, then all file pages are considered
	 * potentially reclaimable. Otherwise, we have to worry about
	 * pages like swapcache and node_unmapped_file_pages() provides
	 * a better estimate
	 */
	if (node_reclaim_mode & RECLAIM_UNMAP)
		nr_pagecache_reclaimable = node_page_state(pgdat, NR_FILE_PAGES);
	else
		nr_pagecache_reclaimable = node_unmapped_file_pages(pgdat);

	/*
	 * Since we can't clean folios through reclaim, remove dirty file
	 * folios from consideration.
	 */
	delta += node_page_state(pgdat, NR_FILE_DIRTY);

	/* Watch for any possible underflows due to delta */
	if (unlikely(delta > nr_pagecache_reclaimable))
		delta = nr_pagecache_reclaimable;

	return nr_pagecache_reclaimable - delta;
}

/*
 * Try to free up some pages from this node through reclaim.
 */
static unsigned long __node_reclaim(struct pglist_data *pgdat, gfp_t gfp_mask,
				    unsigned long nr_pages,
				    struct scan_control *sc)
{
	struct task_struct *p = current;
	unsigned int noreclaim_flag;
	unsigned long pflags;

	trace_mm_vmscan_node_reclaim_begin(pgdat->node_id, sc->order,
					   sc->gfp_mask);

	cond_resched();
	psi_memstall_enter(&pflags);
	delayacct_freepages_start();
	fs_reclaim_acquire(sc->gfp_mask);
	/*
	 * We need to be able to allocate from the reserves for RECLAIM_UNMAP
	 */
	noreclaim_flag = memalloc_noreclaim_save();
	set_task_reclaim_state(p, &sc->reclaim_state);

	if (node_pagecache_reclaimable(pgdat) > pgdat->min_unmapped_pages ||
	    node_page_state_pages(pgdat, NR_SLAB_RECLAIMABLE_B) > pgdat->min_slab_pages) {
		/*
		 * Free memory by calling shrink node with increasing
		 * priorities until we have enough memory freed.
		 */
		do {
			shrink_node(pgdat, sc);
		} while (sc->nr_reclaimed < nr_pages && --sc->priority >= 0);
	}

	set_task_reclaim_state(p, NULL);
	memalloc_noreclaim_restore(noreclaim_flag);
	fs_reclaim_release(sc->gfp_mask);
	delayacct_freepages_end();
	psi_memstall_leave(&pflags);

	trace_mm_vmscan_node_reclaim_end(sc->nr_reclaimed);

	return sc->nr_reclaimed;
}

int node_reclaim(struct pglist_data *pgdat, gfp_t gfp_mask, unsigned int order)
{
	int ret;
	/* Minimum pages needed in order to stay on node */
	const unsigned long nr_pages = 1 << order;
	struct scan_control sc = {
		.nr_to_reclaim = max(nr_pages, SWAP_CLUSTER_MAX),
		.gfp_mask = current_gfp_context(gfp_mask),
		.order = order,
		.priority = NODE_RECLAIM_PRIORITY,
		.may_writepage = !!(node_reclaim_mode & RECLAIM_WRITE),
		.may_unmap = !!(node_reclaim_mode & RECLAIM_UNMAP),
		.may_swap = 1,
		.reclaim_idx = gfp_zone(gfp_mask),
	};

	/*
	 * Node reclaim reclaims unmapped file backed pages and
	 * slab pages if we are over the defined limits.
	 *
	 * A small portion of unmapped file backed pages is needed for
	 * file I/O otherwise pages read by file I/O will be immediately
	 * thrown out if the node is overallocated. So we do not reclaim
	 * if less than a specified percentage of the node is used by
	 * unmapped file backed pages.
	 */
	if (node_pagecache_reclaimable(pgdat) <= pgdat->min_unmapped_pages &&
	    node_page_state_pages(pgdat, NR_SLAB_RECLAIMABLE_B) <=
	    pgdat->min_slab_pages)
		return NODE_RECLAIM_FULL;

	/*
	 * Do not scan if the allocation should not be delayed.
	 */
	if (!gfpflags_allow_blocking(gfp_mask) || (current->flags & PF_MEMALLOC))
		return NODE_RECLAIM_NOSCAN;

	/*
	 * Only run node reclaim on the local node or on nodes that do not
	 * have associated processors. This will favor the local processor
	 * over remote processors and spread off node memory allocations
	 * as wide as possible.
	 */
	if (node_state(pgdat->node_id, N_CPU) && pgdat->node_id != numa_node_id())
		return NODE_RECLAIM_NOSCAN;

	if (test_and_set_bit_lock(PGDAT_RECLAIM_LOCKED, &pgdat->flags))
		return NODE_RECLAIM_NOSCAN;

	ret = __node_reclaim(pgdat, gfp_mask, nr_pages, &sc) >= nr_pages;
	clear_bit_unlock(PGDAT_RECLAIM_LOCKED, &pgdat->flags);

	if (ret)
		count_vm_event(PGSCAN_ZONE_RECLAIM_SUCCESS);
	else
		count_vm_event(PGSCAN_ZONE_RECLAIM_FAILED);

	return ret;
}

#else

static unsigned long __node_reclaim(struct pglist_data *pgdat, gfp_t gfp_mask,
				    unsigned long nr_pages,
				    struct scan_control *sc)
{
	return 0;
}

#endif

enum {
	MEMORY_RECLAIM_SWAPPINESS = 0,
	MEMORY_RECLAIM_SWAPPINESS_MAX,
	MEMORY_RECLAIM_NULL,
};
static const match_table_t tokens = {
	{ MEMORY_RECLAIM_SWAPPINESS, "swappiness=%d"},
	{ MEMORY_RECLAIM_SWAPPINESS_MAX, "swappiness=max"},
	{ MEMORY_RECLAIM_NULL, NULL },
};

int user_proactive_reclaim(char *buf,
			   struct mem_cgroup *memcg, pg_data_t *pgdat)
{
	unsigned int nr_retries = MAX_RECLAIM_RETRIES;
	unsigned long nr_to_reclaim, nr_reclaimed = 0;
	int swappiness = -1;
	char *old_buf, *start;
	substring_t args[MAX_OPT_ARGS];
	gfp_t gfp_mask = GFP_KERNEL;

	if (!buf || (!memcg && !pgdat) || (memcg && pgdat))
		return -EINVAL;

	buf = strstrip(buf);

	old_buf = buf;
	nr_to_reclaim = memparse(buf, &buf) / PAGE_SIZE;
	if (buf == old_buf)
		return -EINVAL;

	buf = strstrip(buf);

	while ((start = strsep(&buf, " ")) != NULL) {
		if (!strlen(start))
			continue;
		switch (match_token(start, tokens, args)) {
		case MEMORY_RECLAIM_SWAPPINESS:
			if (match_int(&args[0], &swappiness))
				return -EINVAL;
			if (swappiness < MIN_SWAPPINESS ||
			    swappiness > MAX_SWAPPINESS)
				return -EINVAL;
			break;
		case MEMORY_RECLAIM_SWAPPINESS_MAX:
			swappiness = SWAPPINESS_ANON_ONLY;
			break;
		default:
			return -EINVAL;
		}
	}

	while (nr_reclaimed < nr_to_reclaim) {
		/* Will converge on zero, but reclaim enforces a minimum */
		unsigned long batch_size = (nr_to_reclaim - nr_reclaimed) / 4;
		unsigned long reclaimed;

		if (signal_pending(current))
			return -EINTR;

		/*
		 * This is the final attempt, drain percpu lru caches in the
		 * hope of introducing more evictable pages.
		 */
		if (!nr_retries)
			lru_add_drain_all();

		if (memcg) {
			unsigned int reclaim_options;

			reclaim_options = MEMCG_RECLAIM_MAY_SWAP |
					  MEMCG_RECLAIM_PROACTIVE;
			reclaimed = try_to_free_mem_cgroup_pages(memcg,
						 batch_size, gfp_mask,
						 reclaim_options,
						 swappiness == -1 ? NULL : &swappiness);
		} else {
			struct scan_control sc = {
				.gfp_mask = current_gfp_context(gfp_mask),
				.reclaim_idx = gfp_zone(gfp_mask),
				.proactive_swappiness = swappiness == -1 ? NULL : &swappiness,
				.priority = DEF_PRIORITY,
				.may_writepage = 1,
				.nr_to_reclaim = max(batch_size, SWAP_CLUSTER_MAX),
				.may_unmap = 1,
				.may_swap = 1,
				.proactive = 1,
			};

			if (test_and_set_bit_lock(PGDAT_RECLAIM_LOCKED,
						  &pgdat->flags))
				return -EBUSY;

			reclaimed = __node_reclaim(pgdat, gfp_mask,
						   batch_size, &sc);
			clear_bit_unlock(PGDAT_RECLAIM_LOCKED, &pgdat->flags);
		}

		if (!reclaimed && !nr_retries--)
			return -EAGAIN;

		nr_reclaimed += reclaimed;
	}

	return 0;
}

/**
 * check_move_unevictable_folios - Move evictable folios to appropriate zone
 * lru list
 * @fbatch: Batch of lru folios to check.
 *
 * Checks folios for evictability, if an evictable folio is in the unevictable
 * lru list, moves it to the appropriate evictable lru list. This function
 * should be only used for lru folios.
 */
void check_move_unevictable_folios(struct folio_batch *fbatch)
{
	struct lruvec *lruvec = NULL;
	int pgscanned = 0;
	int pgrescued = 0;
	int i;

	for (i = 0; i < fbatch->nr; i++) {
		struct folio *folio = fbatch->folios[i];
		int nr_pages = folio_nr_pages(folio);

		pgscanned += nr_pages;

		/* block memcg migration while the folio moves between lrus */
		if (!folio_test_clear_lru(folio))
			continue;

		lruvec = folio_lruvec_relock_irq(folio, lruvec);
		if (folio_evictable(folio) && folio_test_unevictable(folio)) {
			lruvec_del_folio(lruvec, folio);
			folio_clear_unevictable(folio);
			lruvec_add_folio(lruvec, folio);
			pgrescued += nr_pages;
		}
		folio_set_lru(folio);
	}

	if (lruvec) {
		__count_vm_events(UNEVICTABLE_PGRESCUED, pgrescued);
		__count_vm_events(UNEVICTABLE_PGSCANNED, pgscanned);
		unlock_page_lruvec_irq(lruvec);
	} else if (pgscanned) {
		count_vm_events(UNEVICTABLE_PGSCANNED, pgscanned);
	}
}
EXPORT_SYMBOL_GPL(check_move_unevictable_folios);

#if defined(CONFIG_SYSFS) && defined(CONFIG_NUMA)
static ssize_t reclaim_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	int ret, nid = dev->id;

	ret = user_proactive_reclaim((char *)buf, NULL, NODE_DATA(nid));
	return ret ? -EAGAIN : count;
}

static DEVICE_ATTR_WO(reclaim);
int reclaim_register_node(struct node *node)
{
	return device_create_file(&node->dev, &dev_attr_reclaim);
}

void reclaim_unregister_node(struct node *node)
{
	return device_remove_file(&node->dev, &dev_attr_reclaim);
}
#endif
