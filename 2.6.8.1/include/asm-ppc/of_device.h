#ifndef __OF_DEVICE_H__
#define __OF_DEVICE_H__

#include <linux/device.h>
#include <asm/prom.h>

/*
 * The of_platform_bus_type is a bus type used by drivers that do not
 * attach to a macio or similar bus but still use OF probing
 * mecanism
 */
extern struct bus_type of_platform_bus_type;

/*
 * The of_device is a kind of "base class" that is a superset of
 * struct device for use by devices attached to an OF node and
 * probed using OF properties
 */
struct of_device
{
	struct device_node	*node;		/* OF device node */
	u64			dma_mask;	/* DMA mask */
	struct device		dev;		/* Generic device interface */
};
#define	to_of_device(d) container_of(d, struct of_device, dev)

/*
 * Struct used for matching a device
 */
struct of_match
{
	char	*name;
	char	*type;
	char	*compatible;
	void	*data;
};
#define OF_ANY_MATCH		((char *)-1L)

extern const struct of_match *of_match_device(
	const struct of_match *matches, const struct of_device *dev);

extern struct of_device *of_dev_get(struct of_device *dev);
extern void of_dev_put(struct of_device *dev);

/*
 * An of_platform_driver driver is attached to a basic of_device on
 * the "platform bus" (of_platform_bus_type)
 */
struct of_platform_driver
{
	char			*name;
	struct of_match		*match_table;
	struct module		*owner;

	int	(*probe)(struct of_device* dev, const struct of_match *match);
	int	(*remove)(struct of_device* dev);

	int	(*suspend)(struct of_device* dev, u32 state);
	int	(*resume)(struct of_device* dev);
	int	(*shutdown)(struct of_device* dev);

	struct device_driver	driver;
};
#define	to_of_platform_driver(drv) container_of(drv,struct of_platform_driver, driver)

extern int of_register_driver(struct of_platform_driver *drv);
extern void of_unregister_driver(struct of_platform_driver *drv);
extern int of_device_register(struct of_device *ofdev);
extern void of_device_unregister(struct of_device *ofdev);
extern struct of_device *of_platform_device_create(struct device_node *np, const char *bus_id);
extern void of_release_dev(struct device *dev);

#endif /* __OF_DEVICE_H__ */

