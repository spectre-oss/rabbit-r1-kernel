// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
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


static int __init venc_nodes_init(void)
{
 struct device_node *node, *jpeg, *mmu, *vc;
 struct platform_device *pdev = NULL;
 const void *value;
 int length, i, ret = -ENODEV;
 const unsigned int ids[] = { 32, 33, 34, 35, 36, 39, 40, 41, 42 };
 __be32 ports[18], phandle;
 const char names[] = "smi-common\0smi-comm0\0smi-comm1\0larb\0venc";
 node = of_find_node_by_path("/venc@17020000");
 jpeg = of_find_node_by_path("/venc_jpg@17030000");
 mmu = of_find_node_by_path("/m4u@10205000");
 vc = of_find_node_by_path("/vcodecsys@17000000");
 if (!node || !jpeg || !mmu || !vc || !mmu->phandle || !vc->phandle) goto out;
 if (!of_device_is_compatible(jpeg, "mediatek,mt6765-jpgenc") ||
     !of_device_is_compatible(mmu, "mediatek,mt6765-m4u")) goto out;
 pdev = of_find_device_by_node(node);
 if (!pdev) goto out;
 if (pdev->dev.driver) { ret = -EBUSY; goto out; }
 of_changeset_init(&changes);
 value = of_get_property(jpeg, "clocks", &length);
 if (!value || length != 12 * sizeof(__be32)) { ret = -EINVAL; goto destroy; }
 ret = set_prop(node, "clocks", value, 10 * sizeof(__be32));
 if (ret) goto destroy;
 ret = set_prop(node, "clock-names", names, sizeof(names));
 if (ret) goto destroy;
 value = of_get_property(jpeg, "power-domains", &length);
 if (!value || length != 2 * sizeof(__be32)) { ret = -EINVAL; goto destroy; }
 ret = set_prop(node, "power-domains", value, length);
 if (ret) goto destroy;
 for (i = 0; i < ARRAY_SIZE(ids); i++) {
  ports[2*i] = cpu_to_be32(mmu->phandle);
  ports[2*i+1] = cpu_to_be32(ids[i]);
 }
 ret = set_prop(node, "iommus", ports, sizeof(ports));
 if (ret) goto destroy;
 phandle = cpu_to_be32(vc->phandle);
 ret = set_prop(node, "mediatek,vcodec-syscon", &phandle, sizeof(phandle));
 if (ret) goto destroy;
 ret = set_prop(node, "compatible", "rabbit,r1-venc", sizeof("rabbit,r1-venc"));
 if (ret) goto destroy;
 ret = of_changeset_apply(&changes);
 if (ret) goto destroy;
 /* Publish the IOMMU relationship while the consumer is still unbound.
  * Its platform device predates these runtime properties, so initial bus
  * enumeration could not configure DMA. Do not defer this to driver probe.
  */
 ret = of_dma_configure(&pdev->dev, node, true);
 if (ret) {
  pr_err("r1-venc-nodes: DMA configuration failed: %d\n", ret);
  /* Published properties remain resident, as with the other adapters. */
  goto out;
 }
 /* No automatic probe or hardware action. Load the native driver separately.
  * The properties remain resident, matching the existing JPEG adapters.
  */
 pr_info("r1-venc-nodes: video power/clock/IOMMU links prepared; no DMA started\n");
 goto out;
destroy:
 of_changeset_destroy(&changes);
out:
 if (pdev) put_device(&pdev->dev);
 of_node_put(node); of_node_put(jpeg); of_node_put(mmu); of_node_put(vc);
 return ret;
}
module_init(venc_nodes_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 RAM video encoder device-tree links");
