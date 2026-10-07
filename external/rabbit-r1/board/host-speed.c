// SPDX-License-Identifier: GPL-2.0
/* Full-speed retry for a host connection that could not enumerate at high speed.
 * Only clears HSENAB before port enable; normal session setup restores it.
 * No device reset, driver unbind, PHY or power-supply changes here. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include "musb_core.h"
static struct device *controller;
static int fallback(const char *value,const struct kernel_param *kp)
{
 struct musb *m;unsigned long flags;int ret;
 if(!sysfs_streq(value,"fullspeed"))return -EINVAL;
 if(!controller)return -ENODEV;
 ret=pm_runtime_resume_and_get(controller);if(ret<0)return ret;
 m=dev_get_drvdata(controller);
 if(!m){ret=-ENODEV;goto out;}
 spin_lock_irqsave(&m->lock,flags);
 if(!m->is_host || m->gadget_driver || (m->port1_status & USB_PORT_STAT_ENABLE))ret=-EBUSY;
 else {
  musb_writeb(m->mregs,MUSB_POWER,musb_readb(m->mregs,MUSB_POWER)&~MUSB_POWER_HSENAB);
  ret=0;
 }
 spin_unlock_irqrestore(&m->lock,flags);
out:pm_runtime_put(controller);return ret;
}
static const struct kernel_param_ops ops={.set=fallback};
module_param_cb(mode,&ops,NULL,0200);
static int __init start(void) {
 controller=bus_find_device_by_name(&platform_bus_type,NULL,"musb-hdrc.1.auto");
 return controller?0:-ENODEV;
}
static void __exit stop(void){put_device(controller);}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("R1 pre-enumeration full-speed USB host fallback");
