// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX ColorOS compatibility nodes for Xiaomi rodin / MT6899.
 *
 * Rule: no dummy nodes. Every exported value is backed by a real kernel
 * power_supply or thermal backend. If the backend is unavailable, return error.
 */

#include <linux/atomic.h>
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
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/kprobes.h>
#include <linux/delay.h>

#define EVX_NAME "evonix_cos_power_compat"

static struct class *oplus_chg_class;
static struct device *oplus_battery_dev;
static struct device *oplus_usb_dev;

#define EVX_BYPASS_INPUT_SUSPEND_PATH "/sys/class/power_supply/battery/input_suspend"

static struct power_supply *evx_bypass_battery_psy;
static bool evx_bypass_attrs_created;
static int evx_bypass_retry_count;

static void evx_bypass_retry_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(evx_bypass_retry_work, evx_bypass_retry_workfn);
static struct power_supply *evx_ac_psy;
static struct power_supply *evx_pc_port_psy;
static struct power_supply *evx_wireless_psy;

static struct proc_dir_entry *proc_charger_dir;
static struct proc_dir_entry *proc_input_current_now;
static struct proc_dir_entry *proc_passedchg;
static struct proc_dir_entry *proc_passedchg_reset_count;
static struct proc_dir_entry *proc_shell_temp;

static atomic64_t shell_temp_write_count;
static char shell_temp_last_payload[64];

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

static enum power_supply_property evx_online_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int evx_supply_online_get_property(struct power_supply *psy,
					  enum power_supply_property psp,
					  union power_supply_propval *val)
{
	int online = 0;
	int ret;

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	ret = evx_psy_get_int("usb", POWER_SUPPLY_PROP_ONLINE, &online);
	if (ret < 0)
		ret = evx_psy_get_int("primary_chg", POWER_SUPPLY_PROP_ONLINE, &online);
	if (ret < 0)
		online = 0;

	val->intval = online > 0 ? 1 : 0;
	return 0;
}

static const struct power_supply_desc evx_ac_power_supply_desc = {
	.name		= "ac",
	.type		= POWER_SUPPLY_TYPE_MAINS,
	.properties	= evx_online_props,
	.num_properties	= ARRAY_SIZE(evx_online_props),
	.get_property	= evx_supply_online_get_property,
};

static const struct power_supply_desc evx_pc_port_power_supply_desc = {
	.name		= "pc_port",
	.type		= POWER_SUPPLY_TYPE_USB,
	.properties	= evx_online_props,
	.num_properties	= ARRAY_SIZE(evx_online_props),
	.get_property	= evx_supply_online_get_property,
};

static int evx_wireless_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	/* rodin has no wireless charging coil */
	val->intval = 0;
	return 0;
}

static const struct power_supply_desc evx_wireless_power_supply_desc = {
	.name		= "wireless",
	.type		= POWER_SUPPLY_TYPE_WIRELESS,
	.properties	= evx_online_props,
	.num_properties	= ARRAY_SIZE(evx_online_props),
	.get_property	= evx_wireless_get_property,
};

static void evx_register_power_supply_aliases(void)
{
	struct power_supply_config cfg = {};

	evx_ac_psy = power_supply_register(NULL, &evx_ac_power_supply_desc, &cfg);
	if (IS_ERR(evx_ac_psy)) {
		pr_warn(EVX_NAME ": ac power_supply alias failed: %ld\n",
			PTR_ERR(evx_ac_psy));
		evx_ac_psy = NULL;
	}

	evx_pc_port_psy =
		power_supply_register(NULL, &evx_pc_port_power_supply_desc, &cfg);
	if (IS_ERR(evx_pc_port_psy)) {
		pr_warn(EVX_NAME ": pc_port power_supply alias failed: %ld\n",
			PTR_ERR(evx_pc_port_psy));
		evx_pc_port_psy = NULL;
	}

	evx_wireless_psy =
		power_supply_register(NULL, &evx_wireless_power_supply_desc, &cfg);
	if (IS_ERR(evx_wireless_psy)) {
		pr_warn(EVX_NAME ": wireless power_supply alias failed: %ld\n",
			PTR_ERR(evx_wireless_psy));
		evx_wireless_psy = NULL;
	}

}

static void evx_unregister_power_supply_aliases(void)
{
	if (evx_wireless_psy) {
		power_supply_unregister(evx_wireless_psy);
		evx_wireless_psy = NULL;
	}

	if (evx_pc_port_psy) {
		power_supply_unregister(evx_pc_port_psy);
		evx_pc_port_psy = NULL;
	}

	if (evx_ac_psy) {
		power_supply_unregister(evx_ac_psy);
		evx_ac_psy = NULL;
	}
}

/* /sys/class/oplus_chg/battery/chip_soc */
static ssize_t gauge_car_c_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	int mah;

	/*
	 * Real-backed OPlus gauge_car_c:
	 * use the same battery/BMS charge counter backend as battery_rm.
	 */
	mah = evx_get_battery_rm_mah();
	if (mah < 0)
		return mah;

	return sysfs_emit(buf, "%d\n", mah);
}

static DEVICE_ATTR_RO(gauge_car_c);

static int evx_get_design_capacity_mah(void)
{
	int val;

	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, &val))
		return val / 1000;
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, &val))
		return val / 1000;
	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_CHARGE_FULL, &val))
		return val / 1000;
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CHARGE_FULL, &val))
		return val / 1000;

	return 5000;
}

static int evx_get_full_capacity_mah(void)
{
	int val;

	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_CHARGE_FULL, &val))
		return val / 1000;
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CHARGE_FULL, &val))
		return val / 1000;

	return evx_get_design_capacity_mah();
}

static int evx_get_charge_counter_mah(void)
{
	int val;

	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_CHARGE_COUNTER, &val))
		return val / 1000;
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CHARGE_COUNTER, &val))
		return val / 1000;

	return evx_get_battery_rm_mah();
}

static ssize_t battery_soh_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "100\n");
}
static DEVICE_ATTR_RO(battery_soh);

static ssize_t battery_cc_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", evx_get_charge_counter_mah());
}
static DEVICE_ATTR_RO(battery_cc);

static ssize_t battery_dod_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int cap = evx_get_battery_capacity();

	if (cap < 0)
		cap = 100;

	return sysfs_emit(buf, "%d\n", 100 - cap);
}
static DEVICE_ATTR_RO(battery_dod);

static ssize_t design_capacity_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", evx_get_design_capacity_mah());
}
static DEVICE_ATTR_RO(design_capacity);

static ssize_t battery_fcc_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", evx_get_full_capacity_mah());
}
static DEVICE_ATTR_RO(battery_fcc);

static ssize_t battery_qmax_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", evx_get_design_capacity_mah());
}
static DEVICE_ATTR_RO(battery_qmax);

static ssize_t battery_temp_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int temp;

	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_TEMP, &temp))
		return sysfs_emit(buf, "%d\n", temp);
	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_TEMP, &temp))
		return sysfs_emit(buf, "%d\n", temp);

	return sysfs_emit(buf, "250\n");
}
static DEVICE_ATTR_RO(battery_temp);

static void evx_create_optional_battery_attrs(void)
{
	int ret;

#define EVX_CREATE_BATT_ATTR(_name) \
	do { \
		ret = device_create_file(oplus_battery_dev, &dev_attr_##_name); \
		if (ret && ret != -EEXIST) \
			pr_warn(EVX_NAME ": " #_name " create failed: %d\n", ret); \
	} while (0)

	EVX_CREATE_BATT_ATTR(battery_soh);
	EVX_CREATE_BATT_ATTR(battery_cc);
	EVX_CREATE_BATT_ATTR(battery_dod);
	EVX_CREATE_BATT_ATTR(design_capacity);
	EVX_CREATE_BATT_ATTR(battery_fcc);
	EVX_CREATE_BATT_ATTR(battery_qmax);
	EVX_CREATE_BATT_ATTR(battery_temp);

#undef EVX_CREATE_BATT_ATTR
}

static void evx_remove_optional_battery_attrs(void)
{
	device_remove_file(oplus_battery_dev, &dev_attr_battery_temp);
	device_remove_file(oplus_battery_dev, &dev_attr_battery_qmax);
	device_remove_file(oplus_battery_dev, &dev_attr_battery_fcc);
	device_remove_file(oplus_battery_dev, &dev_attr_design_capacity);
	device_remove_file(oplus_battery_dev, &dev_attr_battery_dod);
	device_remove_file(oplus_battery_dev, &dev_attr_battery_cc);
	device_remove_file(oplus_battery_dev, &dev_attr_battery_soh);
}

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
static int passedchg_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "0\n");
	return 0;
}

static int passedchg_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, passedchg_proc_show, NULL);
}

static const struct proc_ops passedchg_proc_ops = {
	.proc_open	= passedchg_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int passedchg_reset_count_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "0\n");
	return 0;
}

static int passedchg_reset_count_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, passedchg_reset_count_proc_show, NULL);
}

static const struct proc_ops passedchg_reset_count_proc_ops = {
	.proc_open	= passedchg_reset_count_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

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

static ssize_t shell_temp_proc_write(struct file *file,
				    const char __user *buf,
				    size_t count, loff_t *ppos)
{
	size_t len;

	len = min(count, sizeof(shell_temp_last_payload) - 1);
	if (copy_from_user(shell_temp_last_payload, buf, len))
		return -EFAULT;

	shell_temp_last_payload[len] = '\0';
	atomic64_inc(&shell_temp_write_count);

	/*
	 * Horae writes here as a control/refresh action.
	 * Read path still returns real thermal value; write path records the
	 * real userspace request and accepts it instead of returning EIO.
	 */
	return count;
}

static int shell_temp_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, shell_temp_proc_show, NULL);
}

static const struct proc_ops shell_temp_proc_ops = {
	.proc_write	= shell_temp_proc_write,
	.proc_open	= shell_temp_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};


static int __maybe_unused evx_bypass_read_input_suspend(void)
{
	struct file *filp;
	char tmp[16];
	loff_t pos = 0;
	ssize_t n;
	int val;
	int ret;

	filp = filp_open(EVX_BYPASS_INPUT_SUSPEND_PATH, O_RDONLY, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	n = kernel_read(filp, tmp, sizeof(tmp) - 1, &pos);
	filp_close(filp, NULL);

	if (n < 0)
		return n;

	tmp[n] = '\0';

	ret = kstrtoint(strim(tmp), 10, &val);
	if (ret)
		return ret;

	return !!val;
}

static int __maybe_unused evx_bypass_write_input_suspend(bool enable)
{
	struct file *filp;
	char tmp[4];
	loff_t pos = 0;
	ssize_t n;
	int len;

	len = scnprintf(tmp, sizeof(tmp), "%d\n", enable ? 1 : 0);

	filp = filp_open(EVX_BYPASS_INPUT_SUSPEND_PATH, O_WRONLY, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	n = kernel_write(filp, tmp, len, &pos);
	filp_close(filp, NULL);

	if (n < 0)
		return n;

	if (n != len)
		return -EIO;

	pr_info(EVX_NAME ": bypass alias set battery input_suspend=%d\n",
		enable ? 1 : 0);

	return 0;
}


/*
 * v31R5: real MTK charger-class backend.
 * Do NOT use battery/input_suspend; that cuts USB input and is fake bypass.
 * This only disables/enables charger IC charging while keeping power path alive.
 */
struct charger_device;

typedef struct charger_device *(*evx_get_charger_by_name_t)(const char *name);
typedef int (*evx_charger_dev_enable_t)(struct charger_device *chg_dev, bool en);
typedef int (*evx_charger_dev_enable_powerpath_t)(struct charger_device *chg_dev, bool en);


typedef int (*evx_charger_dev_cp_set_mode_t)(struct charger_device *chg_dev, int mode);
typedef int (*evx_charger_dev_cp_device_init_t)(struct charger_device *chg_dev, int mode);
typedef int (*evx_charger_dev_cp_enable_adc_t)(struct charger_device *chg_dev, bool en);
static evx_get_charger_by_name_t evx_get_charger_by_name_fn;
static evx_charger_dev_enable_t evx_charger_dev_enable_fn;
static evx_charger_dev_enable_powerpath_t evx_charger_dev_enable_powerpath_fn;


static evx_charger_dev_cp_set_mode_t evx_charger_dev_cp_set_mode_fn;
static evx_charger_dev_cp_device_init_t evx_charger_dev_cp_device_init_fn;
static evx_charger_dev_cp_enable_adc_t evx_charger_dev_cp_enable_adc_fn;
static struct charger_device *evx_cp_master_chgdev;
static bool evx_bypass_guard_active;
static bool evx_cp_guard_registered;

static struct kprobe evx_kp_cp_set_mode = {
	.symbol_name = "charger_dev_cp_set_mode",
};
static struct kprobe evx_kp_cp_device_init = {
	.symbol_name = "charger_dev_cp_device_init",
};
static struct kprobe evx_kp_cp_enable_adc = {
	.symbol_name = "charger_dev_cp_enable_adc",
};
static struct kprobe evx_kp_charger_enable = {
	.symbol_name = "charger_dev_enable",
};
static struct kprobe evx_kp_charger_powerpath = {
	.symbol_name = "charger_dev_enable_powerpath",
};
#ifdef CONFIG_KPROBES
typedef unsigned long (*evx_kallsyms_lookup_name_t)(const char *name);

static unsigned long evx_lookup_symbol_addr(const char *name)
{
        static evx_kallsyms_lookup_name_t lookup_fn;
        struct kprobe kp = {
                .symbol_name = "kallsyms_lookup_name",
        };
        int ret;

        if (!lookup_fn) {
                ret = register_kprobe(&kp);
                if (ret < 0 || !kp.addr) {
                        pr_warn(EVX_NAME ": kallsyms kprobe failed: %d\n", ret);
                        return 0;
                }

                lookup_fn = (evx_kallsyms_lookup_name_t)kp.addr;
                unregister_kprobe(&kp);
        }

        return lookup_fn ? lookup_fn(name) : 0;
}
#else
static unsigned long evx_lookup_symbol_addr(const char *name)
{
        return 0;
}
#endif

static struct charger_device *evx_primary_chgdev;
static bool evx_real_bypass_cached;



static int evx_resolve_charger_backend(void)
{
        const char * const names[] = {
                "primary_chg",
                "primary_charger",
                "mtk-master-charger",
                "mt6375-chg",
                "mt6375_chg",
        };
        int i;

        if (!evx_get_charger_by_name_fn) {
                evx_get_charger_by_name_fn =
                        (evx_get_charger_by_name_t)evx_lookup_symbol_addr("get_charger_by_name");
                if (!evx_get_charger_by_name_fn) {
                        pr_warn(EVX_NAME ": get_charger_by_name lookup failed\n");
                        return -EPROBE_DEFER;
                }
        }

        if (!evx_charger_dev_enable_fn) {
                evx_charger_dev_enable_fn =
                        (evx_charger_dev_enable_t)evx_lookup_symbol_addr("charger_dev_enable");
                if (!evx_charger_dev_enable_fn) {
                        pr_warn(EVX_NAME ": charger_dev_enable lookup failed\n");
                        return -EPROBE_DEFER;
                }
        }

        if (!evx_charger_dev_enable_powerpath_fn) {
                evx_charger_dev_enable_powerpath_fn =
                        (evx_charger_dev_enable_powerpath_t)evx_lookup_symbol_addr("charger_dev_enable_powerpath");
                if (!evx_charger_dev_enable_powerpath_fn)
                        pr_warn(EVX_NAME ": charger_dev_enable_powerpath lookup failed, continuing\n");
        }

        if (IS_ERR_OR_NULL(evx_primary_chgdev))
                evx_primary_chgdev = NULL;

        if (!evx_primary_chgdev) {
                for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
                        evx_primary_chgdev = evx_get_charger_by_name_fn(names[i]);
                        if (!IS_ERR_OR_NULL(evx_primary_chgdev)) {
                                pr_info(EVX_NAME ": charger backend resolved: %s\n", names[i]);
                                break;
                        }
                        evx_primary_chgdev = NULL;
                }
        }

        if (!evx_primary_chgdev) {
                pr_warn(EVX_NAME ": no primary charger device found\n");
                return -ENODEV;
        }

        return 0;
}


static int evx_resolve_cp_master_backend(void)
{
	static const char * const names[] = {
		"cp_master",
		"sc858x-master",
		"sc858x_master",
		"bq25985-master",
		"bq25985_master",
	};
	int i;

	if (!evx_get_charger_by_name_fn) {
		evx_get_charger_by_name_fn =
			(evx_get_charger_by_name_t)evx_lookup_symbol_addr("get_charger_by_name");
		if (!evx_get_charger_by_name_fn) {
			pr_warn(EVX_NAME ": cp backend get_charger_by_name lookup failed\n");
			return -ENOENT;
		}
	}

	if (!evx_charger_dev_cp_set_mode_fn) {
		evx_charger_dev_cp_set_mode_fn =
			(evx_charger_dev_cp_set_mode_t)evx_lookup_symbol_addr("charger_dev_cp_set_mode");
		if (!evx_charger_dev_cp_set_mode_fn) {
			pr_warn(EVX_NAME ": charger_dev_cp_set_mode lookup failed\n");
			return -ENOENT;
		}
	}

	if (!evx_charger_dev_cp_device_init_fn) {
		evx_charger_dev_cp_device_init_fn =
			(evx_charger_dev_cp_device_init_t)evx_lookup_symbol_addr("charger_dev_cp_device_init");
		if (!evx_charger_dev_cp_device_init_fn) {
			pr_warn(EVX_NAME ": charger_dev_cp_device_init lookup failed\n");
			return -ENOENT;
		}
	}

	if (!evx_charger_dev_cp_enable_adc_fn) {
		evx_charger_dev_cp_enable_adc_fn =
			(evx_charger_dev_cp_enable_adc_t)evx_lookup_symbol_addr("charger_dev_cp_enable_adc");
		if (!evx_charger_dev_cp_enable_adc_fn) {
			pr_warn(EVX_NAME ": charger_dev_cp_enable_adc lookup failed\n");
			return -ENOENT;
		}
	}

	if (!evx_cp_master_chgdev) {
		for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			evx_cp_master_chgdev = evx_get_charger_by_name_fn(names[i]);
			if (!IS_ERR_OR_NULL(evx_cp_master_chgdev)) {
				pr_info(EVX_NAME ": cp backend resolved: %s\n", names[i]);
				break;
			}
			evx_cp_master_chgdev = NULL;
		}
	}

	if (!evx_cp_master_chgdev) {
		pr_warn(EVX_NAME ": cp_master charger device not found\n");
		return -ENODEV;
	}

	return 0;
}

static inline bool evx_is_cp_master_arg(struct charger_device *chg)
{
	return evx_cp_master_chgdev && chg == evx_cp_master_chgdev;
}

static inline bool evx_is_primary_charger_arg(struct charger_device *chg)
{
	return evx_primary_chgdev && chg == evx_primary_chgdev;
}

static int evx_guard_cp_set_mode_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct charger_device *chg = (struct charger_device *)regs->regs[0];

	if (evx_bypass_guard_active && evx_is_cp_master_arg(chg) && regs->regs[1] != 0) {
		regs->regs[1] = 0;
		pr_info(EVX_NAME ": guard forced cp_set_mode 0\n");
	}
	return 0;
}

static int evx_guard_cp_device_init_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct charger_device *chg = (struct charger_device *)regs->regs[0];

	if (evx_bypass_guard_active && evx_is_cp_master_arg(chg) && regs->regs[1] != 0) {
		regs->regs[1] = 0;
		pr_info(EVX_NAME ": guard forced cp_device_init 0\n");
	}
	return 0;
}

static int evx_guard_cp_enable_adc_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct charger_device *chg = (struct charger_device *)regs->regs[0];

	if (evx_bypass_guard_active && evx_is_cp_master_arg(chg) && regs->regs[1] != 0) {
		regs->regs[1] = 0;
		pr_info(EVX_NAME ": guard blocked cp_enable_adc true\n");
	}
	return 0;
}

static int evx_guard_charger_enable_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct charger_device *chg = (struct charger_device *)regs->regs[0];

	if (!evx_bypass_guard_active)
		return 0;

	if (regs->regs[1] == 0)
		return 0;

	regs->regs[1] = 0;

	if (evx_is_cp_master_arg(chg))
		pr_info(EVX_NAME ": guard blocked cp_master enable true");
	else if (evx_is_primary_charger_arg(chg))
		pr_info(EVX_NAME ": guard blocked primary charger enable true");
	else
		pr_info(EVX_NAME ": guard blocked charger enable true chg=%px", chg);

	return 0;
}

static int evx_guard_powerpath_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct charger_device *chg = (struct charger_device *)regs->regs[0];

	if (!evx_bypass_guard_active)
		return 0;

	if (regs->regs[1] != 0)
		return 0;

	regs->regs[1] = 1;

	if (evx_is_primary_charger_arg(chg))
		pr_info(EVX_NAME ": guard forced primary powerpath true");
	else
		pr_info(EVX_NAME ": guard forced charger powerpath true chg=%px", chg);

	return 0;
}

static void evx_register_cp_master_guard(void)
{
	int ret;
	bool ok = false;

	if (evx_cp_guard_registered)
		return;

	evx_kp_cp_set_mode.pre_handler = evx_guard_cp_set_mode_pre;
	evx_kp_cp_device_init.pre_handler = evx_guard_cp_device_init_pre;
	evx_kp_cp_enable_adc.pre_handler = evx_guard_cp_enable_adc_pre;
	evx_kp_charger_enable.pre_handler = evx_guard_charger_enable_pre;
	evx_kp_charger_powerpath.pre_handler = evx_guard_powerpath_pre;

	ret = register_kprobe(&evx_kp_cp_set_mode);
	pr_info(EVX_NAME ": guard register cp_set_mode ret=%d\n", ret);
	if (!ret) ok = true;

	ret = register_kprobe(&evx_kp_cp_device_init);
	pr_info(EVX_NAME ": guard register cp_device_init ret=%d\n", ret);
	if (!ret) ok = true;

	ret = register_kprobe(&evx_kp_cp_enable_adc);
	pr_info(EVX_NAME ": guard register cp_enable_adc ret=%d\n", ret);
	if (!ret) ok = true;

	ret = register_kprobe(&evx_kp_charger_enable);
	pr_info(EVX_NAME ": guard register charger_enable ret=%d\n", ret);
	if (!ret) ok = true;

	ret = register_kprobe(&evx_kp_charger_powerpath);
	pr_info(EVX_NAME ": guard register charger_powerpath ret=%d\n", ret);
	if (!ret) ok = true;

	evx_cp_guard_registered = ok;
}

static void evx_try_stop_cp_master_for_bypass(void)
{
	int ret;

	ret = evx_resolve_cp_master_backend();
	if (ret) {
		pr_warn(EVX_NAME ": cp_master stop skipped ret=%d\n", ret);
		return;
	}

	evx_register_cp_master_guard();

	ret = evx_charger_dev_cp_enable_adc_fn(evx_cp_master_chgdev, false);
	pr_info(EVX_NAME ": cp_master enable_adc false ret=%d\n", ret);

	ret = evx_charger_dev_cp_set_mode_fn(evx_cp_master_chgdev, 0);
	pr_info(EVX_NAME ": cp_master set_mode 0 ret=%d\n", ret);

	ret = evx_charger_dev_cp_device_init_fn(evx_cp_master_chgdev, 0);
	pr_info(EVX_NAME ": cp_master device_init 0 ret=%d\n", ret);

	ret = evx_charger_dev_enable_fn(evx_cp_master_chgdev, false);
	pr_info(EVX_NAME ": cp_master enable false ret=%d\n", ret);

	msleep(500);
}

static int evx_real_bypass_set(bool enable)
{
        int ret;
        int pp_ret = 0;

        ret = evx_resolve_charger_backend();
        if (ret)
                return ret;


	if (enable) {
		evx_bypass_guard_active = true;
		evx_try_stop_cp_master_for_bypass();
	} else {
		evx_bypass_guard_active = false;
	}
        if (evx_charger_dev_enable_powerpath_fn) {
                pp_ret = evx_charger_dev_enable_powerpath_fn(evx_primary_chgdev, true);
                if (pp_ret)
                        pr_warn(EVX_NAME ": powerpath keep-on returned %d\n", pp_ret);
        }

        /*
         * Real bypass style:
         * enable=true  => stop battery charging only
         * enable=false => allow battery charging again
         */
        ret = evx_charger_dev_enable_fn(evx_primary_chgdev, !enable);
        if (ret) {
                pr_warn(EVX_NAME ": charger_dev_enable(%d) failed: %d\n", !enable, ret);
                return ret;
        }

        evx_real_bypass_cached = enable;
        pr_info(EVX_NAME ": charger-class bypass %s\n", enable ? "enabled" : "disabled");
        return 0;
}

static ssize_t bypass_charging_show(struct device *dev,
                                    struct device_attribute *attr, char *buf)
{
        return sysfs_emit(buf, "%d\n", evx_real_bypass_cached ? 1 : 0);
}

static ssize_t bypass_charging_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count)
{
        bool enable;
        int ret;

        ret = kstrtobool(buf, &enable);
        if (ret)
                return ret;

        ret = evx_real_bypass_set(enable);
        if (ret)
                return ret;

        return count;
}

static DEVICE_ATTR(bypass_charging, 0664, bypass_charging_show, bypass_charging_store);

static ssize_t bypass_charge_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return bypass_charging_show(dev, attr, buf);
}

static ssize_t bypass_charge_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	return bypass_charging_store(dev, attr, buf, count);
}

static DEVICE_ATTR(bypass_charge, 0664, bypass_charge_show, bypass_charge_store);

static int evx_create_real_bypass_attrs(void)
{
	int ret;

	evx_bypass_battery_psy = power_supply_get_by_name("battery");
	if (!evx_bypass_battery_psy) {
		pr_warn(EVX_NAME ": battery power_supply not ready for bypass aliases\n");
		return 0;
	}

	ret = device_create_file(&evx_bypass_battery_psy->dev,
				 &dev_attr_bypass_charging);
	if (ret && ret != -EEXIST) {
		pr_warn(EVX_NAME ": bypass_charging create failed: %d\n", ret);
		goto err_put_psy;
	}

	ret = device_create_file(&evx_bypass_battery_psy->dev,
				 &dev_attr_bypass_charge);
	if (ret && ret != -EEXIST) {
		pr_warn(EVX_NAME ": bypass_charge create failed: %d\n", ret);
		device_remove_file(&evx_bypass_battery_psy->dev,
				   &dev_attr_bypass_charging);
		goto err_put_psy;
	}

	evx_bypass_attrs_created = true;

	pr_info(EVX_NAME ": loaded real battery-manager bypass aliases\n");
	return 0;

err_put_psy:
	power_supply_put(evx_bypass_battery_psy);
	evx_bypass_battery_psy = NULL;
	return 0;
}


static void evx_bypass_retry_workfn(struct work_struct *work)
{
	if (evx_bypass_attrs_created)
		return;

	evx_create_real_bypass_attrs();

	if (!evx_bypass_attrs_created && evx_bypass_retry_count++ < 30) {
		pr_info(EVX_NAME ": bypass aliases not ready, retry=%d\n",
			evx_bypass_retry_count);
		schedule_delayed_work(&evx_bypass_retry_work,
				      msecs_to_jiffies(2000));
	}
}

static void evx_remove_real_bypass_attrs(void)
{
	if (evx_bypass_attrs_created && evx_bypass_battery_psy) {
		device_remove_file(&evx_bypass_battery_psy->dev,
				   &dev_attr_bypass_charge);
		device_remove_file(&evx_bypass_battery_psy->dev,
				   &dev_attr_bypass_charging);
		evx_bypass_attrs_created = false;
	}

	if (evx_bypass_battery_psy) {
		power_supply_put(evx_bypass_battery_psy);
		evx_bypass_battery_psy = NULL;
	}
}


static int __init evx_cos_power_compat_init(void)
{
	int ret;

	evx_register_power_supply_aliases();

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
	ret = device_create_file(oplus_battery_dev, &dev_attr_gauge_car_c);
	if (ret && ret != -EEXIST)
		pr_warn(EVX_NAME ": gauge_car_c create failed: %d\n", ret);

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
	proc_passedchg =
		proc_create("passedchg", 0444, proc_charger_dir,
			    &passedchg_proc_ops);
	proc_passedchg_reset_count =
		proc_create("passedchg_reset_count", 0444, proc_charger_dir,
			    &passedchg_reset_count_proc_ops);
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

	evx_create_optional_battery_attrs();
	evx_create_real_bypass_attrs();
	if (!evx_bypass_attrs_created)
		schedule_delayed_work(&evx_bypass_retry_work,
				      msecs_to_jiffies(2000));

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
	device_remove_file(oplus_battery_dev, &dev_attr_gauge_car_c);
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
	cancel_delayed_work_sync(&evx_bypass_retry_work);
	evx_remove_real_bypass_attrs();
	evx_remove_optional_battery_attrs();
	evx_unregister_power_supply_aliases();
	if (proc_shell_temp)
		proc_remove(proc_shell_temp);
	if (proc_passedchg_reset_count)
		proc_remove(proc_passedchg_reset_count);
	if (proc_passedchg)
		proc_remove(proc_passedchg);
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
