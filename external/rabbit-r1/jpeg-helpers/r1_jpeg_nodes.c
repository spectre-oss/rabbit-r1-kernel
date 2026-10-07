// SPDX-License-Identifier: GPL-2.0-only
/* RAM-only stock-DT adapter for queued JPEG diagnostics.
 * Requires the codec domain and physical SMI path prepared beforehand.
 * Does not start DMA or enable address translation. Reboot restores the DT.
 * Properties outlive this module; intentionally no unload entry point.
 */
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

static struct of_changeset changes;
/* The native display currently inherits these bootloader-enabled gates without
 * owning clock-framework references. Keep them on for this board lifetime,
 * before the codec genpd can take and release temporary references. */
static struct clk_bulk_data display_bus[] = {
	{ .id = "smi-common" },
	{ .id = "smi-comm0" },
	{ .id = "smi-comm1" },
};
static int set_prop(struct device_node *node, const char *name,
                    const void *value, size_t size)
{
	struct property *p = kzalloc(sizeof(*p), GFP_KERNEL);
	int ret;
	if (!p)
		return -ENOMEM;
	p->name = kstrdup(name, GFP_KERNEL);
	p->value = kmemdup(value, size, GFP_KERNEL);
	p->length = size;
	if (!p->name || !p->value) {
		kfree(p->name); kfree(p->value); kfree(p);
		return -ENOMEM;
	}
	ret = of_find_property(node, name, NULL) ?
		of_changeset_update_property(&changes, node, p) :
		of_changeset_add_property(&changes, node, p);
	if (ret) {
		kfree(p->name); kfree(p->value); kfree(p);
	}
	return ret;
}

static int __init r1_jpeg_nodes_init(void)
{
	struct device_node *node, *mm, *vc;
	struct platform_device *pdev = NULL;
	const char names[] = "smi-common\0smi-comm0\0smi-comm1\0larb\0venc\0jpgenc";
	const char compat[] = "mediatek,mt6765-jpgenc";
	__be32 clocks[12];
	const u32 ids[] = { 19, 21, 22, 0, 1, 2 };
	int i, ret = -ENODEV;

	node = of_find_node_by_path("/venc_jpg@17030000");
	mm = of_find_node_by_path("/r1-display/clock-controller@14000000");
	if (!mm)
		mm = of_find_compatible_node(NULL, NULL, "mediatek,mt6765-mmsys");
	vc = of_find_compatible_node(NULL, NULL, "mediatek,mt6765-vcodecsys");
	if (!node || !mm || !vc || !mm->phandle || !vc->phandle)
		goto out;
	pdev = of_find_device_by_node(node);
	if (!pdev)
		goto out;
	if (pdev->dev.driver) {
		ret = -EBUSY;
		goto out;
	}
	for (i = 0; i < 6; i++) {
		clocks[2*i] = cpu_to_be32(i < 3 ? mm->phandle : vc->phandle);
		clocks[2*i+1] = cpu_to_be32(ids[i]);
	}
	of_changeset_init(&changes);
	ret = set_prop(node, "clocks", clocks, sizeof(clocks));
	if (!ret) ret = set_prop(node, "clock-names", names, sizeof(names));
	if (!ret) ret = set_prop(node, "compatible", compat, sizeof(compat));
	if (ret) goto destroy;
	ret = of_changeset_apply(&changes);
	if (ret) goto destroy;
	ret = clk_bulk_get(&pdev->dev, ARRAY_SIZE(display_bus), display_bus);
	if (ret) goto revert;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(display_bus), display_bus);
	if (ret) {
		clk_bulk_put(ARRAY_SIZE(display_bus), display_bus);
		goto revert;
	}
	pr_info("r1-jpeg-nodes: shared display bus clock references retained\n");
	ret = device_attach(&pdev->dev);
	pr_info("r1-jpeg-nodes: standard JPEG attach=%d; physical DMA diagnostic, no IOMMU changes\n", ret);
	/* Retain live properties even when probe defers or fails. */
	ret = 0;
	goto out;
revert:
	if (of_changeset_revert(&changes)) {
		pr_err("r1-jpeg-nodes: failed to revert clock setup; reboot required\n");
		goto out;
	}
destroy:
	of_changeset_destroy(&changes);
out:
	if (pdev) put_device(&pdev->dev);
	of_node_put(node); of_node_put(mm); of_node_put(vc);
	return ret;
}
module_init(r1_jpeg_nodes_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 RAM-only JPEG device-tree adapter for V4L2 testing");
