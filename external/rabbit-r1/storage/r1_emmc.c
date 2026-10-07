// SPDX-License-Identifier: GPL-2.0
/* RAM-only adaptation of the existing, unbound stock MSDC0 node.
 * Uses the native mtk-msdc driver, no raw register pokes, no data writes.
 * Low-speed discovery only. LK's pad mux and powered rails are retained.
 * No unload: changes must outlive all MMC driver and block-device users.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <dt-bindings/clock/mt6765-clk.h>

static struct of_changeset changes;
static struct device_node *node;

static int prop(const char *name, const void *data, size_t len)
{
    struct property *p = kzalloc(sizeof(*p), GFP_KERNEL);
    if (!p) return -ENOMEM;
    p->name = kstrdup(name, GFP_KERNEL);
    p->value = kmemdup(data, len ?: 1, GFP_KERNEL);
    p->length = len;
    if (!p->name || !p->value) return -ENOMEM;
    return of_find_property(node, name, NULL)
        ? of_changeset_update_property(&changes, node, p)
        : of_changeset_add_property(&changes, node, p);
}
static int drop(const char *name)
{
    struct property *p = of_find_property(node, name, NULL);
    return p ? of_changeset_remove_property(&changes, node, p) : 0;
}
#define SET(n, v) do { ret = prop(n, v, sizeof(v)); if (ret) goto fail; } while (0)
#define DROP(n) do { ret = drop(n); if (ret) goto fail; } while (0)
static int __init r1_emmc_init(void)
{
    struct device_node *top, *infra;
    struct platform_device *pdev;
    __be32 clocks[6], frequency = cpu_to_be32(26000000);
    const char names[] = "source\0hclk\0source_cg";
    const char pins[] = "default\0state_uhs";
    int ret;
    if (!of_machine_is_compatible("mediatek,MT6765")) return -ENODEV;
    node = of_find_node_by_path("/msdc@11230000");
    top = of_find_node_by_path("/topckgen@10000000");
    infra = of_find_node_by_path("/infracfg_ao@10001000");
    if (!node || !top || !infra) return -ENODEV;
    pdev = of_find_device_by_node(node);
    if (!pdev) return -ENODEV;
    if (pdev->dev.driver) { put_device(&pdev->dev); return -EBUSY; }
    clocks[0] = cpu_to_be32(top->phandle);
    clocks[1] = cpu_to_be32(CLK_TOP_MSDC50_0);
    clocks[2] = cpu_to_be32(infra->phandle);
    clocks[3] = cpu_to_be32(CLK_IFR_MSDC0);
    clocks[4] = cpu_to_be32(infra->phandle);
    clocks[5] = cpu_to_be32(CLK_IFR_MSDC0_SRC);
    of_node_put(top); of_node_put(infra);
    of_changeset_init(&changes);
    SET("compatible", "mediatek,mt6765-mmc");
    SET("clocks", clocks); SET("clock-names", names);
    ret = prop("max-frequency", &frequency, sizeof(frequency)); if (ret) goto fail;
    SET("pinctrl-names", pins);
    ret = prop("pinctrl-0", "", 0); if (ret) goto fail;
    ret = prop("pinctrl-1", "", 0); if (ret) goto fail;
    DROP("mmc-hs400-1_8v"); DROP("mmc-hs200-1_8v");
    DROP("mmc-ddr-1_8v"); DROP("cap-mmc-highspeed");
    /* Stock phandle targets a vendor-only node. Do not toggle the rail. */
    DROP("vmmc-supply"); DROP("vqmmc-supply");
    ret = of_changeset_apply(&changes);
    if (ret) goto fail;
    ret = device_attach(&pdev->dev);
    pr_info("r1_emmc: native MSDC0 attach=%d, max 26MHz SDR, retained LK pads/rails; no filesystem mounted\n", ret);
    put_device(&pdev->dev);
    return 0;
fail:
    pr_err("r1_emmc: setup failed %d\n", ret);
    put_device(&pdev->dev);
    return ret;
}
module_init(r1_emmc_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 MSDC0 native driver RAM-only DT adaptation");
