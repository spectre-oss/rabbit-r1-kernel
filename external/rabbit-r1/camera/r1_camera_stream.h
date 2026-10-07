/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/atomic.h>
#include <linux/seqlock.h>
#include <linux/wait.h>
#include "sensor-tables.h"
#include <generated/rabbit-r1-camera-receiver.h>
#include "r1_camera_capture.h"
static bool receiver_started, sensor_configured;
static int cam_irq;
static DECLARE_WAIT_QUEUE_HEAD(snapshot_wait);
static seqcount_t snapshot_seq=SEQCNT_ZERO(snapshot_seq);
static unsigned long snapshot_count;
static bool stream_running;
static bool requeue_enabled;
static atomic_t sof_count=ATOMIC_INIT(0), done_count=ATOMIC_INIT(0), irq_error_count=ATOMIC_INIT(0);
static irqreturn_t camera_irq_handler(int irq,void *cookie) {
 u32 st=readl(sv+0x1c); /* Shipped handler reads status; no W1C write. */
 if(!st)return IRQ_NONE;
 if(st&BIT(7))atomic_inc(&sof_count);
 if(st&BIT(10)) {
  atomic_inc(&done_count);
  dma_rmb();
  write_seqcount_begin(&snapshot_seq);
  memcpy(snapshot_mem,capture_mem,SZ_1M);
  snapshot_count++;
  write_seqcount_end(&snapshot_seq);
  wake_up_interruptible(&snapshot_wait);
 }
 if((st&BIT(10)) && !(st&(BIT(4)|BIT(5)|BIT(16)|BIT(17))) && READ_ONCE(requeue_enabled)) {
  writel(capture_dma,sv+0x118);writel(0,sv+0xe04);
  writel(readl(sv+0x2c)|1,sv+0x2c);
 }
 if(st&(BIT(4)|BIT(5)|BIT(16)|BIT(17))) {atomic_inc(&irq_error_count);WRITE_ONCE(requeue_enabled,false);wake_up_interruptible(&snapshot_wait);}
 return IRQ_HANDLED;
}
static int camera_irq_start(void) {
 struct device_node *np=of_find_node_by_path("/camsv1@1a050000");int r;
 if(!np)return -ENODEV;
 cam_irq=irq_of_parse_and_map(np,0);of_node_put(np);
 if(!cam_irq)return -ENODEV;
 r=request_irq(cam_irq,camera_irq_handler,0,"r1-camera-diagnostic",&cam_irq);
 if(r){irq_dispose_mapping(cam_irq);cam_irq=0;return r;}
 readl(sv+0x1c); /* clear any preexisting read-to-clear status */
 writel(BIT(7)|BIT(10)|BIT(20)|BIT(4)|BIT(5)|BIT(16)|BIT(17),sv+0x18);
 return 0;
}
static void camera_irq_stop(void) {
 if(!cam_irq)return;
 writel(0,sv+0x18);readl(sv+0x1c);
 free_irq(cam_irq,&cam_irq);irq_dispose_mapping(cam_irq);cam_irq=0;
 pr_info("r1-camera-irq: sof=%d done=%d errors=%d\n",atomic_read(&sof_count),atomic_read(&done_count),atomic_read(&irq_error_count));
}
static unsigned int analog_gain;
module_param(analog_gain,uint,0400);
static bool diagnostics;
module_param(diagnostics,bool,0400);
static int wr(u16 reg,u16 value) {
 u8 b[]={reg>>8,reg,value>>8,value};
 struct i2c_msg m={.addr=0x20,.len=4,.buf=b};
 int r=i2c_transfer(&bus,&m,1);return r==1?0:(r<0?r:-EIO);
}
static int sensor_table(const u16 (*t)[2],size_t n) {
 size_t i;int r;
 for(i=0;i<n;i++){r=wr(t[i][0],t[i][1]);if(r){pr_err("r1-camera: sensor write %04x failed %d\n",t[i][0],r);return r;}}
 return 0;
}
static void __iomem *receiver_addr(u32 a) {
 if(a>=0x1a040000 && a<0x1a048000)return sen+(a-0x1a040000);
 if(a>=0x11c10000 && a<0x11c16000)return phy+(a-0x11c10000);
 return NULL;
}
static int receiver_start(void) {
 u32 saved[ARRAY_SIZE(receiver_steps)];size_t i;
 for(i=0;i<ARRAY_SIZE(receiver_steps);i++) {
  void __iomem *p=receiver_addr(receiver_steps[i].physical);
  if(!p)return -EINVAL;
  saved[i]=readl(p);
 }
 for(i=0;i<ARRAY_SIZE(receiver_steps);i++) {
  const struct receiver_step *s=&receiver_steps[i];
  writel((saved[i]&s->keep)|s->value,receiver_addr(s->physical));
  udelay(50); /* >= all fixed delays in the stock PHY sequence. */
 }
 /* Stock setSeninfMuxCtrl: mux2, CSI2 source8, RAW10 format1, two pixels. */
 rmw(sen,8,15<<8,4<<8);
 rmw(sen,0x2d00,0,0x80000000);
 rmw(sen,0x2d00,0xf000,0x8000);
 rmw(sen,0x2d3c,3,1);rmw(sen,0x2d3c,0x10,0);
 rmw(sen,0x2d00,0,0x100);
 rmw(sen,0x2d00,0x30000000,0x20000000);
 rmw(sen,0x2d00,0x0fc00000,0x06c00000);
 rmw(sen,0x2d00,0x003f0000,0x001f0000);
 rmw(sen,0x2d00,0x600,0);
 rmw(sen,0x2d00,0,3);rmw(sen,0x2d00,3,0);
 rmw(sen,0x2d3c,0,0x100);
 /* Stock CAMSV reset and TG-only configuration; DMA remains disabled. */
 writel(4,sv+0x20);writel(0,sv+0x20);writel(1,sv+0x20);
 if(pollbits(sv,0x20,3,3))return -ETIMEDOUT;
 writel(0,sv+0x20);writel(0x8005,sv+0x30);
 writel(0x40000001,sv+0x10);writel(1,sv+0x14);
 writel(0x10002,sv+0x500);writel(0,sv+0x504);
 writel(640<<16,sv+0x508);writel(480<<16,sv+0x50c);
 writel(0x10003,sv+0x500);
 receiver_started=true;return 0;
}
static void receiver_stop(void) {
 if(!receiver_started)return;
 rmw(sv,0x500,1,0);writel(0,sv+0x10);writel(0,sv+0x30);
 rmw(sen,0x4a00,0x1f,0);rmw(sen,0x4200,1,0);
 rmw(phy,0x4000,12,0);rmw(phy,0x5000,12,0);receiver_started=false;
}
static int stream_test(void) {
 int r;
 r=wr(0x0a00,0);if(r)return r;
 if(!sensor_configured) {
 r=sensor_table(sensor_init,ARRAY_SIZE(sensor_init));if(r)return r;
 r=sensor_table(sensor_hs_video,ARRAY_SIZE(sensor_hs_video));if(r)return r;
 r=wr(0x0006,2526);if(r)return r;
 r=wr(0x0074,2000);if(r)return r;
 if(analog_gain>240)return -EINVAL;
 r=wr(0x0076,analog_gain);if(r)return r;
 pr_info("r1-camera: exposure=%02x%02x%02x gain=%02x frame=%02x%02x\n",rd(0x73),rd(0x74),rd(0x75),rd(0x77),rd(6),rd(7));
 if(diagnostics)pr_info("r1-camera: programmed VGA dimensions=%02x%02x/%02x%02x\n",rd(0xa12),rd(0xa13),rd(0xa14),rd(0xa15));
 sensor_configured=true;
 }
 r=receiver_start();if(r)return r;
 if(pipeline_stage==3){r=capture_start();if(r)goto stop;}
 r=camera_irq_start();if(r)goto stop;
 WRITE_ONCE(requeue_enabled,true);
 r=wr(0x0a00,0x0100);if(r)goto stop;
 stream_running=true;
 r=wait_event_interruptible_timeout(snapshot_wait,READ_ONCE(snapshot_count)>0 || atomic_read(&irq_error_count),HZ);
 if(r>0 && !atomic_read(&irq_error_count))return 0;
 r=r<0?r:-ETIMEDOUT;
stop:
 WRITE_ONCE(requeue_enabled,false);
 wr(0x0a00,0);msleep(50);
 camera_irq_stop();
 if(dma_attempted){int cr=capture_stop();if(cr)r=cr;}
 receiver_stop();stream_running=false;return r;
}
static int stream_shutdown(void) {
 int r=0;
 WRITE_ONCE(requeue_enabled,false);
 if(stream_running){r=wr(0x0a00,0);msleep(50);}
 camera_irq_stop();
 if(dma_attempted){int cr=capture_stop();if(cr)r=cr;dma_attempted=false;}
 receiver_stop();stream_running=false;
 return r;
}
