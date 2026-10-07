// SPDX-License-Identifier: GPL-2.0
/* Rabbit R1 MT6357 ERM haptics. No pulse on load; all PMIC I/O in process context. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6357/registers.h>
#include <linux/regmap.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/delay.h>
#define EN BIT(0)
#define DA_EN BIT(15)
#define VOSEL_MASK 0xf00
#define VOSEL (0xd << 8) /* 3.3 V: previously owner-verified on this unit. */
#define MAX_MS 500
struct haptics {
 struct regmap *map;
 struct mutex lock;
 struct delayed_work off;
 bool on, fault;
 unsigned commands, completed, duration, saved_ana, saved_op;
 unsigned during_en, during_da, during_oc;
 unsigned long next;
 int result;
};
static int off_locked(struct haptics *h)
{
 unsigned en=EN;int ret=regmap_update_bits(h->map,MT6357_LDO_VIBR_CON0,EN,0);
 if(!ret)ret=regmap_read(h->map,MT6357_LDO_VIBR_CON0,&en);
 if(!ret&&(en&EN))ret=-EIO;
 h->on=false;
 if(ret)h->fault=true;
 h->result=ret;return ret;
}
static void off_work(struct work_struct *work)
{
 struct haptics *h=container_of(to_delayed_work(work),struct haptics,off);
 mutex_lock(&h->lock);
 if(!off_locked(h))h->completed++;
 mutex_unlock(&h->lock);
}
static ssize_t buzz_store(struct device *dev,struct device_attribute *attr,const char *buf,size_t count)
{
 struct haptics *h=dev_get_drvdata(dev);unsigned ms;int ret;
 if(kstrtouint(buf,10,&ms)||!ms||ms>MAX_MS)return -EINVAL;
 mutex_lock(&h->lock);
 if(h->fault){ret=-EIO;goto out;}
 if(h->on){ret=-EBUSY;goto out;}
 if(time_before(jiffies,h->next)){ret=-EAGAIN;goto out;}
 ret=regmap_write(h->map,MT6357_LDO_VIBR_OP_EN_SET,EN);
 if(!ret)ret=regmap_update_bits(h->map,MT6357_VIBR_ANA_CON0,VOSEL_MASK,VOSEL);
 if(!ret)ret=regmap_update_bits(h->map,MT6357_LDO_VIBR_CON0,EN,EN);
 if(ret)goto fail;
 h->on=true;
 /* Establish automatic shutoff before diagnostic reads. */
 mod_delayed_work(system_highpri_wq,&h->off,msecs_to_jiffies(ms));
 h->duration=ms;h->commands++;h->next=jiffies+msecs_to_jiffies(max(250U,ms*3));
 usleep_range(1000,1500);
 ret=regmap_read(h->map,MT6357_LDO_VIBR_CON0,&h->during_en);
 if(!ret)ret=regmap_read(h->map,MT6357_LDO_VIBR_CON1,&h->during_da);
 if(!ret)ret=regmap_read(h->map,MT6357_LDO_TOP_INT_STATUS1,&h->during_oc);
 if(!ret&&(!(h->during_en&EN)||!(h->during_da&DA_EN)||(h->during_oc&BIT(3))))ret=-EIO;
 if(ret)goto fail;
 h->result=0;goto out;
fail:
 h->fault=true;off_locked(h);h->result=ret;
out:
 mutex_unlock(&h->lock);return ret?ret:count;
}
static ssize_t stop_store(struct device *dev,struct device_attribute *attr,const char *buf,size_t count)
{
 struct haptics *h=dev_get_drvdata(dev);int ret;
 if(!sysfs_streq(buf,"1"))return -EINVAL;
 cancel_delayed_work_sync(&h->off);mutex_lock(&h->lock);ret=off_locked(h);mutex_unlock(&h->lock);
 return ret?ret:count;
}
static ssize_t status_show(struct device *dev,struct device_attribute *attr,char *buf)
{
 struct haptics *h=dev_get_drvdata(dev);unsigned en=0;int ret;ssize_t n;
 mutex_lock(&h->lock);ret=regmap_read(h->map,MT6357_LDO_VIBR_CON0,&en);
 n=ret?ret:sysfs_emit(buf,"ready=%u on=%u hw_en=%u commands=%u completed=%u last_ms=%u result=%d da_en=%u oc=%u voltage=3300000 max_ms=500\n",
 !h->fault,h->on,!!(en&EN),h->commands,h->completed,h->duration,h->result,!!(h->during_da&DA_EN),!!(h->during_oc&BIT(3)));
 mutex_unlock(&h->lock);return n;
}
static DEVICE_ATTR_WO(buzz);
static DEVICE_ATTR_WO(stop);
static DEVICE_ATTR_RO(status);
static struct attribute *haptics_attrs[]={&dev_attr_buzz.attr,&dev_attr_stop.attr,&dev_attr_status.attr,NULL};
ATTRIBUTE_GROUPS(haptics);
static int probe(struct platform_device *pdev)
{
 struct device_node *np;struct platform_device *pmic;struct mt6397_chip *chip;struct haptics *h;int ret;
 np=of_find_compatible_node(NULL,NULL,"mediatek,mt6357");if(!np)return -ENODEV;
 pmic=of_find_device_by_node(np);of_node_put(np);if(!pmic)return -EPROBE_DEFER;
 chip=dev_get_drvdata(&pmic->dev);
 h=devm_kzalloc(&pdev->dev,sizeof(*h),GFP_KERNEL);
 if(!h){put_device(&pmic->dev);return -ENOMEM;}
 h->map=chip?chip->regmap:NULL;put_device(&pmic->dev);if(!h->map)return -EPROBE_DEFER;
 mutex_init(&h->lock);INIT_DELAYED_WORK(&h->off,off_work);
 ret=regmap_read(h->map,MT6357_VIBR_ANA_CON0,&h->saved_ana);
 if(!ret)ret=regmap_read(h->map,MT6357_LDO_VIBR_OP_EN,&h->saved_op);
 if(!ret)ret=off_locked(h);
 if(ret)return ret;
 platform_set_drvdata(pdev,h);dev_info(&pdev->dev,"haptics ready, off; bounded asynchronous pulses\n");return 0;
}
static void remove_device(struct platform_device *pdev)
{
 struct haptics *h=platform_get_drvdata(pdev);
 cancel_delayed_work_sync(&h->off);mutex_lock(&h->lock);off_locked(h);
 regmap_update_bits(h->map,MT6357_VIBR_ANA_CON0,VOSEL_MASK,h->saved_ana&VOSEL_MASK);
 regmap_update_bits(h->map,MT6357_LDO_VIBR_OP_EN,EN,h->saved_op&EN);
 mutex_unlock(&h->lock);
}
static struct platform_device *device;
static struct platform_driver driver={.probe=probe,.remove=remove_device,.shutdown=remove_device,.driver={.name="r1_vibrator",.dev_groups=haptics_groups}};
static int __init start(void)
{
 int ret;
 ret=platform_driver_register(&driver);if(ret)return ret;
 device=platform_device_register_simple("r1_vibrator",-1,NULL,0);
 if(IS_ERR(device)){platform_driver_unregister(&driver);return PTR_ERR(device);}return 0;
}
static void __exit stop(void){platform_device_unregister(device);platform_driver_unregister(&driver);}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 persistent bounded MT6357 vibrator haptics");
