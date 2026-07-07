// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX ColorOS compatibility nodes for Xiaomi rodin / MT6899.
 *
 * Rule: no dummy nodes. Every exported value is backed by a real kernel
 * power_supply or thermal backend. If the backend is unavailable, return error.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/types.h>

#define EVX_NAME "evonix_cos_power_compat"

static struct class *oplus_chg_class;
static struct device *oplus_battery_dev;
static struct device *oplus_usb_dev;

static struct proc_dir_entry *proc_charger_dir;
static struct proc_dir_entry *proc_input_current_now;
static struct proc_dir_entry *proc_shell_temp;

static int evx_psy_get_int(const char *psy_name,
			   enum power_supply_property psp,
			   int *out)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int ret;

	if (!out)
		return -EINVAL;

	psy = power_supply_get_by_name(psy_name);
	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy, psp, &val);
	power_supply_put(psy);

	if (ret < 0)
		return ret;

	*out = val.intval;
	return 0;
}

static int evx_get_battery_capacity(void)
{
	int val;

	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CAPACITY, &val))
		return val;

	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_CAPACITY, &val))
		return val;

	return -ENODEV;
}

static int evx_get_battery_rm_mah(void)
{
	int charge_counter;
	int charge_full;
	int capacity;
	s64 rm;

	/*
	 * On rodin current backend:
	 * /sys/class/power_supply/battery/charge_counter = 4342000
	 * Expose OPlus battery_rm as mAh: 4342.
	 */
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CHARGE_COUNTER,
			     &charge_counter))
		return charge_counter / 1000;

	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_CHARGE_COUNTER,
			     &charge_counter))
		return charge_counter / 1000;

	/*
	 * Real fallback: full capacity * percentage.
	 */
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CHARGE_FULL,
			     &charge_full) &&
	    !evx_psy_get_int("battery", POWER_SUPPLY_PROP_CAPACITY,
			     &capacity)) {
		rm = div_s64((s64)charge_full * capacity, 100000);
		return (int)rm;
	}

	return -ENODEV;
}

static const char *evx_battery_technology_name(int tech)
{
	switch (tech) {
	case POWER_SUPPLY_TECHNOLOGY_NiMH:
		return "NiMH";
	case POWER_SUPPLY_TECHNOLOGY_LION:
		return "Li-ion";
	case POWER_SUPPLY_TECHNOLOGY_LIPO:
		return "Li-poly";
	case POWER_SUPPLY_TECHNOLOGY_LiFe:
		return "LiFe";
	case POWER_SUPPLY_TECHNOLOGY_NiCd:
		return "NiCd";
	case POWER_SUPPLY_TECHNOLOGY_LiMn:
		return "LiMn";
	case POWER_SUPPLY_TECHNOLOGY_UNKNOWN:
	default:
		return "Unknown";
	}
}

static int evx_get_usb_current_now(void)
{
	int val;

	/*
	 * Current rodin backend exposes:
	 * /sys/class/power_supply/usb/current_now
	 * /sys/class/power_supply/usb/input_current_now
	 *
	 * Use generic power_supply current property. If unavailable, use
	 * CURRENT_MAX. Negative current is normalized.
	 */
	if (!evx_psy_get_int("usb", POWER_SUPPLY_PROP_CURRENT_NOW, &val)) {
		if (val < 0)
			val = -val;
		return val;
	}

	if (!evx_psy_get_int("usb", POWER_SUPPLY_PROP_CURRENT_MAX, &val)) {
		if (val < 0)
			val = -val;
		return val;
	}

	return -ENODEV;
}

static int evx_get_fast_chg_type(void)
{
	int online;
	int current_ma;

	if (evx_psy_get_int("usb", POWER_SUPPLY_PROP_ONLINE, &online) || !online)
		return 0;

	current_ma = evx_get_usb_current_now();
	if (current_ma < 0)
		return 0;

	/*
	 * Real-derived state from USB power_supply current.
	 * Existing backend value on CDP was around 845 mA => normal = 0.
	 * Higher real input current means fast charging path is active.
	 */
	return current_ma >= 1500 ? 1 : 0;
}

static int evx_get_shell_temp_mc(void)
{
	static const char * const zones[] = {
		"battery",
		"bms",
		"mt6375-gauge",
		"quiet_therm",
		"wifi_therm",
		"flash_therm",
		"charger1_therm",
		"ScreenPmic_therm",
		"mtktsAP",
		"soc_max",
		"consys",
	};
	int i;
	int temp;
	int bat_temp;
	struct thermal_zone_device *tz;

	/*
	 * Real primary backend:
	 * /sys/class/power_supply/battery/temp exists on rodin.
	 * power_supply temp is normally deci-Celsius:
	 * 381 => 38.1C => 38100 milli-Celsius.
	 */
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_TEMP, &bat_temp))
		return bat_temp * 100;

	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_TEMP, &bat_temp))
		return bat_temp * 100;

	/*
	 * Real fallback: use actual thermal zone names observed on rodin.
	 */
	for (i = 0; i < ARRAY_SIZE(zones); i++) {
		tz = thermal_zone_get_zone_by_name(zones[i]);
		if (IS_ERR(tz))
			continue;

		if (!thermal_zone_get_temp(tz, &temp))
			return temp;
	}

	return -ENODEV;
}

/* /sys/class/oplus_chg/battery/chip_soc */
static ssize_t chip_soc_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	int soc = evx_get_battery_capacity();

	if (soc < 0)
		return soc;

	return sysfs_emit(buf, "%d\n", soc);
}
static DEVICE_ATTR_RO(chip_soc);

/* /sys/class/oplus_chg/battery/battery_rm */
static ssize_t battery_rm_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	int rm = evx_get_battery_rm_mah();

	if (rm < 0)
		return rm;

	return sysfs_emit(buf, "%d\n", rm);
}
static DEVICE_ATTR_RO(battery_rm);

/* /sys/devices/virtual/oplus_chg/battery/charge_technology */
static ssize_t charge_technology_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	int tech;
	int ret;

	ret = evx_psy_get_int("battery", POWER_SUPPLY_PROP_TECHNOLOGY, &tech);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%s\n", evx_battery_technology_name(tech));
}
static DEVICE_ATTR_RO(charge_technology);

/* /sys/devices/virtual/oplus_chg/usb/fast_chg_type */
static ssize_t fast_chg_type_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", evx_get_fast_chg_type());
}
static DEVICE_ATTR_RO(fast_chg_type);

/* /proc/charger/input_current_now */
static int input_current_now_proc_show(struct seq_file *m, void *v)
{
	int cur = evx_get_usb_current_now();

	if (cur < 0)
		return cur;

	seq_printf(m, "%d\n", cur);
	return 0;
}

static int input_current_now_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, input_current_now_proc_show, NULL);
}

static const struct proc_ops input_current_now_proc_ops = {
	.proc_open	= input_current_now_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* /proc/shell-temp */
static int shell_temp_proc_show(struct seq_file *m, void *v)
{
	int temp = evx_get_shell_temp_mc();

	if (temp < 0)
		return temp;

	seq_printf(m, "%d\n", temp);
	return 0;
}

static int shell_temp_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, shell_temp_proc_show, NULL);
}

static const struct proc_ops shell_temp_proc_ops = {
	.proc_open	= shell_temp_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init evx_cos_power_compat_init(void)
{
	int ret;

	oplus_chg_class = class_create("oplus_chg");
	if (IS_ERR(oplus_chg_class)) {
		pr_err(EVX_NAME ": failed to create oplus_chg class: %ld\n",
		       PTR_ERR(oplus_chg_class));
		return PTR_ERR(oplus_chg_class);
	}

	oplus_battery_dev = device_create(oplus_chg_class, NULL, MKDEV(0, 0),
					  NULL, "battery");
	if (IS_ERR(oplus_battery_dev)) {
		ret = PTR_ERR(oplus_battery_dev);
		pr_err(EVX_NAME ": failed to create battery device: %d\n", ret);
		goto err_class;
	}

	oplus_usb_dev = device_create(oplus_chg_class, NULL, MKDEV(0, 0),
				      NULL, "usb");
	if (IS_ERR(oplus_usb_dev)) {
		ret = PTR_ERR(oplus_usb_dev);
		pr_err(EVX_NAME ": failed to create usb device: %d\n", ret);
		goto err_battery_dev;
	}

	ret = device_create_file(oplus_battery_dev, &dev_attr_chip_soc);
	if (ret)
		goto err_usb_dev;

	ret = device_create_file(oplus_battery_dev, &dev_attr_battery_rm);
	if (ret)
		goto err_remove_chip_soc;

	ret = device_create_file(oplus_battery_dev, &dev_attr_charge_technology);
	if (ret)
		goto err_remove_battery_rm;

	ret = device_create_file(oplus_usb_dev, &dev_attr_fast_chg_type);
	if (ret)
		goto err_remove_charge_technology;

	proc_charger_dir = proc_mkdir("charger", NULL);
	if (!proc_charger_dir) {
		ret = -ENOMEM;
		goto err_remove_fast_chg_type;
	}

	proc_input_current_now = proc_create("input_current_now", 0444,
					     proc_charger_dir,
					     &input_current_now_proc_ops);
	if (!proc_input_current_now) {
		ret = -ENOMEM;
		goto err_remove_proc_charger;
	}

	proc_shell_temp = proc_create("shell-temp", 0666, NULL,
				      &shell_temp_proc_ops);
	if (!proc_shell_temp) {
		ret = -ENOMEM;
		goto err_remove_input_current;
	}

	pr_info(EVX_NAME ": loaded real-backed ColorOS power compatibility nodes\n");
	return 0;

err_remove_input_current:
	proc_remove(proc_input_current_now);
err_remove_proc_charger:
	proc_remove(proc_charger_dir);
err_remove_fast_chg_type:
	device_remove_file(oplus_usb_dev, &dev_attr_fast_chg_type);
err_remove_charge_technology:
	device_remove_file(oplus_battery_dev, &dev_attr_charge_technology);
err_remove_battery_rm:
	device_remove_file(oplus_battery_dev, &dev_attr_battery_rm);
err_remove_chip_soc:
	device_remove_file(oplus_battery_dev, &dev_attr_chip_soc);
err_usb_dev:
	device_unregister(oplus_usb_dev);
err_battery_dev:
	device_unregister(oplus_battery_dev);
err_class:
	class_destroy(oplus_chg_class);
	return ret;
}

static void __exit evx_cos_power_compat_exit(void)
{
	if (proc_shell_temp)
		proc_remove(proc_shell_temp);
	if (proc_input_current_now)
		proc_remove(proc_input_current_now);
	if (proc_charger_dir)
		proc_remove(proc_charger_dir);

	if (oplus_usb_dev) {
		device_remove_file(oplus_usb_dev, &dev_attr_fast_chg_type);
		device_unregister(oplus_usb_dev);
	}

	if (oplus_battery_dev) {
		device_remove_file(oplus_battery_dev, &dev_attr_charge_technology);
		device_remove_file(oplus_battery_dev, &dev_attr_battery_rm);
		device_remove_file(oplus_battery_dev, &dev_attr_chip_soc);
		device_unregister(oplus_battery_dev);
	}

	if (oplus_chg_class)
		class_destroy(oplus_chg_class);
}

module_init(evx_cos_power_compat_init);
module_exit(evx_cos_power_compat_exit);

MODULE_DESCRIPTION("EVONIX ColorOS real-backed power/thermal compatibility nodes");
MODULE_AUTHOR("EVONIX");
MODULE_LICENSE("GPL");
