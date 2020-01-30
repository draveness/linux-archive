#ifndef _LINUX_FIRMWARE_H
#define _LINUX_FIRMWARE_H
#include <linux/module.h>
#include <linux/types.h>
#define FIRMWARE_NAME_MAX 30 
#define FW_ACTION_NOHOTPLUG 0
#define FW_ACTION_HOTPLUG 1

struct firmware {
	size_t size;
	u8 *data;
};
struct device;
int request_firmware(const struct firmware **fw, const char *name,
		     struct device *device);
int request_firmware_nowait(
	struct module *module, int uevent,
	const char *name, struct device *device, void *context,
	void (*cont)(const struct firmware *fw, void *context));

void release_firmware(const struct firmware *fw);
#endif
