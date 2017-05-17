#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/frontswap.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/hashtable.h>

struct page_list {
	struct list_head list_head;
	struct hlist_node hash_node;
	unsigned long addr;
};

LIST_HEAD(pool_head);
DEFINE_HASHTABLE(used_pages, 10);

#define TMEM_POOL_ID (0)
#define TMEM_OBJ_ID (0)
#define TMEM_POOL_SIZE (64 * 1024)

/* generic tmem ops */

static int pagelist_tmem_put_page(u64 index, struct page *page)
{
	struct page_list *page_entry;

	if (list_empty(&pool_head))
		return -ENOMEM;

	page_entry = list_first_entry(&pool_head, struct page_list, list_head);
	list_del(&page_entry->list_head);

	memcpy(page_address(page), (void *) page_entry->addr, sizeof(struct page));

	hash_add(used_pages, &page_entry->hash_node, index);

	return 0;
}

static int pagelist_tmem_get_page(u64 index, struct page *page)
{
	struct page_list *page_entry;
	int ret = -EINVAL;

	hash_for_each_possible(used_pages, page_entry, hash_node, index) {
		if (page_entry->addr == index) {

			hash_del(&page_entry->hash_node);
			memcpy((void *) page_entry->addr, page_address(page), sizeof(struct page));
			list_add(&page_entry->list_head, &pool_head);

			ret = 0;
			break;
		}
	}


	return ret;
}

static void pagelist_tmem_invalidate_page(u64 index)
{
	struct page_list *page_entry;

	hash_for_each_possible(used_pages, page_entry, hash_node, index) {
		if (page_entry->addr == index) {
			hash_del(&page_entry->hash_node);
			list_add(&page_entry->list_head, &pool_head);
			break;
		}
	}

}


static void pagelist_tmem_invalidate_area(void)
{
}

/* TODO: Pool creation/destruction - right now only one is used */
/* TODO: Use the addressing scheme described in the standard */
static int tmem_frontswap_store(unsigned type, pgoff_t offset,
				struct page *page)
{
	return pagelist_tmem_put_page((u64)offset, page);
}

static int tmem_frontswap_load(unsigned type, pgoff_t offset,
				struct page *page)
{
	return pagelist_tmem_get_page((u64)offset, page);
}

static void tmem_frontswap_invalidate_page(unsigned type, pgoff_t offset)
{
	pagelist_tmem_invalidate_page((u64)offset);
}

static void tmem_frontswap_invalidate_area(unsigned type)
{
	pagelist_tmem_invalidate_area();
}

static void tmem_frontswap_init(unsigned ignored)
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

	while (!list_empty(&pool_head)) {
		page_entry = list_first_entry(&pool_head, struct page_list, list_head);
		list_del(&page_entry->list_head);
		free_page(page_entry->addr);
		kfree(page_entry);
	}

}

static int __init pagelist_tmem_init(void)
{
	struct page_list *page_entry;
	int err;
	int i;

	hash_init(used_pages);

	for (i = 0; i < TMEM_POOL_SIZE; i++) {

		page_entry = kmalloc(sizeof(struct page_list), GFP_KERNEL);
		if (!page_entry)
			goto out;

		page_entry->addr = __get_free_page(GFP_KERNEL);
		if (!page_entry->addr) {
			kfree(page_entry);
			goto out;
		}

		list_add(&page_entry->list_head, &pool_head);
	}

	frontswap_register_ops(&tmem_frontswap_ops);

	return 0;

out:
	pr_err("tmem: not enough memory");
	pr_err("tmem: frontswap ops_could not be registered");
	err = -ENOMEM;

	teardown_structs();

	return err;
}


static void __exit pagelist_tmem_exit(void)
{
	teardown_structs();
}

module_init(pagelist_tmem_init);
module_exit(pagelist_tmem_exit);
MODULE_AUTHOR("Aimilios Tsalapatis");
MODULE_LICENSE("GPL");
