#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include "tmem_chrdev.h"

static int __init tmem_init(void)
{
	if (tmem_chrdev_init()) {
		goto init_out;
	}

	pr_info("character device created\n");
	return 0;

init_out:
	pr_debug("device creation failed\n");
	return -1;
}


static void __exit tmem_exit(void)
{
	pr_info("character device destroyed\n");
	tmem_chrdev_destroy();
}


module_init(tmem_init);
module_exit(tmem_exit);

MODULE_DESCRIPTION("Native tmem backend implementation");
MODULE_LICENSE("GPL");
