// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX ColorOS display/backlight compatibility mirrors for rodin.
 * Read-only mirror nodes only. Does not touch real Xiaomi displayfeature/LHBM/FOD.
 */

#include <linux/backlight.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/string.h>

#define EVX_DISPLAY_NAME "evonix_cos_display_compat"

#define EVX_PRIMARY_LED_BL "/sys/class/leds/lcd-backlight/brightness"
#define EVX_PRIMARY_PANEL_BL "/sys/class/backlight/panel0-backlight/brightness"

static struct led_classdev evx_lcd_backlight1_cdev;
static struct backlight_device *evx_panel1_bd;
static struct kobject *evx_oplus_display_kobj;

static int evx_read_int_file(const char *path, int fallback)
{
	struct file *filp;
	char buf[64];
	loff_t pos = 0;
	ssize_t len;
	int val;

	filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp))
		return fallback;

	len = kernel_read(filp, buf, sizeof(buf) - 1, &pos);
	filp_close(filp, NULL);

	if (len <= 0)
		return fallback;

	buf[len] = '\0';

	if (kstrtoint(strim(buf), 0, &val))
		return fallback;

	return val;
}

static int evx_get_real_brightness(void)
{
	int val;

	val = evx_read_int_file(EVX_PRIMARY_LED_BL, -1);
	if (val >= 0)
		return val;

	val = evx_read_int_file(EVX_PRIMARY_PANEL_BL, -1);
	if (val >= 0)
		return val;

	return 0;
}

static enum led_brightness evx_lcd_backlight1_brightness_get(struct led_classdev *cdev)
{
	int val = evx_get_real_brightness();

	if (val < 0)
		val = 0;
	if (val > cdev->max_brightness)
		val = cdev->max_brightness;

	return val;
}

static int evx_panel1_get_brightness(struct backlight_device *bd)
{
	return evx_get_real_brightness();
}

static const struct backlight_ops evx_panel1_backlight_ops = {
	.get_brightness = evx_panel1_get_brightness,
};

static ssize_t oplus_brightness_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", evx_get_real_brightness());
}

static struct kobj_attribute oplus_brightness_attr =
	__ATTR_RO(oplus_brightness);

static int __init evx_cos_display_compat_init(void)
{
	struct backlight_properties props = {};
	int ret;

	evx_lcd_backlight1_cdev.name = "lcd-backlight1";
	evx_lcd_backlight1_cdev.max_brightness = 2047;
	evx_lcd_backlight1_cdev.brightness_get =
		evx_lcd_backlight1_brightness_get;

	ret = led_classdev_register(NULL, &evx_lcd_backlight1_cdev);
	if (ret)
		pr_warn(EVX_DISPLAY_NAME ": lcd-backlight1 register failed: %d\n",
			ret);

	props.type = BACKLIGHT_RAW;
	props.max_brightness = 2047;
	props.brightness = evx_get_real_brightness();

	evx_panel1_bd = backlight_device_register("panel1-backlight", NULL,
						  NULL,
						  &evx_panel1_backlight_ops,
						  &props);
	if (IS_ERR(evx_panel1_bd)) {
		pr_warn(EVX_DISPLAY_NAME ": panel1-backlight register failed: %ld\n",
			PTR_ERR(evx_panel1_bd));
		evx_panel1_bd = NULL;
	}

	evx_oplus_display_kobj = kobject_create_and_add("oplus_display",
							kernel_kobj);
	if (evx_oplus_display_kobj) {
		ret = sysfs_create_file(evx_oplus_display_kobj,
					&oplus_brightness_attr.attr);
		if (ret)
			pr_warn(EVX_DISPLAY_NAME ": oplus_brightness create failed: %d\n",
				ret);
	}

	pr_info(EVX_DISPLAY_NAME ": loaded display mirror nodes\n");
	return 0;
}

static void __exit evx_cos_display_compat_exit(void)
{
	if (evx_oplus_display_kobj) {
		sysfs_remove_file(evx_oplus_display_kobj,
				  &oplus_brightness_attr.attr);
		kobject_put(evx_oplus_display_kobj);
		evx_oplus_display_kobj = NULL;
	}

	if (evx_panel1_bd) {
		backlight_device_unregister(evx_panel1_bd);
		evx_panel1_bd = NULL;
	}

	led_classdev_unregister(&evx_lcd_backlight1_cdev);
}

module_init(evx_cos_display_compat_init);
module_exit(evx_cos_display_compat_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("EVONIX ColorOS display/backlight compatibility mirrors");
