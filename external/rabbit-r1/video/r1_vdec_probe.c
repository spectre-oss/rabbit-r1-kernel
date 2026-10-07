// SPDX-License-Identifier: GPL-2.0-only
/* Rabbit R1 stateless V4L2 H.264 decoder and bounded bring-up interface. */
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include "r1_vcodec_owner.h"
#include "r1_h264_regs.h"
#include "r1_h264_weight_span.h"
#include "r1_h264_reorder_check.h"
#include <media/media-device.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>
#include <linux/vmalloc.h>
static bool video;
module_param(video, bool, 0400);

static unsigned int fixture_width = 320, fixture_height = 240;
module_param(fixture_width, uint, 0400);
module_param(fixture_height, uint, 0400);
static bool compressed, sequence;
module_param(sequence, bool, 0400);
module_param(compressed, bool, 0400);
MODULE_PARM_DESC(compressed, "Use the fixed x264 compressed IDR fixture");

#define FETCH _IO('D', 0)
#define DECODE _IO('D', 1)
#include "r1_h264_limits.h"
#define INPUT_SIZE SZ_2M
static const size_t work_sizes[] = {R1_H264_PIXEL_STORAGE, R1_H264_MV_STORAGE, SZ_64K, SZ_128K, SZ_32K, [5 ... 36] = R1_H264_PIXEL_STORAGE, [37 ... 52] = R1_H264_MV_STORAGE};
struct decoder {
 struct device *dev, *dma_dev;
 struct module *dma_owner;
 struct clk *clks[5];
 struct regmap *syscon;
 void __iomem *regs;
 void *input;
 dma_addr_t input_dma;
 struct mutex lock;
 struct miscdevice misc;
 struct media_device media;
 struct v4l2_device v4l2;
 struct video_device video_dev;
 struct v4l2_m2m_dev *m2m;
 bool pinned, decoded;
 unsigned int frame;
 void *work[ARRAY_SIZE(work_sizes)];
 dma_addr_t work_dma[ARRAY_SIZE(work_sizes)];
 struct sg_table *pixels_sgt;
 struct completion done;
 int irq;
 u32 irq_status;
};
/* Tiled pixels require contiguous IOVA, not scarce physically contiguous CMA. */
static void pixels_cpu(struct device *dev,void *ptr,struct sg_table *sgt)
{
 dma_sync_sgtable_for_cpu(dev,sgt,DMA_BIDIRECTIONAL);
 invalidate_kernel_vmap_range(ptr,work_sizes[0]);
}
static void pixels_device(struct device *dev,void *ptr,struct sg_table *sgt)
{
 flush_kernel_vmap_range(ptr,work_sizes[0]);
 dma_sync_sgtable_for_device(dev,sgt,DMA_BIDIRECTIONAL);
}
static void pixels_free(struct device *dev,void *ptr,struct sg_table *sgt)
{
 dma_vunmap_noncontiguous(dev,ptr);
 dma_free_noncontiguous(dev,work_sizes[0],sgt,DMA_BIDIRECTIONAL);
}
static void *pixels_alloc(struct device *dev,dma_addr_t *addr,struct sg_table **table)
{
 struct sg_table *sgt;
 void *ptr;
 sgt=dma_alloc_noncontiguous(dev,work_sizes[0],DMA_BIDIRECTIONAL,GFP_KERNEL,DMA_ATTR_ALLOC_SINGLE_PAGES);
 if(!sgt)return NULL;
 ptr=dma_vmap_noncontiguous(dev,work_sizes[0],sgt);
 if(!ptr){dma_free_noncontiguous(dev,work_sizes[0],sgt,DMA_BIDIRECTIONAL);return NULL;}
 *addr=sg_dma_address(sgt->sgl);*table=sgt;
 pixels_cpu(dev,ptr,sgt);
 return ptr;
}
static void wr(struct decoder *d, u32 reg, u32 value)
{ writel(value, d->regs + reg); }
static u32 rd(struct decoder *d, u32 reg)
{ return readl(d->regs + reg); }
/* Fixed first-frame fixture programming. */
#include "r1_vdec_hw.inc"
#include "r1_vdec_pcm.inc"
static void pin(struct decoder *d)
{
 if (!READ_ONCE(d->pinned)) {
  WRITE_ONCE(d->pinned, true);
  __module_get(THIS_MODULE);
  dev_err(d->dev, "DMA termination/ownership unverified; resources pinned until reboot\n");
 }
}
static int power_on(struct decoder *d)
{
 int ret, i;
 ret = pm_runtime_resume_and_get(d->dma_dev);
 if (ret < 0) return ret;
 for (i = 0; i < ARRAY_SIZE(d->clks); i++) {
  ret = clk_prepare_enable(d->clks[i]);
  if (ret) goto undo;
 }
 ret = r1_vcodec_acquire(d->syscon, d, 2);
 if (!ret) return 0;
undo:
 while (i--) clk_disable_unprepare(d->clks[i]);
 pm_runtime_put_sync(d->dma_dev);
 return ret;
}
static int power_off(struct decoder *d)
{
 int i, ret = r1_vcodec_release(d->syscon, d);
 if (ret) { pin(d); return ret; }
 for (i = ARRAY_SIZE(d->clks); i--;) clk_disable_unprepare(d->clks[i]);
 pm_runtime_put_sync(d->dma_dev);
 return 0;
}
static int stop_fetch(struct decoder *d)
{
 u32 value;
 int ret;
 /* Stock vdec_break handshake. Never free DMA on an unverified stop. */
 wr(d, 0x5100, 1);
 ret = readl_poll_timeout(d->regs + 0x5104, value,
                          (value & 0x11) == 0x11, 10, 100000);
 if (ret) { pin(d); return ret; }
 wr(d, 0x108, 1); wr(d, 0x108, 0);
 rd(d, 0x108);
 return 0;
}
static int initialize(struct decoder *d)
{
 u32 value;
 int ret;
 /* H264_HAL_ResetHW: conditional VLD idle check, with a real time limit. */
 ret = readl_poll_timeout(d->regs + 0xf4, value,
                         !(value & BIT(15)) || (value & BIT(0)), 10, 100000);
 if (ret) return ret;
 wr(d, 0x108, 0x101); wr(d, 0x5084, 2);
 wr(d, 0x5000, (rd(d, 0x5000) & 0xff400000) | 0x801ff0);
 rd(d, 0x5000);
 wr(d, 0x50c8, 0x1f7); wr(d, 0x50cc, 0x53e30182);
 wr(d, 0x5178, 0xffffffe1); wr(d, 0x50f4, 0);
 wr(d, 0x5084, 2); wr(d, 0x108, 0);
 wr(d, 0x50ec, 1); wr(d, 0x5004, 0x10);
 wr(d, 0x5160, 0x65ff10);
 return 0;
}
static irqreturn_t interrupt(int irq, void *data)
{
 struct decoder *d = data;
 u32 status = rd(d, 0x50a4);
 if (!(status & BIT(16))) return IRQ_NONE;
 d->irq_status = status;
 wr(d, 0x50a4, status | 0x11);
 wr(d, 0x50a4, rd(d, 0x50a4) & ~0x10U);
 complete(&d->done);
 return IRQ_HANDLED;
}
static void skip_header(struct decoder *d,unsigned int bits)
{
 while(bits){unsigned int count=min(bits,32U);rd(d,0x2000+count*4);bits-=count;}
}
static int decode_job(struct decoder *d, const struct r1_h264_job *job,
                      unsigned int header_bits,unsigned int weight_start,unsigned int weight_end,unsigned int input_size)
{
 u32 value;
 long waited;
 int ret;
 unsigned int reorder_start=0,reorder_end=0;
 bool inter=job->slice.slice_type!=V4L2_H264_SLICE_TYPE_I;
 if(inter && r1_h264_reorder_span((u8 *)d->input+4,input_size-4,&job->sps,&job->pps,&job->slice,&job->dec,&reorder_start,&reorder_end,NULL))return -EINVAL;
 h264_program(d,job,d->work_dma[0],d->work_dma[1],d->work_dma[2],d->work_dma[3]);
 skip_header(d,inter?reorder_start:header_bits);
 if (job->picture_has_p || job->slice.slice_type == V4L2_H264_SLICE_TYPE_P) {
  /* Stock InitPRefList: frame picture, MaxFrameNum16, current frame1. */
  wr(d, 0x1564, 0); wr(d, 0x2228, 1);
  ret = readl_poll_timeout(d->regs + 0x2228, value, !(value & 1), 10, 100000);
  if (ret) return ret;
  wr(d, 0x20a0, 1U << (job->sps.log2_max_frame_num_minus4 + 4));
  wr(d, 0x20a4, job->dec.frame_num);
  for (value = 0; value < 32; value++) {
   wr(d, 0x13dc + value * 4, d->work_dma[0]);
   wr(d, 0x3000 + value * 4, 0);
  }
  for (value=0;value<job->reference_count;value++) {
   unsigned int slot=job->p_slot[value];
   const struct v4l2_h264_dpb_entry *ref=&job->dec.dpb[job->default_lists[0][slot]];
   struct r1_h264_ref_regs r=r1_h264_pack_reference(value,job->reference_identity[0][slot],(video?job->reference_pixels[0][slot]:d->work_dma[5+slot]),ref);
   if(!(ref->flags&V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM))r.frame_num=r1_h264_p_reference_number(r.frame_num,job->dec.frame_num,job->sps.log2_max_frame_num_minus4);
   wr(d,r.mc_offset,r.address);wr(d,r.frame_offset,r.frame_num);
   wr(d,r.mv_offset,r.top);wr(d,r.mv_offset+4,r.bottom);
  }
 }
 if(job->picture_has_b) {
  unsigned int n=job->reference_count;
  wr(d,0x1568,0);wr(d,0x156c,0);
  for(value=0;value<32;value++) {
   wr(d,0x145c+value*4,d->work_dma[0]);wr(d,0x14dc+value*4,d->work_dma[0]);
   wr(d,0x3080+value*4,0);wr(d,0x3100+value*4,0);
  }
  for(value=0;value<n;value++) {
   unsigned int a=value;
   unsigned int b=value;
   unsigned int ia=job->default_lists[0][a],ib=job->default_lists[1][b],k;
   struct r1_h264_bref_regs r=r1_h264_pack_b_reference(value,job->reference_identity[0][a],job->reference_identity[1][b],
    job->reference_pixels[0][a],job->reference_pixels[1][b],job->reference_mv[b],&job->dec.dpb[ia],&job->dec.dpb[ib]);
   for(k=0;k<10;k++)wr(d,r.offset[k],r.value[k]);
  }
 }
 if(inter) {
  wr(d,0x222c,1);
  ret=readl_poll_timeout(d->regs+0x222c,value,value,10,100000);
  if(ret)return ret;
  if(V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(&job->pps,&job->slice)) {
   u32 old;
   if(weight_start!=reorder_end)return -EINVAL;
   old=rd(d,0x50f0);wr(d,0x50f0,old|1);wr(d,0x2230,1);
   ret=readl_poll_timeout(d->regs+0x2230,value,value,10,100000);
   wr(d,0x20f0,old&~1U);if(ret)return ret;
   skip_header(d,header_bits-weight_end);
  } else skip_header(d,header_bits-reorder_end);
 }
 if(job->pps.flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE)wr(d,0x2234,1);
 wr(d,0x3200,min(job->dec.top_field_order_cnt,job->dec.bottom_field_order_cnt)&0x3ffff);
 wr(d,0x3204,job->dec.top_field_order_cnt&0x3ffff);
 wr(d,0x3208,job->dec.bottom_field_order_cnt&0x3ffff);
 ret = readl_poll_timeout(d->regs + 0x225c, value, !(value & BIT(25)), 10, 100000);
 if (ret) return ret;
 reinit_completion(&d->done); d->irq_status = 0;
 wr(d, 0x50a4, rd(d, 0x50a4) | 0x11);
 wr(d, 0x50a4, rd(d, 0x50a4) & ~0x10U);
 dma_wmb(); enable_irq(d->irq);
 wr(d, 0x2238, 1);
 waited = wait_for_completion_timeout(&d->done, msecs_to_jiffies(1000));
 disable_irq(d->irq);
 dma_rmb();
 dev_dbg(d->dev, "decode wait=%ld irq=%#x finish=%#x error=%#x mb=%#x\n",
          waited, d->irq_status, rd(d, 0x2274),
          rd(d, 0x2280) & rd(d, 0x2284), rd(d, 0x20a4));
 if (!waited || !rd(d, 0x2274)) {dev_err(d->dev,"decode timeout irq=%#x finish=%#x\n",d->irq_status,rd(d,0x2274));return -ETIMEDOUT;}
 if (rd(d, 0x2280) & rd(d, 0x2284)) {dev_err(d->dev,"decode hardware error=%#x\n",rd(d,0x2280)&rd(d,0x2284));return -EIO;}
 return 0;
}
/* Caller owns d->lock and validated controls; references are kernel DMA only.
 * Reference snapshots are copied to decoder-owned DMA storage before submission. */
static int submit_job(struct decoder *d, const struct r1_h264_job *job,
                      const u8 *data, size_t size, unsigned int header_bits,
                      bool decode)
{
 u32 value, header, payload;
 unsigned int weight_start=0,weight_end=0;
 int ret, cleanup, i;
 bool output_handed=false;
 if (READ_ONCE(d->pinned)) return -EIO;
 ret = validate_job(job);
 if (ret) return ret;
 if (size < 5 || size > INPUT_SIZE - 256 || memcmp(data, "\0\0\1", 3) ||
     (data[3] & 0x80) || ((data[3] & 31) != 1 && (data[3] & 31) != 5) ||
     header_bits > (size - 4) * 8) return -EINVAL;
 if(V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(&job->pps,&job->slice) &&
    (r1_h264_weight_span(data+4,size-4,&job->sps,&job->pps,&job->slice,&job->dec,
                         &job->weights,&weight_start,&weight_end) || weight_end>header_bits))return -EINVAL;
 d->decoded = false;
 for (i = 0; i < 5; i++) memset(d->work[i], 0, work_sizes[i]);
 memset(d->input, 0, INPUT_SIZE);
 memcpy(d->input, data, size);
 ret = power_on(d);
 if (ret) return ret;
 ret = initialize(d);
 if (ret) { pin(d); return ret; }
 /* InitH264BarrelShift plus aligned read pointer; all DMA stays in input. */
 wr(d, 0xc0, 0x40000); wr(d, 0x2084, 0xae00e007);
 wr(d, 0x854, rd(d, 0x854) | 1); wr(d, 0x1024, 6);
 wr(d, 0xb4, d->input_dma >> 6);
 wr(d, 0xb8, (d->input_dma + INPUT_SIZE) >> 6);
 wr(d, 0xec, rd(d, 0xec) | BIT(28));
 /* Stock prediction allocation: 32 KiB. */
 wr(d, 0x828, d->work_dma[4]);
 wr(d, 0x228c, 1); wr(d, 0x228c, 0);
 wr(d, 0x110, rd(d, 0x110) | 1);
 wr(d, 0xb0, d->input_dma); wr(d, 0xb0, d->input_dma);
 wr(d, 0x110, d->input_dma + INPUT_SIZE);
 wr(d, 0x228c, 1); wr(d, 0x108, 0x100); wr(d, 0x108, 0);
 wr(d, 0xcc, 0);
 dma_wmb(); wr(d, 0x8c, 0x100000);
 ret = readl_poll_timeout(d->regs + 0xe8, value, value & 1, 10, 100000);
 if (!ret) {
  wr(d, 0x8c, 0x800000); wr(d, 0x228c, 0);
  header = rd(d, 0x2000);
  rd(d, 0x2080); /* consume 32 bits: start code plus NAL header */
  payload = rd(d, 0x2000);
  dev_dbg(d->dev, "fetch ready=%#x header=%#x payload=%#x\n", value, header, payload);
  if (header != (0x100 | data[3]) || payload >> 16 !=
      (((u8 *)d->input)[4] << 8 | ((u8 *)d->input)[5])) ret = -EBADMSG;
 } else dev_err(d->dev, "fetch timeout VLD+e8=%#x\n", value);
 if (!ret && decode) {
  pixels_device(d->dma_dev,d->work[0],d->pixels_sgt);
  output_handed=true;
  ret = decode_job(d, job, header_bits,weight_start,weight_end,size);
 }
 cleanup = stop_fetch(d);
 if (cleanup) return cleanup;
 if(output_handed)pixels_cpu(d->dma_dev,d->work[0],d->pixels_sgt);
 cleanup = power_off(d);
 if (!ret && !cleanup && decode) d->decoded = true;
 return cleanup ? cleanup : ret;
}

static int fetch(struct decoder *d, bool decode)
{
 const struct firmware *fw;
 char fixture_name[64];
 struct r1_h264_job *job;
 unsigned int bits = sequence && d->frame ? 18 : (compressed || sequence) ? 24 : 16;
 int ret;
 /* Existing allocations bound this dimension experiment to smaller frames.
  * Non-PCM vectors retain their independently parsed original dimensions. */
 if (!fixture_width || !fixture_height || fixture_width > 1280 || fixture_height > 720 ||
     (fixture_width & 15) || (fixture_height & 15) ||
     ((compressed || sequence) && (fixture_width != 320 || fixture_height != 240))) return -EINVAL;
 if (sequence && d->frame > 1) return -ENODATA;
 if (sequence && d->frame && !d->decoded) return -EIO;
 if (sequence && d->frame) memcpy(d->work[5], d->work[0], work_sizes[0]);
 d->decoded = false;
 if (READ_ONCE(d->pinned)) return -EIO;
 snprintf(fixture_name, sizeof(fixture_name), "r1-vdec-pcm-%ux%u.bin", fixture_width, fixture_height);
 ret = request_firmware(&fw, (fixture_width != 320 || fixture_height != 240) ? fixture_name : sequence ? (d->frame ? "r1-vdec-ip-1.bin" : "r1-vdec-ip-0.bin") :
                        compressed ? "r1-vdec-compressed-idr.bin" : "r1-vdec-pcm-idr.bin", d->dev);
 if (ret) return ret;
 job=kzalloc(sizeof(*job),GFP_KERNEL);
 if(!job){release_firmware(fw);return -ENOMEM;}
 fixture_job(d,job);
 job->slice.header_bit_size=bits+8;
 if(sequence && d->frame) {
  job->reference_count=1;
  job->default_lists[0][0]=0;
  job->dec.dpb[0].flags=V4L2_H264_DPB_ENTRY_FLAG_VALID|V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
  job->dec.dpb[0].fields=V4L2_H264_FRAME_REF;
  job->slice.ref_pic_list0[0].fields=V4L2_H264_FRAME_REF;
 }
 ret = submit_job(d, job, fw->data, fw->size, bits, decode);
 kfree(job);
 release_firmware(fw);
 if (!ret && decode && sequence) d->frame++;
 return ret;
}

static long ioctl(struct file *file, unsigned int command, unsigned long arg)
{
 struct decoder *d = container_of(file->private_data, struct decoder, misc);
 int ret;
 if (command != FETCH && command != DECODE) return -ENOTTY;
 if (mutex_lock_interruptible(&d->lock)) return -ERESTARTSYS;
 ret = fetch(d, command == DECODE);
 mutex_unlock(&d->lock);
 return ret;
}
static ssize_t read_output(struct file *file, char __user *buf, size_t size, loff_t *pos)
{
 struct decoder *d = container_of(file->private_data, struct decoder, misc);
 ssize_t ret;
 if (mutex_lock_interruptible(&d->lock)) return -ERESTARTSYS;
 ret = d->decoded ? simple_read_from_buffer(buf, size, pos, d->work[0], work_sizes[0]) : -ENODATA;
 mutex_unlock(&d->lock);
 return ret;
}
static const struct file_operations fops = {
 .read = read_output, .owner = THIS_MODULE, .unlocked_ioctl = ioctl, .compat_ioctl = ioctl,
 .llseek = noop_llseek,
};
#include "r1_vdec_v4l2.inc"

static void release_resources(struct decoder *d)
{
 int i;
 for (i = 0; i < ARRAY_SIZE(d->work); i++)
  if (d->work[i]) {
   if(i)dma_free_coherent(d->dma_dev, work_sizes[i], d->work[i], d->work_dma[i]);
   else pixels_free(d->dma_dev,d->work[0],d->pixels_sgt);
  }
 if (d->input) dma_free_coherent(d->dma_dev, INPUT_SIZE, d->input, d->input_dma);
 for (i = ARRAY_SIZE(d->clks); i--;) if (!IS_ERR_OR_NULL(d->clks[i])) clk_put(d->clks[i]);
 if (d->dma_owner) module_put(d->dma_owner);
 if (d->dma_dev) put_device(d->dma_dev);
}
static int probe(struct platform_device *pdev)
{
 static const char *const shared[] = {"smi-common", "smi-comm0", "smi-comm1", "larb"};
 struct of_phandle_args clock = { .args_count = 1, .args = {3} };
 struct decoder *d;
 int i, ret = -ENODEV;
 d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
 if (!d) return -ENOMEM;
 d->dev = &pdev->dev; mutex_init(&d->lock); init_completion(&d->done);
 d->dma_dev = bus_find_device_by_name(&platform_bus_type, NULL, "17020000.venc");
 if (!d->dma_dev) return -EPROBE_DEFER;
 device_lock(d->dma_dev);
 if (d->dma_dev->driver && !strcmp(d->dma_dev->driver->name, "r1-venc") &&
     d->dma_dev->driver->suppress_bind_attrs && try_module_get(d->dma_dev->driver->owner))
  d->dma_owner = d->dma_dev->driver->owner;
 device_unlock(d->dma_dev);
 if (!d->dma_owner) goto fail;
 for (i = 0; i < ARRAY_SIZE(shared); i++) {
  d->clks[i] = clk_get(d->dma_dev, shared[i]);
  if (IS_ERR(d->clks[i])) { ret = PTR_ERR(d->clks[i]); goto fail; }
 }
 clock.np = of_find_node_by_path("/vcodecsys@17000000");
 if (!clock.np) goto fail;
 d->clks[4] = of_clk_get_from_provider(&clock);
 of_node_put(clock.np);
 if (IS_ERR(d->clks[4])) { ret = PTR_ERR(d->clks[4]); goto fail; }
 d->syscon = syscon_regmap_lookup_by_phandle(d->dma_dev->of_node, "mediatek,vcodec-syscon");
 if (IS_ERR(d->syscon)) { ret = PTR_ERR(d->syscon); goto fail; }
 d->regs = devm_platform_ioremap_resource(pdev, 0);
 if (IS_ERR(d->regs)) { ret = PTR_ERR(d->regs); goto fail; }
 d->input = dma_alloc_coherent(d->dma_dev, INPUT_SIZE, &d->input_dma, GFP_KERNEL);
 if (!d->input) { ret = -ENOMEM; goto fail; }
 if ((d->input_dma & 63) || d->input_dma > U32_MAX - INPUT_SIZE) { ret = -ERANGE; goto fail; }
 /* Extra reference slots are allocated lazily before a job can start DMA. */
 for (i = 0; i < 6; i++) {
  d->work[i] = i ? dma_alloc_coherent(d->dma_dev, work_sizes[i], &d->work_dma[i], GFP_KERNEL) : pixels_alloc(d->dma_dev,&d->work_dma[i],&d->pixels_sgt);
  if (!d->work[i]) { ret = -ENOMEM; goto fail; }
  if(!i)pixels_cpu(d->dma_dev,d->work[0],d->pixels_sgt);
  if ((d->work_dma[i] & 4095) || d->work_dma[i] > U32_MAX - work_sizes[i]) { ret = -ERANGE; goto fail; }
 }
 d->irq = platform_get_irq(pdev, 0);
 if (d->irq < 0) { ret = d->irq; goto fail; }
 ret = devm_request_irq(d->dev, d->irq, interrupt, IRQF_NO_AUTOEN, "r1-vdec-probe", d);
 if (ret) goto fail;
 d->misc = (struct miscdevice){ .minor = MISC_DYNAMIC_MINOR,
  .name = "r1-vdec-probe", .fops = &fops, .mode = 0600, .parent = d->dev };
 ret = video ? register_video(d) : misc_register(&d->misc);
 if (ret) goto fail;
 platform_set_drvdata(pdev, d);
 dev_info(d->dev, "bounded fixed-frame decoder probe ready; no decoder DMA started\n");
 return 0;
fail:
 release_resources(d);
 return ret;
}
static void remove_device(struct platform_device *pdev)
{
 struct decoder *d = platform_get_drvdata(pdev);
 if (video) unregister_video(d); else misc_deregister(&d->misc);
 release_resources(d);
}
static const struct of_device_id matches[] = {{ .compatible = "mediatek,vdec" }, {}};
MODULE_DEVICE_TABLE(of, matches);
static struct platform_driver vdec_probe_driver = { .probe = probe, .remove = remove_device,
 .driver = { .name = "r1-vdec-probe", .of_match_table = matches, .suppress_bind_attrs = true } };
module_platform_driver(vdec_probe_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 bounded native fixed-frame decoder probe");
