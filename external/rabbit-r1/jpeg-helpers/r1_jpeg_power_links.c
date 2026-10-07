// SPDX-License-Identifier: GPL-2.0-only
/* Attach quiesced JPEG and LARB devices to the RAM VCODEC provider. */
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


static int __init links_init(void)
{
 struct device_node *provider, *nodes[2] = { NULL, NULL };
 struct platform_device *devs[2] = { NULL, NULL };
 __be32 cells[2];
 int i, ret = -ENODEV;
 provider = of_find_node_by_path("/sleep@10006000/jpeg-power-controller");
 if (!provider || !provider->phandle) goto out;
 nodes[0] = of_find_node_by_path("/smi_larb1@17010000");
 nodes[1] = of_find_node_by_path("/venc_jpg@17030000");
 for (i = 0; i < 2; i++) {
  if (!nodes[i]) goto out;
  devs[i] = of_find_device_by_node(nodes[i]);
  if (!devs[i]) goto out;
  if (devs[i]->dev.driver) { ret = -EBUSY; goto out; }
 }
 cells[0] = cpu_to_be32(provider->phandle); cells[1] = cpu_to_be32(10);
 of_changeset_init(&changes);
 ret = set_prop(nodes[0], "power-domains", cells, sizeof(cells));
 if (!ret) ret = set_prop(nodes[1], "power-domains", cells, sizeof(cells));
 if (!ret) ret = of_changeset_apply(&changes);
 if (ret) of_changeset_destroy(&changes);
 else pr_info("r1-jpeg-power-links: JPEG/LARB linked; explicitly rebind suppliers before JPEG\n");
out:
 for (i = 0; i < 2; i++) { if (devs[i]) put_device(&devs[i]->dev); of_node_put(nodes[i]); }
 of_node_put(provider);
 return ret;
}
module_init(links_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 diagnostic VCODEC consumer links");
