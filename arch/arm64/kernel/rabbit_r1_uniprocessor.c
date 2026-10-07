// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/kernel.h>

static int __init rabbit_r1_lock_uniprocessor(void)
{
	cpu_hotplug_disable();
	pr_info("rabbit-r1: CPU hotplug disabled (block r1-cpus SMP panic)\n");
	return 0;
}
arch_initcall(rabbit_r1_lock_uniprocessor);
