/*
 *  Universal power supply monitor class
 *
 *  Copyright © 2007  Anton Vorontsov <cbou@mail.ru>
 *  Copyright © 2004  Szabolcs Gyurko
 *  Copyright © 2003  Ian Molton <spyro@f2s.com>
 *
 *  Modified: 2004, Oct     Szabolcs Gyurko
 *
 *  You may use this code as per GPL version 2
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/power_supply.h>
#include "power_supply.h"

struct class *power_supply_class;

static void power_supply_changed_work(struct work_struct *work)
{
	struct power_supply *psy = container_of(work, struct power_supply,
						changed_work);
	int i;

	dev_dbg(psy->dev, "%s\n", __FUNCTION__);

	for (i = 0; i < psy->num_supplicants; i++) {
		struct device *dev;

		down(&power_supply_class->sem);
		list_for_each_entry(dev, &power_supply_class->devices, node) {
			struct power_supply *pst = dev_get_drvdata(dev);

			if (!strcmp(psy->supplied_to[i], pst->name)) {
				if (pst->external_power_changed)
					pst->external_power_changed(pst);
			}
		}
		up(&power_supply_class->sem);
	}

	power_supply_update_leds(psy);

	kobject_uevent(&psy->dev->kobj, KOBJ_CHANGE);
}

void power_supply_changed(struct power_supply *psy)
{
	dev_dbg(psy->dev, "%s\n", __FUNCTION__);

	schedule_work(&psy->changed_work);
}

int power_supply_am_i_supplied(struct power_supply *psy)
{
	union power_supply_propval ret = {0,};
	struct device *dev;

	down(&power_supply_class->sem);
	list_for_each_entry(dev, &power_supply_class->devices, node) {
		struct power_supply *epsy = dev_get_drvdata(dev);
		int i;

		for (i = 0; i < epsy->num_supplicants; i++) {
			if (!strcmp(epsy->supplied_to[i], psy->name)) {
				if (epsy->get_property(epsy,
					  POWER_SUPPLY_PROP_ONLINE, &ret))
					continue;
				if (ret.intval)
					goto out;
			}
		}
	}
out:
	up(&power_supply_class->sem);

	dev_dbg(psy->dev, "%s %d\n", __FUNCTION__, ret.intval);

	return ret.intval;
}

int power_supply_register(struct device *parent, struct power_supply *psy)
{
	int rc = 0;

	psy->dev = device_create(power_supply_class, parent, 0,
				 "%s", psy->name);
	if (IS_ERR(psy->dev)) {
		rc = PTR_ERR(psy->dev);
		goto dev_create_failed;
	}

	dev_set_drvdata(psy->dev, psy);

	INIT_WORK(&psy->changed_work, power_supply_changed_work);

	rc = power_supply_create_attrs(psy);
	if (rc)
		goto create_attrs_failed;

	rc = power_supply_create_triggers(psy);
	if (rc)
		goto create_triggers_failed;

	power_supply_changed(psy);

	goto success;

create_triggers_failed:
	power_supply_remove_attrs(psy);
create_attrs_failed:
	device_unregister(psy->dev);
dev_create_failed:
success:
	return rc;
}

void power_supply_unregister(struct power_supply *psy)
{
	flush_scheduled_work();
	power_supply_remove_triggers(psy);
	power_supply_remove_attrs(psy);
	device_unregister(psy->dev);
}

static int __init power_supply_class_init(void)
{
	power_supply_class = class_create(THIS_MODULE, "power_supply");

	if (IS_ERR(power_supply_class))
		return PTR_ERR(power_supply_class);

	power_supply_class->dev_uevent = power_supply_uevent;

	return 0;
}

static void __exit power_supply_class_exit(void)
{
	class_destroy(power_supply_class);
}

EXPORT_SYMBOL_GPL(power_supply_changed);
EXPORT_SYMBOL_GPL(power_supply_am_i_supplied);
EXPORT_SYMBOL_GPL(power_supply_register);
EXPORT_SYMBOL_GPL(power_supply_unregister);

/* exported for the APM Power driver, APM emulation */
EXPORT_SYMBOL_GPL(power_supply_class);

subsys_initcall(power_supply_class_init);
module_exit(power_supply_class_exit);

MODULE_DESCRIPTION("Universal power supply monitor class");
MODULE_AUTHOR("Ian Molton <spyro@f2s.com>, "
	      "Szabolcs Gyurko, "
	      "Anton Vorontsov <cbou@mail.ru>");
MODULE_LICENSE("GPL");
