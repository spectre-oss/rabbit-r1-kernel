// SPDX-License-Identifier: GPL-2.0-only
/* Native MT6765 V4L2 H.264 encoder. Hardware state is owned per stream. */
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/uaccess.h>
#include "r1_venc_regs.h"
#include <generated/rabbit-r1-encoder-modes.h>
#include "r1_vcodec_owner.h"
#define SOURCE_SIZE (1920 * 1088 * 3 / 2)
#define OUTPUT_SIZE (4 * 1024 * 1024)
struct buffer { void *cpu; dma_addr_t dma; size_t size; };
struct probe {
 struct device *dev;
 void __iomem *regs;
 struct regmap *syscon;
 struct clk_bulk_data clks[5];
 struct v4l2_device v4l2;
 struct video_device video;
 struct v4l2_m2m_dev *m2m;
 struct mutex lock;
 struct completion done;
 struct buffer work[9], input, output;
 int irq;
 u32 status, count;
 bool faulted;
};
static int allocate(struct probe *p, struct buffer *b, size_t size)
{
 b->size = size;
 b->cpu = dma_alloc_coherent(p->dev, size, &b->dma, GFP_KERNEL);
 if (!b->cpu) return -ENOMEM;
 if ((b->dma & 127) || b->dma + size - 1 > U32_MAX) return -ERANGE;
 memset(b->cpu, 0, size);
 return 0;
}
static void free_buffer(struct probe *p, struct buffer *b)
{
 if (b->cpu) dma_free_coherent(p->dev, b->size, b->cpu, b->dma);
 b->cpu = NULL;
}
static void wr(struct probe *p, u32 offset, u32 value)
{ writel(value, p->regs + offset); }
static void reset(struct probe *p)
{
 wr(p, R1_VENC_RESET, 0); wr(p, R1_VENC_RESET, 0);
 readl(p->regs + R1_VENC_RESET);
 wr(p, R1_VENC_RESET, 1); readl(p->regs + R1_VENC_RESET);
}
static void acknowledge(struct probe *p, u32 status)
{
 unsigned int bit;
 for (bit = 0; bit < 6; bit++)
  if (status & BIT(bit)) wr(p, R1_VENC_IRQ_ACK, BIT(bit));
}
static irqreturn_t interrupt(int irq, void *data)
{
 struct probe *p = data;
 u32 status = readl(p->regs + R1_VENC_IRQ_STATUS) & 0x3f;
 if (!status) return IRQ_NONE;
 acknowledge(p, status);
 p->status |= status;
 if (status & (R1_VENC_IRQ_FRAME | R1_VENC_IRQ_DRAM)) complete(&p->done);
 return IRQ_HANDLED;
}
static void pin_fault(struct probe *p);
static int power_on(struct probe *p)
{
 u32 value;
 int ret = pm_runtime_resume_and_get(p->dev);
 if (ret < 0) return ret;
 ret = clk_bulk_prepare_enable(ARRAY_SIZE(p->clks), p->clks);
 if (ret) goto put;
 ret = r1_vcodec_acquire(p->syscon, p, 1);
 if (ret) goto clocks;
 ret = regmap_read(p->syscon, 0x24, &value);
 if (!ret && !value) ret = regmap_write(p->syscon, 0x24, 1);
 if (!ret) return 0;
 if (r1_vcodec_release(p->syscon, p)) { pin_fault(p); return ret; }
clocks:
 clk_bulk_disable_unprepare(ARRAY_SIZE(p->clks), p->clks);
put:
 pm_runtime_put_sync(p->dev);
 return ret;
}
static void pin_fault(struct probe *p)
{
 /* Keep DMA memory, clocks and selector owned on unverified termination.
  * Module reference prevents unloading. Recovery requires a normal reboot.
  */
 if (p->faulted) return;
 p->faulted = true;
 __module_get(THIS_MODULE);
 dev_err(p->dev, "termination unverified: resources pinned until reboot\n");
}
static int power_off(struct probe *p)
{
 int ret = r1_vcodec_release(p->syscon, p);
 if (ret) { pin_fault(p); return ret; }
 clk_bulk_disable_unprepare(ARRAY_SIZE(p->clks), p->clks);
 pm_runtime_put_sync(p->dev);
 return 0;
}
static void address(struct probe *p, u32 reg, dma_addr_t dma)
{ wr(p, reg, (u32)(dma >> 4)); }
static int encode_frame(struct probe *p, struct buffer *work, const struct mode *mode, unsigned int frame, unsigned int bitrate)
{
 unsigned int i, phase = frame % 30, parity = phase & 1;
 long waited;
 int ret;
 if (p->faulted) { ret = -EIO; goto unlock; }
 p->count = 0;
 ret = power_on(p);
 if (ret) goto unlock;
 /* Hardware writes every byte in BS_COUNT; only that payload is copied out. */
 reinit_completion(&p->done); p->status = 0;
 reset(p); acknowledge(p, 0x3f);
 for (i = 0; i < ARRAY_SIZE(mode->setup); i++) wr(p, mode->setup[i][0], mode->setup[i][1]);
 wr(p, 0x48, 0x40000 | (bitrate / 1000));
 address(p, R1_VENC_RC_CODE, work[1].dma);
 address(p, R1_VENC_RC_DATA, work[0].dma);
 address(p, R1_VENC_BS_ADDRESS, p->output.dma);
 wr(p, R1_VENC_BS_CAPACITY, p->output.size >> 7);
 address(p, R1_VENC_INPUT_Y, p->input.dma);
 address(p, R1_VENC_INPUT_U, p->input.dma + mode->width * mode->stride_height);
 address(p, R1_VENC_INPUT_V, p->input.dma + mode->width * mode->stride_height * 5 / 4);
 wr(p, 0x10, phase ? (phase == 1 ? 0x108400 : 0x108500) :
    (0x10b406 | (((frame / 30) & 1) << 3)));
 wr(p, 0x14, frame); wr(p, 0x18, frame); wr(p, 0x1c, frame - phase);
 address(p, R1_VENC_REFERENCE_Y, work[parity ? 2 : 4].dma);
 address(p, R1_VENC_REFERENCE_C, work[parity ? 3 : 5].dma);
 address(p, R1_VENC_RECONSTRUCT_Y, work[parity ? 4 : 2].dma);
 address(p, R1_VENC_RECONSTRUCT_C, work[parity ? 5 : 3].dma);
 address(p, R1_VENC_MV_BUFFER0, work[parity ? 7 : 6].dma);
 address(p, R1_VENC_MV_BUFFER1, work[parity ? 6 : 7].dma);
 wr(p, R1_VENC_CONTROL, frame ? (mode->control & ~BIT(22)) | BIT(23) : mode->control);
 dma_wmb(); enable_irq(p->irq);
 wr(p, R1_VENC_START, 1); wr(p, R1_VENC_IRQ_ENABLE, R1_VENC_IRQ_FRAME);
 waited = wait_for_completion_timeout(&p->done, msecs_to_jiffies(1000));
 disable_irq(p->irq);
 wr(p, R1_VENC_IRQ_ENABLE, 0);
 dev_dbg(p->dev, "encode wait=%ld status=%#x count=%u reset=%#x\n", waited,
          p->status, readl(p->regs + R1_VENC_BS_COUNT), readl(p->regs + R1_VENC_RESET));
 if (!waited || !(p->status & R1_VENC_IRQ_FRAME) || (p->status & R1_VENC_IRQ_DRAM)) {
  reset(p); acknowledge(p, 0x3f); pin_fault(p);
  ret = waited ? -EIO : -ETIMEDOUT; goto unlock;
 }
 dma_rmb(); p->count = readl(p->regs + R1_VENC_BS_COUNT);
 ret = (!p->count || p->count > p->output.size) ? -EOVERFLOW : 0;
 if (ret) p->count = 0;
 reset(p); acknowledge(p, 0x3f);
 if (power_off(p)) ret = -EIO;
unlock:
 return ret;
}
#include "r1_venc_v4l2.inc"
static int probe_device(struct platform_device *pdev)
{
 static const char *const names[] = {"smi-common", "smi-comm0", "smi-comm1", "larb", "venc"};
 const struct firmware *fw;
 struct probe *p;
 int ret, i;
 p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
 if (!p) return -ENOMEM;
 p->dev = &pdev->dev; mutex_init(&p->lock); init_completion(&p->done);
 platform_set_drvdata(pdev, p);
 p->regs = devm_platform_ioremap_resource(pdev, 0);
 if (IS_ERR(p->regs)) return PTR_ERR(p->regs);
 p->syscon = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "mediatek,vcodec-syscon");
 if (IS_ERR(p->syscon)) return PTR_ERR(p->syscon);
 for (i = 0; i < ARRAY_SIZE(names); i++) p->clks[i].id = names[i];
 ret = devm_clk_bulk_get(p->dev, ARRAY_SIZE(p->clks), p->clks);
 if (ret) return ret;
 ret = dma_set_mask_and_coherent(p->dev, DMA_BIT_MASK(32));
 if (ret) return ret;
 p->irq = platform_get_irq(pdev, 0); if (p->irq < 0) return p->irq;
 ret = devm_request_irq(p->dev, p->irq, interrupt, IRQF_NO_AUTOEN, "r1-venc", p);
 if (ret) return ret;
 for (i = 0; i < ARRAY_SIZE(p->work); i++) {
  if (i != 1) continue; /* Immutable RC code is shared; state belongs to each stream. */
  ret = allocate(p, &p->work[i], modes[0].sizes[i]); if (ret) goto free;
 }
 ret = allocate(p, &p->input, SOURCE_SIZE); if (ret) goto free;
 ret = allocate(p, &p->output, OUTPUT_SIZE); if (ret) goto free;
 ret = request_firmware(&fw, "r1-venc-rc.bin", p->dev); if (ret) goto free;
 if (fw->size != p->work[1].size) ret = -EINVAL;
 else memcpy(p->work[1].cpu, fw->data, fw->size);
 release_firmware(fw); if (ret) goto free;
 pm_runtime_enable(p->dev);
 ret = register_video(p);
 if (ret) { pm_runtime_disable(p->dev); goto free; }
 dev_info(p->dev, "native H264 V4L2 encoder registered\n");
 return 0;
free:
 free_buffer(p, &p->input); free_buffer(p, &p->output);
 for (i = 0; i < ARRAY_SIZE(p->work); i++) free_buffer(p, &p->work[i]);
 return ret;
}
static void remove_device(struct platform_device *pdev)
{
 struct probe *p = platform_get_drvdata(pdev);
 int i;
 video_unregister_device(&p->video);
 v4l2_m2m_release(p->m2m); v4l2_device_unregister(&p->v4l2);
 pm_runtime_disable(p->dev);
 free_buffer(p, &p->input); free_buffer(p, &p->output);
 for (i = 0; i < ARRAY_SIZE(p->work); i++) free_buffer(p, &p->work[i]);
}
static const struct of_device_id matches[] = {{ .compatible = "rabbit,r1-venc" }, {}};
MODULE_DEVICE_TABLE(of, matches);
static struct platform_driver venc_driver = { .probe = probe_device, .remove = remove_device,
 .driver = { .name = "r1-venc", .of_match_table = matches, .suppress_bind_attrs = true } };
module_platform_driver(venc_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 native H264 V4L2 encoder");
