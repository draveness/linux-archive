#include <linux/kernel.h>
#include <linux/stat.h>
#include <asm/macio.h>


#define macio_config_of_attr(field, format_string)			\
static ssize_t								\
field##_show (struct device *dev, struct device_attribute *attr,	\
              char *buf)						\
{									\
	struct macio_dev *mdev = to_macio_device (dev);			\
	return sprintf (buf, format_string, mdev->ofdev.node->field);	\
}

static ssize_t
compatible_show (struct device *dev, struct device_attribute *attr, char *buf)
{
	struct of_device *of;
	const char *compat;
	int cplen;
	int length = 0;

	of = &to_macio_device (dev)->ofdev;
	compat = of_get_property(of->node, "compatible", &cplen);
	if (!compat) {
		*buf = '\0';
		return 0;
	}
	while (cplen > 0) {
		int l;
		length += sprintf (buf, "%s\n", compat);
		buf += length;
		l = strlen (compat) + 1;
		compat += l;
		cplen -= l;
	}

	return length;
}

static ssize_t modalias_show (struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct of_device *ofdev = to_of_device(dev);
	int len;

	len = of_device_get_modalias(ofdev, buf, PAGE_SIZE);

	buf[len] = '\n';
	buf[len+1] = 0;

	return len+1;
}

macio_config_of_attr (name, "%s\n");
macio_config_of_attr (type, "%s\n");

struct device_attribute macio_dev_attrs[] = {
	__ATTR_RO(name),
	__ATTR_RO(type),
	__ATTR_RO(compatible),
	__ATTR_RO(modalias),
	__ATTR_NULL
};
