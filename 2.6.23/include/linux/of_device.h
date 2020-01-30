#ifndef _LINUX_OF_DEVICE_H
#define _LINUX_OF_DEVICE_H
#ifdef __KERNEL__

#include <linux/device.h>
#include <linux/of.h>
#include <linux/mod_devicetable.h>

#include <asm/of_device.h>

#define	to_of_device(d) container_of(d, struct of_device, dev)

extern const struct of_device_id *of_match_node(
	const struct of_device_id *matches, const struct device_node *node);
extern const struct of_device_id *of_match_device(
	const struct of_device_id *matches, const struct of_device *dev);

extern struct of_device *of_dev_get(struct of_device *dev);
extern void of_dev_put(struct of_device *dev);

extern int of_device_register(struct of_device *ofdev);
extern void of_device_unregister(struct of_device *ofdev);
extern void of_release_dev(struct device *dev);

#endif /* __KERNEL__ */
#endif /* _LINUX_OF_DEVICE_H */
