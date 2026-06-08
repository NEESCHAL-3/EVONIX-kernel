// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX kernel identification node
 * Device target: POCO X7 Pro / rodin
 */

#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/utsname.h>

static int evonix_info_show(struct seq_file *m, void *v)
{
	seq_puts(m, "EVONIX Kernel\n");
	seq_puts(m, "Maintainer: Neeschal\n");
	seq_puts(m, "Device target: POCO X7 Pro / rodin\n");
	seq_puts(m, "Release: v3.0\n");
	seq_puts(m, "Base: 6.6 LTS\n");
	seq_puts(m, "Linux: 6.6.139\n");
	seq_printf(m, "Kernel release: %s\n", utsname()->release);

	return 0;
}

static int __init evonix_info_init(void)
{
	proc_create_single("evonix_info", 0444, NULL, evonix_info_show);
	return 0;
}

fs_initcall(evonix_info_init);
