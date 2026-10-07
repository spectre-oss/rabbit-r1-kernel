// SPDX-License-Identifier: GPL-2.0
/* Replace only modem clock lists with native MT6765 IDs in the live OF tree.
 * No boot image change. Retain changeset storage until a clean reboot.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <dt-bindings/clock/mt6765-clk.h>
static struct of_changeset modem_changes;
static struct property *property(const char *name, const void *value, int len)
{
 struct property *p = kzalloc(sizeof(*p), GFP_KERNEL);
 if (!p) return NULL;
 p->name = kstrdup(name, GFP_KERNEL);
 p->value = kmemdup(value, len, GFP_KERNEL);
 p->length = len;
 if (!p->name || !p->value) { kfree(p->name); kfree(p->value); kfree(p); return NULL; }
 return p;
}
static int add_clocks(const char *path, const u32 *ids, int count,
                     const char *names, int names_len, u32 provider)
{
 struct device_node *np = of_find_node_by_path(path);
 struct platform_device *pdev;
 struct property *p;
 __be32 cells[12];
 int i, ret;
 if (!np || count > 6) { of_node_put(np); return -EINVAL; }
 pdev = of_find_device_by_node(np);
 if (!pdev) { of_node_put(np); return -ENODEV; }
 ret = pdev->dev.driver ? -EBUSY : 0;
 put_device(&pdev->dev);
 if (ret) { of_node_put(np); return ret; }
 for (i = 0; i < count; i++) {
  cells[2*i] = cpu_to_be32(provider);
  cells[2*i+1] = cpu_to_be32(ids[i]);
 }
 p = property("clocks", cells, count * 8);
 if (!p) { of_node_put(np); return -ENOMEM; }
 ret = of_changeset_update_property(&modem_changes, np, p);
 if (ret) { of_node_put(np); return ret; }
 p = property("clock-names", names, names_len);
 if (!p) { of_node_put(np); return -ENOMEM; }
 ret = of_changeset_update_property(&modem_changes, np, p);
 of_node_put(np);
 return ret;
}
int r1_prepare_modem_nodes(void)
{
 struct device_node *infra = of_find_node_by_path("/infracfg_ao@10001000");
 u32 provider;
 int ret;
 static const u32 ccif[] = { CLK_IFR_CCIF_AP, CLK_IFR_CCIF_MD,
  CLK_IFR_CCIF1_AP, CLK_IFR_CCIF1_MD, CLK_IFR_CCIF2_AP, CLK_IFR_CCIF2_MD };
 static const u32 cldma[] = { CLK_IFR_CLDMA_BCLK };
 static const char ccif_names[] = "infra-ccif-ap\0infra-ccif-md\0infra-ccif1-ap\0infra-ccif1-md\0infra-ccif2-ap\0infra-ccif2-md";
 static const char cldma_names[] = "infra-cldma-bclk";
 if (!infra) return -ENODEV;
 provider = infra->phandle;
 of_node_put(infra);
 if (!provider) return -EINVAL;
 of_changeset_init(&modem_changes);
 ret = add_clocks("/ccifdriver@10209000", ccif, ARRAY_SIZE(ccif), ccif_names, sizeof(ccif_names), provider);
 if (ret) return ret;
 ret = add_clocks("/cldmadriver@10014000", cldma, ARRAY_SIZE(cldma), cldma_names, sizeof(cldma_names), provider);
 if (ret) return ret;
 ret = of_changeset_apply(&modem_changes);
 if (!ret) pr_info("r1_eccci: native CCIF/CLDMA clock lists applied\n");
 return ret;
}
