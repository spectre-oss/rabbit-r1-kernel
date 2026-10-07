// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
static struct of_changeset changes;
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


static int __init selector_init(void)
{
 struct device_node *jpeg, *vc;
 struct platform_device *pdev = NULL;
 __be32 phandle;
 int ret = -ENODEV;
 jpeg = of_find_node_by_path("/venc_jpg@17030000");
 vc = of_find_node_by_path("/vcodecsys@17000000");
 if (!jpeg || !vc || !vc->phandle) goto out;
 pdev = of_find_device_by_node(jpeg);
 if (!pdev) goto out;
 if (pdev->dev.driver) { ret = -EBUSY; goto out; }
 phandle = cpu_to_be32(vc->phandle);
 of_changeset_init(&changes);
 ret = set_prop(jpeg, "mediatek,vcodec-syscon", &phandle, sizeof(phandle));
 if (!ret) ret = of_changeset_apply(&changes);
 if (ret) of_changeset_destroy(&changes);
out:
 if (pdev) put_device(&pdev->dev);
 of_node_put(jpeg); of_node_put(vc);
 return ret;
}
module_init(selector_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 RAM codec-selector reference");
