#ifndef _TMEM_H
#define _TMEM_H

#include <uapi/asm-generic/ioctl.h>

#define TMEM_MAGIC ('*')

#define TMEM_GET (_IOW(TMEM_MAGIC, 1, long))
#define TMEM_PUT (_IOR(TMEM_MAGIC, 2, long))
#define TMEM_INVAL (_IO(TMEM_MAGIC, 3))

extern int pagelist_tmem_put_page(pgoff_t, struct page *);
extern int pagelist_tmem_get_page(pgoff_t, struct page *);
extern void pagelist_tmem_invalidate_page(pgoff_t);
extern void pagelist_tmem_invalidate_area(void);

struct tmem_dev_ops {
	int (*store)(pgoff_t, struct page *);
	int (*load)(pgoff_t, struct page *);
	void (*invalidate_page)(pgoff_t);
	void (*invalidate_area)(void);
};

#endif /* _TMEM_H */
