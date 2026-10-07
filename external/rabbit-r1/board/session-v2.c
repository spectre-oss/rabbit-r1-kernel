// SPDX-License-Identifier: GPL-2.0
/* Finish the MT6765 role transition after gadget unbind. MediaTek's role
 * callback sets SESSION, but does not restart interrupts stopped by gadget
 * unbind. The vendor USB20 host sequence also wakes and cycles PHY signals. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/io.h>
#include <linux/delay.h>
#include "musb_core.h"
static struct device *dev;
static void __iomem *phy;
static u32 dtm0,dtm1;
static bool ready;
static DEFINE_MUTEX(role_lock);
static int set(const char *value,const struct kernel_param *kp)
{
 struct musb *m;unsigned long flags;u8 ctl;int r;
 bool host=sysfs_streq(value,"host");
 if(!host && !sysfs_streq(value,"device"))return -EINVAL;
 if(!ready)return -ENODEV;
 mutex_lock(&role_lock);
 m=dev_get_drvdata(dev);
 if(!m||m->port_mode!=MUSB_OTG){r=-ENODEV;goto out;}
 if(host && m->gadget_driver){r=-EBUSY;goto out;}
 r=pm_runtime_resume_and_get(dev);if(r<0)goto out;
 if(host) {
  writel(readl(phy+0x68)|BIT(18)|BIT(3),phy+0x68);
  spin_lock_irqsave(&m->lock,flags);
  musb_writeb(m->mregs,MUSB_DEVCTL,0);
  spin_unlock_irqrestore(&m->lock,flags);
  writel((readl(phy+0x6c)&~0x3f3f)|0x3e10,phy+0x6c);
  usleep_range(5000,6000);
  spin_lock_irqsave(&m->lock,flags);
  musb_writeb(m->mregs,MUSB_DEVCTL,MUSB_DEVCTL_SESSION);
  spin_unlock_irqrestore(&m->lock,flags);
  writel((readl(phy+0x6c)&~0x3f3f)|0x3e2c,phy+0x6c);
  r=musb_set_host(m);
  if(!r) {
   spin_lock_irqsave(&m->lock,flags);
   m->intrtxe=m->epmask;m->intrrxe=m->epmask&0xfffe;
   musb_writew(m->mregs,MUSB_INTRTXE,m->intrtxe);
   musb_writew(m->mregs,MUSB_INTRRXE,m->intrrxe);
   musb_writeb(m->mregs,MUSB_INTRUSBE,0xf7);
   musb_writeb(m->mregs,MUSB_POWER,MUSB_POWER_ISOUPDATE|MUSB_POWER_HSENAB);
   musb_set_state(m,OTG_STATE_A_WAIT_BCON);
   ctl=musb_readb(m->mregs,MUSB_DEVCTL)|MUSB_DEVCTL_SESSION;
   musb_writeb(m->mregs,MUSB_DEVCTL,ctl);
   spin_unlock_irqrestore(&m->lock,flags);
  }
 } else {
  writel((readl(phy+0x6c)&~0x3f3f)|(dtm1&0x3f3f)|BIT(1),phy+0x6c);
  writel((readl(phy+0x68)&~(BIT(18)|BIT(3)))|(dtm0&(BIT(18)|BIT(3))),phy+0x68);
  r=musb_set_peripheral(m);
 }
 pm_runtime_put(dev);
out:mutex_unlock(&role_lock);return r;
}
static const struct kernel_param_ops ops={.set=set};
module_param_cb(mode,&ops,NULL,0200);
static int __init start(void)
{
 dev=bus_find_device_by_name(&platform_bus_type,NULL,"musb-hdrc.1.auto");
 if(!dev)return -ENODEV;
 phy=ioremap(0x11cc0800,0x100);
 if(!phy){put_device(dev);return -ENOMEM;}
 dtm0=readl(phy+0x68);dtm1=readl(phy+0x6c);
 ready=true;return 0;
}
module_init(start);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("R1 runtime role session sequencing");
