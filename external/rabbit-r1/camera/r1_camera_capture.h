/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>
#include <linux/sizes.h>
static struct platform_device *capture_dev;
static void *capture_mem;
static void *snapshot_mem;
static dma_addr_t capture_dma;
static struct dentry *capture_debug;

static bool dma_attempted;
static void __iomem *capture_larb;
static u32 capture_port;
static int capture_alloc(void) {
 int r;
 capture_dev=platform_device_register_simple("r1-camera-capture",-1,NULL,0);
 if(IS_ERR(capture_dev)){r=PTR_ERR(capture_dev);capture_dev=NULL;return r;}
 r=dma_coerce_mask_and_coherent(&capture_dev->dev,DMA_BIT_MASK(32));
 if(r)return r;
 capture_mem=dma_alloc_coherent(&capture_dev->dev,SZ_1M,&capture_dma,GFP_KERNEL);
 if(!capture_mem)return -ENOMEM;
 snapshot_mem=vzalloc(SZ_1M);if(!snapshot_mem)return -ENOMEM;
 memset(capture_mem,0xa5,SZ_1M);return 0;
}
static int capture_start(void) {
 struct device *ld;u32 port;
 int r=0;
 if(!capture_mem){r=capture_alloc();if(r)return r;}
 memset(capture_mem,0xa5,SZ_1M);
 /* CAM was off on entry and LARB3 must have no bound native owner.
  * Use the stock per-port physical-address mode only for CAM_SOC0, and
  * restore its exact state after verified DMA reset. Other ports unchanged. */
 ld=bus_find_device_by_name(&platform_bus_type,NULL,"1a002000.smi_larb3");
 if(!ld)return -ENODEV;
 if(ld->driver){put_device(ld);return -EBUSY;}put_device(ld);
 if(!capture_larb)capture_larb=ioremap(0x1a002000,0x1000);
 if(!capture_larb)return -ENOMEM;
 capture_port=readl(capture_larb+0x3b0);port=capture_port;
 writel(port&~1U,capture_larb+0x3b0);
 if(readl(capture_larb+0x3b0)&1){writel(capture_port,capture_larb+0x3b0);return -EIO;}
 /* Stock RAW10 packed VGA DMA parameters. Frame headers stay disabled. */
 writel(0x00010000,sv+0x110);writel(0,sv+0xe00);
 writel(capture_dma,sv+0x220);writel(0,sv+0x228);
 writel(799,sv+0x230);writel(479,sv+0x234);writel(0x01030320,sv+0x238);
 writel(0,sv+0x248);writel(0x80000080,sv+0x23c);
 writel(0x0004f004,sv+0x240);writel(0x0002f002,sv+0x244);
 writel(0x11,sv+0x4c);writel(0x61000000,sv+0x218);
 writel(0,sv+0x18);writel(~0U,sv+0x1c);
 writel(0x00018000,sv+0x110);
 writel(capture_dma,sv+0x118);writel(0,sv+0xe04);
 writel(readl(sv+0x2c)|1,sv+0x2c);
 dma_wmb();dma_attempted=true;
 writel(0x40000015,sv+0x10);writel(1,sv+0x504);
 return 0;
}
static int capture_stop(void) {
 int r;
 rmw(sv,0x504,1,0);rmw(sv,0x500,1,0);rmw(sv,0x10,BIT(4),0);
 writel(4,sv+0x20);writel(0,sv+0x20);writel(1,sv+0x20);
 r=pollbits(sv,0x20,3,3);if(!r)writel(0,sv+0x20);
 if(!r && capture_larb)writel(capture_port,capture_larb+0x3b0);
 if(r)pr_err("r1-camera: capture DMA reset=%d\n",r);
 return r;
}
static void capture_free(void) {
 debugfs_remove_recursive(capture_debug);
 vfree(snapshot_mem);
 if(capture_larb)iounmap(capture_larb);
 if(capture_mem)dma_free_coherent(&capture_dev->dev,SZ_1M,capture_mem,capture_dma);
 if(capture_dev)platform_device_unregister(capture_dev);
}
