// SPDX-License-Identifier: GPL-2.0-only
/* Repair a deferred diagnostic domain after native display replaces MM clocks. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
static struct of_changeset changes;
static int __init fix_init(void)
{
 struct device_node *src, *dst, *controller;
 struct platform_device *pdev;
 struct property *p;
 const void *clocks;
 int size, ret = -ENODEV;
 src = of_find_node_by_path("/venc_jpg@17030000");
 dst = of_find_node_by_path("/sleep@10006000/jpeg-power-controller/power-domain@a");
 controller = of_find_node_by_path("/sleep@10006000/jpeg-power-controller");
 if (!src || !dst || !controller) goto out;
 pdev = of_find_device_by_node(controller);
 if (!pdev) goto out;
 if (pdev->dev.driver) { ret = -EBUSY; goto put; }
 clocks = of_get_property(src, "clocks", &size);
 if (!clocks || size < 24) { ret = -EINVAL; goto put; }
 p = kzalloc(sizeof(*p), GFP_KERNEL);
 if (!p) { ret = -ENOMEM; goto put; }
 p->name = kstrdup("clocks", GFP_KERNEL);
 p->value = kmemdup(clocks, 24, GFP_KERNEL);
 p->length = 24;
 if (!p->name || !p->value) { kfree(p->name); kfree(p->value); kfree(p); ret = -ENOMEM; goto put; }
 of_changeset_init(&changes);
 ret = of_changeset_update_property(&changes, dst, p);
 if (!ret) ret = of_changeset_apply(&changes);
 if (ret) { of_changeset_destroy(&changes); goto put; }
 ret = device_attach(&pdev->dev);
 pr_info("r1-jpeg-power-clocks: existing JPEG MM clocks adopted; attach=%d\n", ret);
 ret = 0;
put:
 put_device(&pdev->dev);
out:
 of_node_put(src); of_node_put(dst); of_node_put(controller);
 return ret;
}
module_init(fix_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 diagnostic VCODEC clock provider repair");
