// SPDX-License-Identifier: GPL-2.0-only
/* RAM-only adapter for the existing Linux MT6765 scpsys driver.
 * Does not replace the GPU/modem/display controllers or run DMA.
 * No unload: live overlay/provider lifetime ends at reboot.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include "vcodec-power-blob.h"

static int overlay_id;
static int __init r1_jpeg_power_nodes_init(void)
{
 struct device_node *old, *mm, *parent, *node;
 struct platform_device *parent_dev, *pdev;
 u32 access;
 int ret;
 old = of_find_node_by_path("/sleep@10006000/power-controller");
 mm = of_find_node_by_path("/mmsys_config@14000000");
 if (!old || !mm || mm->phandle != 0x61 ||
     of_property_read_u32(old, "access-controllers", &access) || access != 0x25) {
  ret = -EINVAL;
  goto refs;
 }
 node = of_find_node_by_path("/sleep@10006000/jpeg-power-controller");
 if (node) { of_node_put(node); ret = -EEXIST; goto refs; }
 parent = of_get_parent(old);
 parent_dev = of_find_device_by_node(parent);
 of_node_put(parent);
 if (!parent_dev) { ret = -ENODEV; goto refs; }
 ret = of_overlay_fdt_apply(vcodec_power_blob, sizeof(vcodec_power_blob), &overlay_id, NULL);
 if (ret) { put_device(&parent_dev->dev); goto refs; }
 node = of_find_node_by_path("/sleep@10006000/jpeg-power-controller");
 pdev = of_find_device_by_node(node);
 if (pdev)
  put_device(&pdev->dev);
 else
  pdev = of_platform_device_create(node, NULL, &parent_dev->dev);
 of_node_put(node);
 put_device(&parent_dev->dev);
 /* Keep the overlay resident even if platform probe defers/fails. */
 pr_info("r1-jpeg-power: overlay=%d platform_created=%d; inspect genpd before attaching consumers\n", overlay_id, !!pdev);
 ret = 0;
refs:
 of_node_put(old); of_node_put(mm);
 return ret;
}
module_init(r1_jpeg_power_nodes_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 isolated VCODEC genpd RAM adapter");
