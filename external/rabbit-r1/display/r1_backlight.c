// SPDX-License-Identifier: GPL-2.0-only
/* R1 DISP_PWM backlight. MT6765 offsets/commit and off-duty=1 follow
 * vendor video/common/pwm10/ddp_pwm.c and mt6765/dispsys/ddp_reg_pq.h.
 * The MT6370 BLED/bias supply configuration remains board firmware state.
 */
#include <linux/backlight.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct r1_backlight {
	void __iomem *regs;
	struct clk_bulk_data *clocks;
	int num_clocks;
	struct backlight_device *bl;
};

static void update_bits(struct r1_backlight *r, u32 off, u32 mask, u32 value)
{
	writel((readl(r->regs + off) & ~mask) | (value & mask), r->regs + off);
}

static int r1_bl_update(struct backlight_device *bl)
{
	struct r1_backlight *r = bl_get_data(bl);
	u32 level = backlight_get_brightness(bl);
	u32 duty = level ? DIV_ROUND_CLOSEST(level * 1023, 255) : 1;

	update_bits(r, 0x1c, GENMASK(28, 16) | GENMASK(9, 0), (duty << 16) | 1023);
	update_bits(r, 0, BIT(0), level ? BIT(0) : 0);
	writel(1, r->regs + 0x0c);
	writel(0, r->regs + 0x0c);
	return 0;
}

static int r1_bl_get(struct backlight_device *bl)
{
	struct r1_backlight *r = bl_get_data(bl);
	u32 duty;

	if (!(readl(r->regs) & 1))
		return 0;
	duty = (readl(r->regs + 0x1c) >> 16) & 0x1fff;
	return min_t(u32, DIV_ROUND_CLOSEST(duty * 255, 1023), 255);
}

static const struct backlight_ops r1_bl_ops = {
	.update_status = r1_bl_update, .get_brightness = r1_bl_get,
};

static void disable_clocks(void *data)
{
	struct r1_backlight *r = data;
	clk_bulk_disable_unprepare(r->num_clocks, r->clocks);
}

static int r1_bl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct backlight_properties props = { .type = BACKLIGHT_RAW, .max_brightness = 255 };
	struct r1_backlight *r;
	int ret;

	r = devm_kzalloc(dev, sizeof(*r), GFP_KERNEL);
	if (!r) return -ENOMEM;
	r->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(r->regs)) return PTR_ERR(r->regs);
	r->num_clocks = devm_clk_bulk_get_all(dev, &r->clocks);
	if (r->num_clocks < 0) return dev_err_probe(dev, r->num_clocks, "PWM clocks\n");
	if (r->num_clocks != 2) return -EINVAL;
	ret = clk_bulk_prepare_enable(r->num_clocks, r->clocks);
	if (ret) return ret;
	ret = devm_add_action_or_reset(dev, disable_clocks, r);
	if (ret) return ret;
	/* Board policy: 1024 PWM ticks, divide by one. No mux/rail changes. */
	update_bits(r, 0x18, GENMASK(25, 16), 0);
	props.brightness = (readl(r->regs) & 1) ? 255 : 0;
	r->bl = devm_backlight_device_register(dev, "lcd-backlight", dev, r, &r1_bl_ops, &props);
	if (IS_ERR(r->bl)) return PTR_ERR(r->bl);
	platform_set_drvdata(pdev, r);
	ret = backlight_update_status(r->bl);
	dev_info(dev, "native 0..255 DISP_PWM backlight, clocks owned\n");
	return ret;
}

static const struct of_device_id r1_bl_match[] = {
	{ .compatible = "rabbit,r1-disp-backlight" }, {}
};
MODULE_DEVICE_TABLE(of, r1_bl_match);
static struct platform_driver r1_bl_driver = {
	.probe = r1_bl_probe,
	.driver = { .name = "r1-disp-backlight", .of_match_table = r1_bl_match },
};
module_platform_driver(r1_bl_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 native DISP_PWM backlight");

/* Vendor register reference: Copyright (c) 2019 MediaTek Inc.
 * ddp_pwm.c and ddp_reg_pq.h, SPDX-License-Identifier: GPL-2.0.
 */
