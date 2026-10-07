// SPDX-License-Identifier: GPL-2.0-only
/*
 * Rabbit R1 framebuffer: map the LK leftover scanout.
 * Does not program DSI / ST7701. That is a later panel driver.
 *
 * Address and 480x640 come from stock reserved mblock-14-framebuffer
 * (UART: 0x7efe0000) and the vendor LCM (RGB888, 2-lane video).
 *
 * LK leaves OVL0 L0 = logo FB and L1 = Orange State (next 480x640 page).
 * L1 SRC_SIZE is a short banner. A 24-row CPU fill looked like a white
 * strip; full white needs L0 opaque + ROI/SRC_SIZE 480x640 + L1 off.
 */
#include <linux/aperture.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#define R1FB_W		480
#define R1FB_H		640
#define R1FB_BPP	32
#define R1FB_STRIDE	(R1FB_W * 4)
#define R1FB_SIZE	((R1FB_H << 16) | R1FB_W)

/* mt6765 ddp_reg_ovl.h — OVL0 only. ovl0_2l @ 0x1400c000 is not in the path. */
#define OVL0_PA		0x1400b000UL
#define OVL_EN		0x00c
#define OVL_ROI_SIZE	0x020
#define OVL_ROI_BGCLR	0x028
#define OVL_SRC_CON	0x02c
#define OVL_L0_CON	0x030
#define OVL_L0_SRC_SIZE	0x038
#define OVL_L0_OFFSET	0x03c
#define OVL_L0_PITCH	0x044
#define OVL_L0_CLIP	0x04c
#define OVL_L0_ADDR	0xf40
#define OVL_L1_CON	0x050
#define OVL_L1_SRC_SIZE	0x058
#define OVL_L1_ADDR	0xf60
#define OVL_L3_CON	0x090
#define OVL_L3_SRC_SIZE	0x098
#define OVL_L3_ADDR	0xfa0
#define OVL_RDMA0_CTRL	0x0c0

static const struct fb_fix_screeninfo r1fb_fix = {
	.id		= "r1fb",
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= FB_VISUAL_TRUECOLOR,
	.accel		= FB_ACCEL_NONE,
	.line_length	= R1FB_STRIDE,
};

static int r1fb_setcolreg(unsigned regno, unsigned red, unsigned green,
			  unsigned blue, unsigned transp, struct fb_info *info);
static int r1fb_blank(int blank, struct fb_info *info);

static const struct fb_ops r1fb_ops = {
	.owner		= THIS_MODULE,
	FB_DEFAULT_IOMEM_OPS,
	.fb_setcolreg	= r1fb_setcolreg,
	.fb_blank	= r1fb_blank,
};

struct r1fb_par {
	u32 palette[16];
	struct fb_info *info;
	void __iomem *ovl;
	int blanked;
	unsigned long ignore_until;
	unsigned long last_toggle;
};

static struct r1fb_par *r1fb_par_g;

static void r1fb_ovl_show(struct r1fb_par *par, int on)
{
	if (!par->ovl)
		return;
	if (on) {
		writel(R1FB_SIZE, par->ovl + OVL_ROI_SIZE);
		writel(0xff000000, par->ovl + OVL_ROI_BGCLR);
		writel(0x1, par->ovl + OVL_SRC_CON);
	} else {
		writel(0, par->ovl + OVL_SRC_CON);
		writel(0xff000000, par->ovl + OVL_ROI_BGCLR);
	}
	wmb();
	par->blanked = !on;
}

static void r1fb_ovl_takeover(struct device *dev, struct r1fb_par *par, u32 fbpa)
{
	void __iomem *r = ioremap(OVL0_PA, 0x1000);

	if (!r) {
		dev_err(dev, "ovl0 ioremap failed\n");
		return;
	}
	par->ovl = r;

	dev_info(dev,
		 "ovl0 before EN=%08x ROI=%08x BG=%08x SRC=%08x L0con=%08x L0sz=%08x L0pit=%08x L0off=%08x L0=%08x L1con=%08x L1sz=%08x L1=%08x L3con=%08x L3sz=%08x L3=%08x\n",
		 readl(r + OVL_EN), readl(r + OVL_ROI_SIZE),
		 readl(r + OVL_ROI_BGCLR), readl(r + OVL_SRC_CON),
		 readl(r + OVL_L0_CON), readl(r + OVL_L0_SRC_SIZE),
		 readl(r + OVL_L0_PITCH), readl(r + OVL_L0_OFFSET),
		 readl(r + OVL_L0_ADDR), readl(r + OVL_L1_CON),
		 readl(r + OVL_L1_SRC_SIZE), readl(r + OVL_L1_ADDR),
		 readl(r + OVL_L3_CON), readl(r + OVL_L3_SRC_SIZE),
		 readl(r + OVL_L3_ADDR));

	/* Full 480x640 ROI. Black BG if a layer is still off/transparent. */
	writel(R1FB_SIZE, r + OVL_ROI_SIZE);
	writel(0xff000000, r + OVL_ROI_BGCLR);

	/* L0 opaque (AEN=0, APHA=0xff), full size, leftover FB PA. */
	writel((readl(r + OVL_L0_CON) & ~0x1ff) | 0xff, r + OVL_L0_CON);
	writel(R1FB_SIZE, r + OVL_L0_SRC_SIZE);
	writel(0, r + OVL_L0_OFFSET);
	writel(0, r + OVL_L0_CLIP);
	writel((readl(r + OVL_L0_PITCH) & 0xffff0000) | R1FB_STRIDE,
	       r + OVL_L0_PITCH);
	writel(fbpa, r + OVL_L0_ADDR);
	writel(1, r + OVL_RDMA0_CTRL);
	/* L1 = LK Orange State banner. L3 = stock ASSERT_LAYER/font. Off. */
	writel(0x1, r + OVL_SRC_CON);
	wmb();

	dev_info(dev,
		 "ovl0 after  EN=%08x ROI=%08x BG=%08x SRC=%08x L0con=%08x L0sz=%08x L0pit=%08x L0=%08x\n",
		 readl(r + OVL_EN), readl(r + OVL_ROI_SIZE),
		 readl(r + OVL_ROI_BGCLR), readl(r + OVL_SRC_CON),
		 readl(r + OVL_L0_CON), readl(r + OVL_L0_SRC_SIZE),
		 readl(r + OVL_L0_PITCH), readl(r + OVL_L0_ADDR));
}

static int r1fb_setcolreg(unsigned regno, unsigned red, unsigned green,
			  unsigned blue, unsigned transp, struct fb_info *info)
{
	u32 *pal = info->pseudo_palette;

	if (regno >= 16)
		return -EINVAL;
	pal[regno] =
		((red >> 8) << info->var.red.offset) |
		((green >> 8) << info->var.green.offset) |
		((blue >> 8) << info->var.blue.offset);
	return 0;
}

static int r1fb_blank(int blank, struct fb_info *info)
{
	struct r1fb_par *par = info->par;

	r1fb_ovl_show(par, blank == FB_BLANK_UNBLANK);
	return 0;
}

static void r1fb_input_event(struct input_handle *handle, unsigned int type,
			     unsigned int code, int value)
{
	struct r1fb_par *par = r1fb_par_g;

	/* Userspace owns PTT actions. Do not also toggle OVL here. */
	(void)handle;
	(void)type;
	(void)code;
	(void)value;
	(void)par;
}

static int r1fb_input_connect(struct input_handler *handler,
			      struct input_dev *dev,
			      const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "r1fb-pwr";
	ret = input_register_handle(handle);
	if (ret)
		goto err;
	ret = input_open_device(handle);
	if (ret) {
		input_unregister_handle(handle);
		goto err;
	}
	pr_info("r1fb: PTT on %s (userspace owns click)\n", dev->name);
	return 0;
err:
	kfree(handle);
	return ret;
}

static void r1fb_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id r1fb_input_ids[] = {
	{
		.flags	= INPUT_DEVICE_ID_MATCH_EVBIT |
			  INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit	= { BIT_MASK(EV_KEY) },
		.keybit	= { [BIT_WORD(KEY_POWER)] = BIT_MASK(KEY_POWER) },
	},
	{ }
};

static struct input_handler r1fb_input_handler = {
	.event		= r1fb_input_event,
	.connect	= r1fb_input_connect,
	.disconnect	= r1fb_input_disconnect,
	.name		= "r1fb",
	.id_table	= r1fb_input_ids,
};

static int r1fb_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct fb_info *info;
	struct r1fb_par *par;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	info = framebuffer_alloc(sizeof(*par), &pdev->dev);
	if (!info)
		return -ENOMEM;
	par = info->par;
	platform_set_drvdata(pdev, info);

	info->fix = r1fb_fix;
	info->fix.smem_start = res->start;
	info->fix.smem_len = resource_size(res);

	info->var.xres = R1FB_W;
	info->var.yres = R1FB_H;
	info->var.xres_virtual = R1FB_W;
	info->var.yres_virtual = R1FB_H;
	info->var.bits_per_pixel = R1FB_BPP;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.vmode = FB_VMODE_NONINTERLACED;
	/* LK / Track 0 was BGR (x8b8g8r8). */
	info->var.red.offset = 0;
	info->var.red.length = 8;
	info->var.green.offset = 8;
	info->var.green.length = 8;
	info->var.blue.offset = 16;
	info->var.blue.length = 8;

	info->fbops = &r1fb_ops;
	info->pseudo_palette = par->palette;

	info->screen_base = ioremap_wc(info->fix.smem_start, info->fix.smem_len);
	if (!info->screen_base) {
		framebuffer_release(info);
		return -ENOMEM;
	}
	info->screen_size = info->fix.smem_len;

	/* Black field; fbcon vt.color=0x07 is white text on black. */
	memset_io(info->screen_base, 0x00, info->fix.smem_len);
	wmb();
	r1fb_ovl_takeover(&pdev->dev, par, (u32)info->fix.smem_start);

	ret = devm_aperture_acquire_for_platform_device(pdev, res->start,
							resource_size(res));
	if (ret)
		goto unmap;

	ret = register_framebuffer(info);
	if (ret)
		goto unmap;

	par->info = info;
	par->ignore_until = jiffies + 2 * HZ;
	r1fb_par_g = par;
	ret = input_register_handler(&r1fb_input_handler);
	if (ret) {
		r1fb_par_g = NULL;
		dev_warn(&pdev->dev, "PTT handler %d (panel still on)\n", ret);
	}

	dev_info(&pdev->dev, "fb%d: r1fb %dx%d @ 0x%lx (fbcon, userspace PTT)\n",
		 info->node, R1FB_W, R1FB_H, (unsigned long)res->start);
	return 0;

unmap:
	if (par->ovl)
		iounmap(par->ovl);
	iounmap(info->screen_base);
	framebuffer_release(info);
	return ret;
}

static void r1fb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);
	struct r1fb_par *par = info->par;

	if (r1fb_par_g == par) {
		input_unregister_handler(&r1fb_input_handler);
		r1fb_par_g = NULL;
	}
	unregister_framebuffer(info);
	if (par->ovl)
		iounmap(par->ovl);
	iounmap(info->screen_base);
	framebuffer_release(info);
}

static const struct of_device_id r1fb_of[] = {
	{ .compatible = "rabbit,r1-fb" },
	{ }
};
MODULE_DEVICE_TABLE(of, r1fb_of);

static struct platform_driver r1fb_driver = {
	.probe = r1fb_probe,
	.remove = r1fb_remove,
	.driver = {
		.name = "r1fb",
		.of_match_table = r1fb_of,
	},
};

static int __init r1fb_init(void)
{
	struct device_node *np;
	int ret;

	ret = platform_driver_register(&r1fb_driver);
	if (ret)
		return ret;
	/* /chosen is not a bus; simplefb used to work because LK + of
	 * still instantiated that node. Create ours explicitly. */
	np = of_find_compatible_node(NULL, NULL, "rabbit,r1-fb");
	if (np) {
		of_platform_device_create(np, NULL, NULL);
		of_node_put(np);
	} else {
		/* Stock DTB patch missing: still take over leftover LK FB. */
		static struct resource fbmem = {
			.start	= 0x7efe0000UL,
			.end	= 0x7efe0000UL + 0x420000UL - 1,
			.flags	= IORESOURCE_MEM,
		};
		pr_info("r1fb: no DT node, register leftover 0x7efe0000\n");
		platform_device_register_simple("r1fb", 0, &fbmem, 1);
	}
	return 0;
}
static void __exit r1fb_exit(void)
{
	platform_driver_unregister(&r1fb_driver);
}
module_init(r1fb_init);
module_exit(r1fb_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 LK leftover framebuffer");
