#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/miscdevice.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>

#include "tmem.h"


DEFINE_SEMAPHORE(lock);

int tmem_chrdev_open(struct inode *inode, struct file *filp)
{
	struct page *page;

	if (down_trylock(&lock))
		return -EBUSY;


	page = alloc_page(GFP_KERNEL);
	if (!page) {
		up(&lock);
		return -ENOMEM;
	}
	filp->private_data = page;

	return 0;
}

int tmem_chrdev_release(struct inode *inode, struct file *filp)
{
	__free_page((struct page *) filp->private_data);
	up(&lock);

	return 0;
}


long tmem_chrdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void *page = filp->private_data;

	switch (cmd) {
	case TMEM_GET:
		pr_err("got into get\n");
		if (pagelist_tmem_get_page((pgoff_t) arg, page) < 0) {
			pr_err("TMEM_GET command failed");
			return -EINVAL;
		}

		if (copy_to_user((void *) arg, (void *) page_address(page), PAGE_SIZE)) {
			pr_err("copying to user failed");
			return -EINVAL;
		}

		break;

	case TMEM_PUT:
		pr_err("got into put\n");

		if (copy_from_user((void *) page_address(page), (void *) arg, PAGE_SIZE)) {
			pr_err("copying to user failed");
			return -EINVAL;
		}

		if (pagelist_tmem_put_page((pgoff_t) arg, page) < 0) {
			pr_err("TMEM_PUT command failed");
			return -EINVAL;
		}

		break;

	case TMEM_INVAL:
		pr_err("Got into invalidate\n");
		pagelist_tmem_invalidate_page((pgoff_t) arg);

		break;


	default:

		pr_err("illegal argument");
		return -EINVAL;
	}

	return 0;
}

struct tmem_dev_ops tmem_dev_ops = {
	.store = &pagelist_tmem_put_page,
	.load = &pagelist_tmem_get_page,
	.invalidate_page = &pagelist_tmem_invalidate_page,
	.invalidate_area = &pagelist_tmem_invalidate_area,
};


const struct file_operations tmem_fops = {
	.owner = THIS_MODULE,
	.open = tmem_chrdev_open,
	.release = tmem_chrdev_release,
	.unlocked_ioctl = tmem_chrdev_ioctl,
};


struct miscdevice tmem_chrdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "tmem_dev",
	.fops = &tmem_fops,
};

static int __init init_func(void)
{
	int ret = 0;

	pr_err("IOCTL Numbers for get, put, invalidate: %ld %ld %ld\n",
		TMEM_GET, TMEM_PUT, TMEM_INVAL);

	ret = misc_register(&tmem_chrdev);
	if (ret)
		pr_err("Device registration failed\n");

	return ret;
}


static void __exit exit_func(void)
{
	misc_deregister(&tmem_chrdev);
}


module_init(init_func);
module_exit(exit_func);

MODULE_LICENSE("GPL");

