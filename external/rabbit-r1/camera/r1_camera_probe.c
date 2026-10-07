// SPDX-License-Identifier: GPL-2.0-only
/* Bounded board-specific Hi846 probe and optional RAW10 capture. No motor IO.
 * Wiring/power sequence: shipped overlay-0 and mt6765/imgsensor_cfg_table.c.
 */
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/pinctrl/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <dt-bindings/clock/mt6765-clk.h>
#include "r1_camera_pipeline.h"
static struct gpiod_lookup_table lookup = {
 .dev_id="r1-camera-probe", .table={
 GPIO_LOOKUP("pinctrl_paris",103,"scl",GPIO_ACTIVE_HIGH),
 GPIO_LOOKUP("pinctrl_paris",104,"sda",GPIO_ACTIVE_HIGH),
 GPIO_LOOKUP("pinctrl_paris",97,"pdn",GPIO_ACTIVE_HIGH),
 GPIO_LOOKUP("pinctrl_paris",101,"reset",GPIO_ACTIVE_HIGH),
 GPIO_LOOKUP("pinctrl_paris",165,"avdd",GPIO_ACTIVE_HIGH), {}
 }};
static void __iomem *gpio;
static void output(unsigned p, int value) {
 unsigned bank=(p/32)*16; u32 bit=BIT(p%32);
 writel(bit,gpio+0x100+bank+(value?4:8));
 writel(bit,gpio+bank+4);
}
static void od(unsigned p,int value) {
 unsigned bank=(p/32)*16; u32 bit=BIT(p%32);
 if(value) writel(bit,gpio+bank+8);
 else {writel(bit,gpio+0x100+bank+8); writel(bit,gpio+bank+4);}
}
static int input(unsigned p) {return !!(readl(gpio+0x200+(p/32)*16)&BIT(p%32));}
static void sda(void *p,int v) {od(104,v);}
static void scl(void *p,int v) {od(103,v);}
static int getsda(void *p) {return input(104);}
static int getscl(void *p) {return input(103);}
static struct i2c_algo_bit_data bit={.setsda=sda,.setscl=scl,.getsda=getsda,.getscl=getscl,.udelay=5,.timeout=HZ/20};
static struct i2c_adapter bus={.owner=THIS_MODULE,.name="R1 bounded camera ID",.algo_data=&bit};
static int rd(u16 reg) {
 u8 addr[2]={reg>>8,reg}, val=0;
 struct i2c_msg m[2]={{.addr=0x20,.len=2,.buf=addr},{.addr=0x20,.flags=I2C_M_RD,.len=1,.buf=&val}};
 int r=i2c_transfer(&bus,m,2); return r==2?val:(r<0?r:-EIO);
}
#include "r1_camera_stream.h"
/* Stock regulator nodes do not match the native PMIC names. Temporarily
 * supply only the missing enable/disable board policy, with no voltage change.
 * Both rails must be unused. Restore the exact masks after power cleanup. */
static struct device *raildev[2];
static unsigned masks[2];
static int match_rail(struct device *dev, const void *name) {
 struct regulator_dev *r=dev_get_drvdata(dev);
 return r && r->desc && !strcmp(r->desc->name,name);
}
static int rail_policy(bool restore) {
 struct device *pmic; int i,ret=0;
 const char *names[]={"VCAMIO","VCAMD"};
 if(restore) {
  for(i=1;i>=0;i--) if(raildev[i]) {
   struct regulator_dev *r=dev_get_drvdata(raildev[i]);
   ww_mutex_lock(&r->mutex,NULL);
   r->constraints->valid_ops_mask=masks[i];
   ww_mutex_unlock(&r->mutex);
   put_device(raildev[i]);raildev[i]=NULL;
  }
  return 0;
 }
 pmic=bus_find_device_by_name(&platform_bus_type,NULL,"mt6357-regulator");
 if(!pmic)return -ENODEV;
 for(i=0;i<2;i++) {
  struct regulator_dev *r;
  struct device *d=device_find_child(pmic,names[i],match_rail);
  if(!d){ret=-ENODEV;break;}
  r=dev_get_drvdata(d);
  ww_mutex_lock(&r->mutex,NULL);
  if(r->use_count || r->open_count || !r->constraints) {
   ww_mutex_unlock(&r->mutex);put_device(d);ret=-EBUSY;break;
  }
  masks[i]=r->constraints->valid_ops_mask;raildev[i]=d;
  r->constraints->valid_ops_mask |= REGULATOR_CHANGE_STATUS;
  ww_mutex_unlock(&r->mutex);
 }
 put_device(pmic);
 return ret;
}

 struct device_node *np; struct platform_device *cam=NULL;
 struct device *dev=NULL; struct gpio_desc *pins[5]={};
 const char *names[]={"scl","sda","pdn","reset","avdd"};
 struct regulator *io=NULL,*core=NULL; struct clk *mclk=NULL;
 struct pinctrl *pc=NULL; struct pinctrl_state *on=NULL,*off=NULL;
 struct of_phandle_args a={};
 bool lookup_added=false, bus_on=false,clk_on=false,io_on=false,core_on=false,mux_on=false;


static DEFINE_MUTEX(live_lock);
static bool live_active, live_failed, cleanup_attempted, live_streaming;
/* Optional two-stage startup: retain sensor power while userspace starts motion
 * and programs the image stream in parallel. Default preserves existing callers. */
static bool deferred_stream;
module_param(deferred_stream, bool, 0400);
MODULE_PARM_DESC(deferred_stream, "Power/identify only on load; start stream through debugfs/start");
static unsigned long delivered_count;
static int camera_cleanup(int r) {
 int i;
 if(sv && (stream_running || dma_attempted)){int sr=stream_shutdown();if(sr)return sr;}

 if(pins[3])output(101,0);
 if(pins[2])output(97,0);
 if(pins[4])output(165,0);
 if(pipeline_stage) {int cleanup=camera_power_off();if(cleanup)r=cleanup;}
 if(core_on)regulator_disable(core);
 if(io_on)regulator_disable(io);
 if(clk_on)clk_disable_unprepare(mclk);
 if(mux_on)pinctrl_select_state(pc,off);
 if(bus_on)i2c_del_adapter(&bus);
 for(i=4;i>=0;i--)if(pins[i])gpiod_put(pins[i]);
 if(dev)root_device_unregister(dev);
 if(lookup_added) gpiod_remove_lookup_table(&lookup);
 if(gpio)iounmap(gpio);
 if(pc)pinctrl_put(pc);
 if(mclk)clk_put(mclk);
 if(core)regulator_put(core);
 if(io)regulator_put(io);
 if(cam)put_device(&cam->dev);
 rail_policy(true);

 if(cam_powered)return r?r:-EIO;

 pr_info("r1-camera-probe: result=%d; sensor power cleanup completed; capture buffer=%d\n",r,!!capture_mem);
 return r;
}

static int live_open(struct inode *inode,struct file *file) {
 void *snapshot;int r;unsigned seq;unsigned long count;
 if(!mutex_trylock(&live_lock))return -EBUSY;
 if(!live_active || live_failed){mutex_unlock(&live_lock);return -ENODEV;}
 if(!live_streaming){mutex_unlock(&live_lock);return -EAGAIN;}
 snapshot=vmalloc(SZ_1M);
 if(!snapshot){mutex_unlock(&live_lock);return -ENOMEM;}
 r=wait_event_interruptible_timeout(snapshot_wait,READ_ONCE(snapshot_count)!=delivered_count || atomic_read(&irq_error_count),HZ);
 if(r<=0 || atomic_read(&irq_error_count)){vfree(snapshot);mutex_unlock(&live_lock);return r<0?r:-EIO;}
 do {seq=read_seqcount_begin(&snapshot_seq);memcpy(snapshot,snapshot_mem,SZ_1M);count=snapshot_count;} while(read_seqcount_retry(&snapshot_seq,seq));
 delivered_count=count;
 file->private_data=snapshot;mutex_unlock(&live_lock);return 0;
}
static ssize_t live_read(struct file *f,char __user *buf,size_t n,loff_t *pos) {
 return simple_read_from_buffer(buf,n,pos,f->private_data,SZ_1M);
}
static int live_release(struct inode *inode,struct file *file) {vfree(file->private_data);return 0;}
static const struct file_operations live_ops={.owner=THIS_MODULE,.open=live_open,.read=live_read,.release=live_release};
static ssize_t live_start(struct file *f,const char __user *buf,size_t n,loff_t *pos) {
 int r;char c;
 if(n!=1 || copy_from_user(&c,buf,1) || c!='1')return -EINVAL;
 mutex_lock(&live_lock);
 if(!live_active || live_failed || cleanup_attempted)r=-ENODEV;
 else if(live_streaming)r=-EALREADY;
 else {
  r=stream_test();
  if(r)live_failed=true;
  else live_streaming=true;
  /* On failure retain power until explicit stop: the owner must park first. */
 }
 mutex_unlock(&live_lock);return r?r:n;
}
static const struct file_operations start_ops={.owner=THIS_MODULE,.write=live_start};
static ssize_t live_stop(struct file *f,const char __user *buf,size_t n,loff_t *pos) {
 int r=0;char c;
 if(n!=1 || copy_from_user(&c,buf,1) || c!='1')return -EINVAL;
 mutex_lock(&live_lock);
 if(live_active && cleanup_attempted){mutex_unlock(&live_lock);return -EIO;}
 if(live_active){cleanup_attempted=true;r=camera_cleanup(0);if(!r){live_active=false;live_streaming=false;module_put(THIS_MODULE);}}
 mutex_unlock(&live_lock);return r?r:n;
}
static const struct file_operations stop_ops={.owner=THIS_MODULE,.write=live_stop};
static int live_publish(void) {
 struct dentry *d;
 capture_debug=debugfs_create_dir("r1-camera-live",NULL);
 if(IS_ERR(capture_debug))return PTR_ERR(capture_debug);
 d=debugfs_create_file("frame.raw",0400,capture_debug,NULL,&live_ops);
 if(IS_ERR(d))return PTR_ERR(d);
 d=debugfs_create_file("start",0200,capture_debug,NULL,&start_ops);
 if(IS_ERR(d))return PTR_ERR(d);
 d=debugfs_create_file("stop",0200,capture_debug,NULL,&stop_ops);
 return IS_ERR(d)?PTR_ERR(d):0;
}
static int __init probe_init(void) {
 int r=-ENODEV,i,hi=-1,lo=-1;
 pipeline_stage=3;
 np=of_find_node_by_path("/kd_camera_hw1@1a040000");
 if(np) cam=of_find_device_by_node(np);
 of_node_put(np);
 if(!cam) return -ENODEV;
 {void __iomem *spm=ioremap(0x10006000,0x1000);
 if(spm){pr_info("r1-camera-probe: SPM status=%08x/%08x CAM=%08x\n",readl(spm+0x180),readl(spm+0x184),readl(spm+0x344));iounmap(spm);}}
 if(cam->dev.driver) {r=-EBUSY;goto out;}
 r=rail_policy(false);
 if(r)goto out;
 io=regulator_get_optional(NULL,"VCAMIO"); if(IS_ERR(io)){r=PTR_ERR(io);io=NULL;goto out;}
 core=regulator_get_optional(NULL,"VCAMD"); if(IS_ERR(core)){r=PTR_ERR(core);core=NULL;goto out;}
 if(regulator_get_voltage(io)!=1800000 || regulator_get_voltage(core)!=1200000){r=-ERANGE;goto out;}
 a.np=of_find_node_by_path("/topckgen@10000000"); a.args_count=1;a.args[0]=CLK_TOP_CAMTG_SEL;
 mclk=of_clk_get_from_provider(&a);of_node_put(a.np);
 if(IS_ERR(mclk)){r=PTR_ERR(mclk);mclk=NULL;goto out;}
 if(clk_get_rate(mclk)!=24000000){r=-ERANGE;goto out;}
 pc=pinctrl_get(&cam->dev);
 if(IS_ERR(pc)){r=PTR_ERR(pc);pc=NULL;goto out;}
 on=pinctrl_lookup_state(pc,"cam0_mclk_on");off=pinctrl_lookup_state(pc,"cam0_mclk_off");
 if(IS_ERR(on)||IS_ERR(off)){r=-EINVAL;goto out;}
 gpio=ioremap(0x10005000,0x1000);
 if(!gpio){r=-ENOMEM;goto out;}
 gpiod_add_lookup_table(&lookup); lookup_added=true;
 dev=root_device_register("r1-camera-probe");
 if(IS_ERR(dev)){r=PTR_ERR(dev);dev=NULL;goto out;}
 for(i=0;i<5;i++){pins[i]=gpiod_get(dev,names[i],GPIOD_IN);
 if(IS_ERR(pins[i])){r=PTR_ERR(pins[i]);pins[i]=NULL;goto out;}}
 output(97,0);output(101,0);output(165,0);
 r=i2c_bit_add_bus(&bus);
 if(r)goto out;
 bus_on=true;
 r=pinctrl_select_state(pc,on);
 if(r)goto out;
 mux_on=true;
 r=clk_prepare_enable(mclk);
 if(r)goto out;
 clk_on=true;msleep(1);
 r=regulator_enable(io);
 if(r)goto out;
 io_on=true;msleep(5);
 r=regulator_enable(core);
 if(r)goto out;
 core_on=true;msleep(5);
 if(pipeline_stage) {r=camera_power_on();if(r)goto out;}
 output(165,1);msleep(5);output(97,1);msleep(10);output(101,1);msleep(10);
 lo=rd(0x0f16);hi=rd(0x0f17);
 pr_info("r1-camera-probe: sensor ID bytes %d %d (expected 8 70); bus=%d rails=%d/%d mclk=%lu\n",hi,lo,bus.nr,regulator_get_voltage(io),regulator_get_voltage(core),clk_get_rate(mclk));
 r=(hi==8&&lo==70)?0:-ENODEV;
 if(!r && pipeline_stage>=2 && !deferred_stream){r=stream_test();if(!r)live_streaming=true;}
 if(!r){r=live_publish();if(!r){live_active=true;__module_get(THIS_MODULE);return 0;}}
out:
 r=camera_cleanup(r);
 if(cam_powered){__module_get(THIS_MODULE);return 0;}
 capture_free();return r;
}
static void __exit probe_exit(void) {capture_free();}
module_init(probe_init);module_exit(probe_exit);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Rabbit serialized live snapshots; explicit stop releases power");
