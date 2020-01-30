#ifndef _M68KNOMMU_BUG_H
#define _M68KNOMMU_BUG_H

#define BUG() do { \
  printk("%s(%d): kernel BUG!\n", __FILE__, __LINE__); \
} while (0)

#define BUG_ON(condition) do { \
	if (unlikely((condition)!=0)) \
		BUG(); \
} while(0)

#define PAGE_BUG(page) do { \
         BUG(); \
} while (0)

#define WARN_ON(condition) do { \
	if (unlikely((condition)!=0)) { \
		printk("Badness in %s at %s:%d\n", __FUNCTION__, __FILE__, __LINE__); \
		dump_stack(); \
	} \
} while (0)

#endif
