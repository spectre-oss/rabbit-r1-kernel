// SPDX-License-Identifier: GPL-2.0-only
/* RAM-only SMI/M4U binding adapter for translated JPEG diagnostics.
 * Requires the codec domain and physical SMI path prepared beforehand.
 * Does not start DMA. IOMMU driver controls translation after consumer binding.
 * Properties outlive this module; intentionally no unload entry point.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
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

static int attach_node(struct device_node *node)
{
	struct platform_device *pdev = of_find_device_by_node(node);
	int ret;
	if (!pdev) return -ENODEV;
	ret = device_attach(&pdev->dev);
	pr_info("r1-jpeg-iommu: attach %pOF=%d\n", node, ret);
	put_device(&pdev->dev);
	return ret < 0 ? ret : 0;
}
static int __init r1_jpeg_iommu_nodes_init(void)
{
	struct device_node *common, *larb, *mmu, *jpeg, *mm, *vc, *iter;
	struct device_node *nodes[4];
	struct platform_device *pdev;
	__be32 common_clks[8], larb_clks[4], ports[4], val;
	u32 handle = 0;
	int i, ret = -ENODEV;
	const u32 ids[] = {19,19,21,22};
	common = of_find_node_by_path("/smi_common@14002000");
	larb = of_find_node_by_path("/smi_larb1@17010000");
	mmu = of_find_node_by_path("/m4u@10205000");
	jpeg = of_find_node_by_path("/venc_jpg@17030000");
	mm = of_find_node_by_path("/r1-display/clock-controller@14000000");
	if (!mm)
		mm = of_find_compatible_node(NULL, NULL, "mediatek,mt6765-mmsys");
	vc = of_find_compatible_node(NULL, NULL, "mediatek,mt6765-vcodecsys");
	if (!common || !larb || !mmu || !jpeg || !mm || !vc) goto out;
	nodes[0]=common; nodes[1]=larb; nodes[2]=mmu; nodes[3]=jpeg;
	for (i=0;i<4;i++) {
		pdev = of_find_device_by_node(nodes[i]);
		if (!pdev) goto out;
		ret = pdev->dev.driver ? -EBUSY : 0;
		put_device(&pdev->dev);
		if (ret) goto out;
	}
	if (!common->phandle || !larb->phandle || !mm->phandle || !vc->phandle) {
		ret=-EINVAL; goto out;
	}
	/* This stock node has no phandle and no consumers. Reserve a fresh value
	 * before publishing its consumer properties; never renumber an existing one.
	 * RAM-only test adapter is permanent; production DT must encode this link.
	 */
	if (!mmu->phandle) {
		for (handle=0x10000; handle<0x10100; handle++) {
			iter=of_find_node_by_phandle(handle);
			if (!iter) break;
			of_node_put(iter);
		}
		if (handle==0x10100) { ret=-ENOSPC; goto out; }
	} else handle=mmu->phandle;
	for (i=0;i<4;i++) {
		common_clks[2*i]=cpu_to_be32(mm->phandle);
		common_clks[2*i+1]=cpu_to_be32(ids[i]);
	}
	larb_clks[0]=larb_clks[2]=cpu_to_be32(vc->phandle);
	larb_clks[1]=cpu_to_be32(0); larb_clks[3]=cpu_to_be32(1);
	ports[0]=ports[2]=cpu_to_be32(handle);
	ports[1]=cpu_to_be32((1<<5)|5); ports[3]=cpu_to_be32((1<<5)|6);
	of_changeset_init(&changes);
#define PROP(n,k,v) do { ret=set_prop(n,k,&(v),sizeof(v)); if(ret) goto destroy; } while(0)
	PROP(common,"compatible","mediatek,mt6765-smi-common");
	PROP(common,"clocks",common_clks);
	PROP(common,"clock-names","apb\0smi\0gals0\0gals1");
	PROP(larb,"compatible","mediatek,mt6765-smi-larb");
	PROP(larb,"clocks",larb_clks);
	PROP(larb,"clock-names","apb\0smi");
	val=cpu_to_be32(common->phandle); PROP(larb,"mediatek,smi",val);
	val=cpu_to_be32(1); PROP(larb,"mediatek,larb-id",val);
	PROP(mmu,"compatible","mediatek,mt6765-m4u");
	PROP(mmu,"#iommu-cells",val);
	val=cpu_to_be32(handle); PROP(mmu,"phandle",val);
	val=cpu_to_be32(larb->phandle); PROP(mmu,"mediatek,larbs",val);
	PROP(jpeg,"iommus",ports);
	ret=of_changeset_apply(&changes);
	if (ret) goto destroy;
	mmu->phandle=handle;
	/* Leave JPEG unbound until all three suppliers demonstrably attach. */
	for (i=0;i<3;i++) {
		ret=attach_node(nodes[i]);
		if(ret) break;
		if (i == 0) {
			/* Display scanout already uses this shared clock path without
			 * a runtime-PM consumer link. Pin it BEFORE the IOMMU probe's
			 * temporary reference can drop and gate the display's bus.
			 */
			pdev = of_find_device_by_node(common);
			if (!pdev) { ret = -ENODEV; break; }
			ret = pm_runtime_resume_and_get(&pdev->dev);
			if (ret >= 0) {
				pm_runtime_forbid(&pdev->dev);
				pm_runtime_put(&pdev->dev);
				ret = 0;
				pr_info("r1-jpeg-iommu: shared display bus pinned before IOMMU attachment\n");
			}
			put_device(&pdev->dev);
			if (ret) break;
		}
	}
	pr_info("r1-jpeg-iommu: bindings installed, supplier status=%d, JPEG remains unbound\n",ret);
	ret=0; goto out;
destroy:
	of_changeset_destroy(&changes);
out:
	of_node_put(common);of_node_put(larb);of_node_put(mmu);
	of_node_put(jpeg);of_node_put(mm);of_node_put(vc);
	return ret;
}
module_init(r1_jpeg_iommu_nodes_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 RAM-only JPEG SMI/IOMMU binding adapter");
