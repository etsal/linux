#include <linux/fs.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>


int tmem_chrdev_open(struct inode *inode, struct file *filp)
{
	return 0;
}


int tmem_chrdev_release(struct inode *inode, struct file *filp)
{
	return 0;
}


long tmem_chrdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return -ENOTTY;
}


int tmem_chrdev_init(void)
{
	return 0;
}


void tmem_chrdev_destroy(void)
{
}


const struct file_operations tmem_fops = {
	.owner = THIS_MODULE,
	.open = tmem_chrdev_open,
	.release = tmem_chrdev_release,
	.unlocked_ioctl = tmem_chrdev_ioctl,
};
