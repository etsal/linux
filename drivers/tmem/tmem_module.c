#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include "tmem_chrdev.h"

static int __init tmem_init(void)
{
	if (tmem_chrdev_init()) {
		pr_info("tmem: device creation failed");
		return -1;
	}

	pr_info("tmem: character device created");
	return 0;
}


static void __exit tmem_exit(void)
{
	pr_info("tmem: character device destroyed");
	tmem_chrdev_destroy();
}


module_init(tmem_init);
module_exit(tmem_exit);

MODULE_DESCRIPTION("Native tmem backend implementation");
MODULE_LICENSE("GPL");
