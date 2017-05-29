#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/frontswap.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>

#define UNUSED (0)
#define IN_USE (1)
#define RECLAIMED (2)

static u64 current_pages;

struct page_list {
	/* These two could be merged into a union */
	struct list_head list_head;
	struct hlist_node hash_node;
	pgoff_t index;
	/* This seems to be the standard way of storing addresses */
	unsigned long addr;
	/* For debugging purposes */
	u8 state;
};

LIST_HEAD(pool_head);
DEFINE_SPINLOCK(pool_lock);

DEFINE_HASHTABLE(used_pages, 10);
DEFINE_SPINLOCK(used_lock);


#define TMEM_POOL_ID (0)
#define TMEM_OBJ_ID (0)
#define TMEM_POOL_SIZE (64 * 1024)

/* generic tmem ops */

static int pagelist_tmem_put_page(pgoff_t index, struct page *page)
{
	struct page_list *page_entry = NULL;
	int already_exists = 0;
	unsigned long flags;

	pr_debug("entering put_page\n");


	/* If the page already exists, update it */
	spin_lock_irqsave(&used_lock, flags);
	hash_for_each_possible(used_pages, page_entry, hash_node, index) {
		if (page_entry->index == index) { 
			already_exists = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&used_lock, flags);

	/* Or else get a new one from the pool */
	if (!already_exists) {
		spin_lock_irqsave(&pool_lock, flags);
		if (list_empty(&pool_head)) {
			spin_unlock_irqrestore(&pool_lock, flags);
			pr_debug("leaving put_page - failed\n");
			return -ENOMEM;
		}
	
		page_entry = list_first_entry(&pool_head, struct page_list, list_head);
		list_del(&page_entry->list_head);
		spin_unlock_irqrestore(&pool_lock, flags);
	
		if (page_entry->state == IN_USE)
			pr_err("put_page: page in linked list in state %x\n", page_entry->state);
		page_entry->state = IN_USE;
	}


	memcpy((void *) page_entry->addr, page_address(page), PAGE_SIZE);
	page_entry->index = index;

	if(!already_exists){
		spin_lock_irqsave(&used_lock, flags);
		hash_add(used_pages, &page_entry->hash_node, index);
		spin_unlock_irqrestore(&used_lock, flags);
	
		current_pages++;
	}
	
	pr_err("leaving put_page\n");
	
	return 0;
}
EXPORT_SYMBOL(pagelist_tmem_put_page);

static int pagelist_tmem_get_page(pgoff_t index, struct page *page)
{
	struct page_list *page_entry;
	unsigned long flags;

	pr_debug("entering get_page\n");
	spin_lock_irqsave(&used_lock, flags);
	hash_for_each_possible(used_pages, page_entry, hash_node, index) {
		if (page_entry->index == index) {

			memcpy(page_address(page), (void *) page_entry->addr, PAGE_SIZE);
			spin_unlock_irqrestore(&used_lock, flags);

			if (page_entry->state != IN_USE)
				pr_err("get_page: page in hashtable in state %x\n", page_entry->state);

			pr_err("leaving get_page\n");

			return 0;
		}
	}

	spin_unlock_irqrestore(&used_lock, flags);
	pr_warn("leaving get_page - failed\n");

	return -EINVAL;
}
EXPORT_SYMBOL(pagelist_tmem_get_page);

static void pagelist_tmem_invalidate_page(pgoff_t index)
{
	struct page_list *page_entry;
	unsigned long flags;

	pr_debug("entering invalidate_page\n");

	spin_lock_irqsave(&used_lock, flags);
	hash_for_each_possible(used_pages, page_entry, hash_node, index) {
		if (page_entry->index == index) {
			hash_del(&page_entry->hash_node);
			spin_unlock_irqrestore(&used_lock, flags);

			pr_err("invalidation of %lld successful\n", index);

			if (page_entry->state != IN_USE)
				pr_err("invalidate page: page in hashtable in state %x\n", page_entry->state);
			page_entry->state = RECLAIMED;

			spin_lock_irqsave(&pool_lock, flags);
			list_add(&page_entry->list_head, &pool_head);
			spin_unlock_irqrestore(&pool_lock, flags);

			pr_debug("leaving invalidate_page\n");

			current_pages--;

			return;
		}
	}
	spin_unlock_irqrestore(&used_lock, flags);
	pr_err("leaving invalidate_page - ILLEGAL ARGUMENT\n");

	return;

}
EXPORT_SYMBOL(pagelist_tmem_invalidate_page);


static void pagelist_tmem_invalidate_area(void)
{
	struct page_list *page_entry;
	unsigned long flags;
	int bkt;

	pr_debug("entering invalidate_area\n");

	spin_lock_irqsave(&used_lock, flags);
	hash_for_each(used_pages, bkt, page_entry, hash_node) {
		hash_del(&page_entry->hash_node);
		spin_unlock_irqrestore(&used_lock, flags);

		if (page_entry->state != IN_USE)
			pr_err("invalidate area: page in hashtable in state %x\n", page_entry->state);
		page_entry->state = RECLAIMED;

		spin_lock_irqsave(&pool_lock, flags);
		list_add(&page_entry->list_head, &pool_head);
		spin_unlock_irqrestore(&pool_lock, flags);

		spin_lock_irqsave(&used_lock, flags);
	}
	spin_unlock_irqrestore(&used_lock, flags);
	pr_debug("leaving invalidate_area\n");
}
EXPORT_SYMBOL(pagelist_tmem_invalidate_area);

/* TODO: Pool creation/destruction - right now only one is used */
/* TODO: Use the addressing scheme described in the standard */
static int tmem_frontswap_store(unsigned int type, pgoff_t offset,
				struct page *page)
{
	return pagelist_tmem_put_page(offset, page);
}

static int tmem_frontswap_load(unsigned int type, pgoff_t offset,
				struct page *page)
{
	return pagelist_tmem_get_page(offset, page);
}

static void tmem_frontswap_invalidate_page(unsigned int type, pgoff_t offset)
{
	pagelist_tmem_invalidate_page(offset);
}

static void tmem_frontswap_invalidate_area(unsigned int type)
{
	pagelist_tmem_invalidate_area();
}

static void tmem_frontswap_init(unsigned int ignored)
{
}

static struct frontswap_ops tmem_frontswap_ops = {
	.store = tmem_frontswap_store,
	.load = tmem_frontswap_load,
	.invalidate_page = tmem_frontswap_invalidate_page,
	.invalidate_area = tmem_frontswap_invalidate_area,
	.init = tmem_frontswap_init,
};

static inline void teardown_structs(void)
{
	struct page_list *page_entry;
	unsigned long flags;

	spin_lock_irqsave(&pool_lock, flags);

	while (!list_empty(&pool_head)) {
		page_entry = list_first_entry(&pool_head, struct page_list, list_head);
		list_del(&page_entry->list_head);
		free_page(page_entry->addr);
		kfree(page_entry);
	}

	spin_unlock_irqrestore(&pool_lock, flags);
}

static int __init pagelist_tmem_init(void)
{
	struct page_list *page_entry;
	int err;
	int i;
	unsigned long flags;
	struct dentry *root;

	hash_init(used_pages);

	for (i = 0; i < TMEM_POOL_SIZE; i++) {

		page_entry = kmalloc(sizeof(*page_entry), GFP_KERNEL);
		if (!page_entry)
			goto out;


		page_entry->addr = __get_free_page(GFP_KERNEL);
		page_entry->state = UNUSED;
		if (!page_entry->addr) {
			kfree(page_entry);
			goto out;
		}

		spin_lock_irqsave(&pool_lock, flags);
		list_add(&page_entry->list_head, &pool_head);
		spin_unlock_irqrestore(&pool_lock, flags);
	}

	frontswap_writethrough(false);
	frontswap_register_ops(&tmem_frontswap_ops);
	pr_debug("registration successful");

	root = debugfs_create_dir("pagelist", NULL);
	if (root == NULL) {
		pr_err("debugfs entry could not be set up\n");
		return 0;
	}

	debugfs_create_u64("current_pages", S_IRUGO, root, &current_pages);


	return 0;

out:
	pr_err("not enough memory\n");
	pr_err("frontswap ops_could not be registered\n");
	err = -ENOMEM;

	teardown_structs();

	return err;
}



module_init(pagelist_tmem_init);
MODULE_AUTHOR("Aimilios Tsalapatis");
MODULE_LICENSE("GPL");
