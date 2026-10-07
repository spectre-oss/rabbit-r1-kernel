// SPDX-License-Identifier: GPL-2.0-only
/* Apply fixed, ordered R1 display overlays only after Linux has booted.
 * No automatic takeover, unload, or overlay removal: reboot restores the DT.
 */
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/slab.h>

struct stage_patch { unsigned int offset, provider; };
struct stage_blob { const char *name; const void *data; size_t size; const struct stage_patch *patches; size_t count; };
#include "display-overlays.h"

static DEFINE_MUTEX(stage_lock);
static unsigned int next_stage;
static bool ready;
static int overlay_ids[ARRAY_SIZE(stages)];

static int apply_stage(const char *value, const struct kernel_param *kp)
{
	unsigned int stage;
	int ret;
	void *blob;
	unsigned int i;

	if (!ready)
		return -EPERM;
	ret = kstrtouint(value, 0, &stage);
	if (ret)
		return ret;
	mutex_lock(&stage_lock);
	if (stage != next_stage || stage >= ARRAY_SIZE(stages)) {
		ret = -EINVAL;
		goto out;
	}
	blob = kmemdup(stages[stage].data, stages[stage].size, GFP_KERNEL);
	if (!blob) { ret = -ENOMEM; goto out; }
	for (i = 0; i < stages[stage].count; i++) {
		const struct stage_patch *patch = &stages[stage].patches[i];
		struct device_node *provider = of_find_node_by_path(patch->provider == 1 ?
			"/r1-display/clock-controller@14000000" : "/r1-display/phy@11c80000");
		if (!provider || !provider->phandle || patch->offset + 4 > stages[stage].size) {
			of_node_put(provider); kfree(blob); ret = -EINVAL; goto out;
		}
		*(__be32 *)(blob + patch->offset) = cpu_to_be32(provider->phandle);
		of_node_put(provider);
	}
	ret = of_overlay_fdt_apply(blob, stages[stage].size,
				   &overlay_ids[stage], NULL);
	kfree(blob);
	if (!ret && stage == 0) {
		struct device_node *bus = of_find_node_by_path("/r1-display");
		struct platform_device *pdev = bus ? of_find_device_by_node(bus) : NULL;

		ret = pdev ? of_platform_populate(bus, NULL, NULL, &pdev->dev) : -ENODEV;
		if (pdev)
			put_device(&pdev->dev);
		of_node_put(bus);
	}
	if (!ret) {
		next_stage++;
		pr_info("r1_display_nodes: stage %u %s applied, overlay=%d\n",
			stage, stages[stage].name, overlay_ids[stage]);
	} else {
		/* Failed overlay removal/reapply is not a recovery strategy. */
		ready = false;
		pr_err("r1_display_nodes: stage %u failed=%d; reboot before retry\n", stage, ret);
	}
out:
	mutex_unlock(&stage_lock);
	return ret;
}

static int get_stage(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(next_stage));
}

static const struct kernel_param_ops stage_ops = { .set = apply_stage, .get = get_stage };
module_param_cb(stage, &stage_ops, NULL, 0600);

static int __init r1_display_nodes_init(void)
{
	struct device_node *node;

	if (!of_machine_is_compatible("mediatek,MT6765"))
		return -ENODEV;
	node = of_find_node_by_path("/r1-display");
	if (node) {
		of_node_put(node);
		return -EEXIST;
	}
	ready = true;
	pr_info("r1_display_nodes: ready; no nodes changed until explicit stage write\n");
	return 0;
}
module_init(r1_display_nodes_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ordered Rabbit R1 runtime native display nodes");
