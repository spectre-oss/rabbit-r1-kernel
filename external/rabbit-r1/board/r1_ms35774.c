// SPDX-License-Identifier: GPL-2.0
/* R1-only MS35774 bring-up driver. No homing, boot sweep or shutdown move.
 * Native GPIO descriptors reserve pins and set mux/direction. Per-bit DOUT
 * aliases avoid RMW races with the R1 menu's atomic GPIO bit-banged inputs.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>

/* Defaults remain the original small-movement bring-up limits.
 * An explicitly requested one-shot build may override these at compile time. */
#ifndef MAX_STEPS
#define MAX_STEPS 30 /* Stock: 6 half-step pulses per nominal degree. */
#endif
#ifndef MOVE_BUDGET
#define MOVE_BUDGET 2
#endif
#ifndef GUARD_MS
#define GUARD_MS 200
#endif
#if MAX_STEPS < 1 || MAX_STEPS > 1080 || MOVE_BUDGET < 1 || MOVE_BUDGET > 256 || GUARD_MS > 1800
#error "Unsupported motor test limits"
#endif
/* Maintenance reload can preserve the remaining budget; never arm on probe. */
static unsigned initial_budget = MOVE_BUDGET;
module_param(initial_budget, uint, 0400);
MODULE_PARM_DESC(initial_budget, "Initial command allowance, capped by compiled budget");
#define COOLDOWN_MS 2000
#define ARM_MS 30000
#define GPIO_PA 0x10005000
#define DOUT5 0x150

enum { VM, ENN, DIR, STEP, PDN, MS1, MS2, NPINS };
static const unsigned pins[NPINS] = {163,166,167,168,164,171,170};
static const char * const names[NPINS] = {"vm","enn","dir","step","pdn","ms1","ms2"};
static const int idle[NPINS] = {0,1,0,0,0,1,0};
struct motor {
	struct device *dev;
	struct gpio_desc *gpio[NPINS];
	void __iomem *regs;
	struct mutex command;
	spinlock_t io_lock;
	struct hrtimer guard;
	unsigned long arm_until, next_move;
	bool armed, moving, aborted, io_error;
	unsigned remaining, commands, pulses;
	int relative_steps, last_result;
};
static struct gpiod_lookup_table lookup = {
	.dev_id = "step_motor_ms35774",
	.table = {
		GPIO_LOOKUP("pinctrl_paris",163,"vm",GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("pinctrl_paris",166,"enn",GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("pinctrl_paris",167,"dir",GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("pinctrl_paris",168,"step",GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("pinctrl_paris",164,"pdn",GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("pinctrl_paris",171,"ms1",GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("pinctrl_paris",170,"ms2",GPIO_ACTIVE_HIGH),
		{}
	}
};
/* All runtime writers hold io_lock. Read back the target bit to flush writes. */
static void pin(struct motor *m, unsigned which, bool high)
{
	u32 bit = BIT(pins[which] % 32);
	writel(bit,m->regs + DOUT5 + (high ? 4 : 8));
	if (!!(readl(m->regs + DOUT5) & bit) != high)
		m->io_error = true;
}
static void off(struct motor *m)
{
	pin(m,ENN,1);
	pin(m,STEP,0);
	pin(m,VM,0);
}
static enum hrtimer_restart guard_stop(struct hrtimer *timer)
{
	struct motor *m = container_of(timer,struct motor,guard);
	unsigned long flags;
	spin_lock_irqsave(&m->io_lock,flags);
	m->aborted = true;
	off(m);
	spin_unlock_irqrestore(&m->io_lock,flags);
	return HRTIMER_NORESTART;
}
static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct motor *m = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t n;
	spin_lock_irqsave(&m->io_lock,flags);
	n = sysfs_emit(buf,"armed=%u moving=%u remaining=%u commands=%u pulses=%u relative_steps=%d physical_position=unknown result=%d io_error=%u vm=%u enn=%u step=%u max_steps=%u guard_ms=%u\n",
		m->armed && time_before(jiffies,m->arm_until),m->moving,
		m->remaining,m->commands,m->pulses,m->relative_steps,m->last_result,m->io_error,
		!!(readl(m->regs+DOUT5)&BIT(3)),!!(readl(m->regs+DOUT5)&BIT(6)),
		!!(readl(m->regs+DOUT5)&BIT(8)),MAX_STEPS,GUARD_MS);
	spin_unlock_irqrestore(&m->io_lock,flags);
	return n;
}
static ssize_t arm_store(struct device *dev, struct device_attribute *attr,
			 const char *buf,size_t count)
{
	struct motor *m = dev_get_drvdata(dev);
	unsigned long flags;
	unsigned value;
	int ret = kstrtouint(buf,10,&value);
	if (ret || value > 1) return -EINVAL;
	if (!mutex_trylock(&m->command)) return -EBUSY;
	spin_lock_irqsave(&m->io_lock,flags);
	if (!m->remaining) ret = -EDQUOT;
	else if (m->io_error) ret = -EIO;
	else if (time_before(jiffies,m->next_move)) ret = -EAGAIN;
	else {
		off(m);
		m->armed = value;
		m->arm_until = jiffies + msecs_to_jiffies(ARM_MS);
		if (m->io_error) {m->armed=false;ret=-EIO;}
	}
	spin_unlock_irqrestore(&m->io_lock,flags);
	mutex_unlock(&m->command);
	return ret ? ret : count;
}
static ssize_t stop_store(struct device *dev, struct device_attribute *attr,
			  const char *buf,size_t count)
{
	struct motor *m = dev_get_drvdata(dev);
	unsigned long flags;
	if (!sysfs_streq(buf,"1")) return -EINVAL;
	spin_lock_irqsave(&m->io_lock,flags);
	m->armed = false;
	m->aborted = true;
	off(m);
	spin_unlock_irqrestore(&m->io_lock,flags);
	return count;
}
static ssize_t move_store(struct device *dev, struct device_attribute *attr,
			  const char *buf,size_t count)
{
	struct motor *m = dev_get_drvdata(dev);
	unsigned long flags;
	int requested,steps,i,done=0,ret;
	ret = kstrtoint(buf,10,&requested);
	if (ret || !requested || requested > MAX_STEPS || requested < -MAX_STEPS)
		return -EINVAL;
	if (!mutex_trylock(&m->command)) return -EBUSY;
	spin_lock_irqsave(&m->io_lock,flags);
	if (!m->remaining) ret = -EDQUOT;
	else if (!m->armed || time_after_eq(jiffies,m->arm_until)) ret = -EACCES;
	else if (m->io_error) ret = -EIO;
	else if (time_before(jiffies,m->next_move)) ret = -EAGAIN;
	if (ret) {
		spin_unlock_irqrestore(&m->io_lock,flags);
		mutex_unlock(&m->command);
		return ret;
	}
	m->armed=false; m->moving=true; m->aborted=false;
	m->remaining--; m->commands++;
	off(m); pin(m,DIR,requested>0);
	/* Timer is active before VM rises; callback can disable even if the
	 * command task is delayed. This is not a hardware fault/current sensor. */
	hrtimer_start(&m->guard,ms_to_ktime(GUARD_MS),HRTIMER_MODE_REL);
	if (!m->io_error) pin(m,VM,1);
	spin_unlock_irqrestore(&m->io_lock,flags);
	usleep_range(10000,11000); /* Stock rail settling time. */
	spin_lock_irqsave(&m->io_lock,flags);
	if (!m->aborted && !m->io_error) pin(m,ENN,0);
	spin_unlock_irqrestore(&m->io_lock,flags);
	steps = requested > 0 ? requested : -requested;
	for (i=0;i<steps;i++) {
		spin_lock_irqsave(&m->io_lock,flags);
		if (m->aborted || m->io_error) {
			spin_unlock_irqrestore(&m->io_lock,flags); break;
		}
		pin(m,STEP,1);
		if (!m->io_error) {done++; m->pulses++; m->relative_steps += requested>0 ? 1 : -1;}
		spin_unlock_irqrestore(&m->io_lock,flags);
		udelay(50);
		spin_lock_irqsave(&m->io_lock,flags);
		pin(m,STEP,0);
		spin_unlock_irqrestore(&m->io_lock,flags);
		usleep_range(360,500);
	}
	spin_lock_irqsave(&m->io_lock,flags);
	off(m);
	ret = m->io_error ? -EIO : m->aborted ? -ECANCELED : 0;
	m->last_result=ret; m->moving=false;
	m->next_move=jiffies+msecs_to_jiffies(COOLDOWN_MS);
	spin_unlock_irqrestore(&m->io_lock,flags);
	hrtimer_cancel(&m->guard);
	dev_info(dev,"command=%u requested=%d emitted=%d result=%d; motor off\n",m->commands,requested,done,ret);
	mutex_unlock(&m->command);
	return ret ? ret : count;
}
static DEVICE_ATTR_RO(status);
static DEVICE_ATTR_WO(arm);
static DEVICE_ATTR_WO(move);
static DEVICE_ATTR_WO(stop);
static struct attribute *motor_attrs[] = {
	&dev_attr_status.attr,&dev_attr_arm.attr,&dev_attr_move.attr,&dev_attr_stop.attr,NULL
};
ATTRIBUTE_GROUPS(motor);

static int motor_probe(struct platform_device *pdev)
{
	struct motor *m;
	unsigned i;
	u32 saved,changed;
	int ret;
	m=devm_kzalloc(&pdev->dev,sizeof(*m),GFP_KERNEL);
	if (!m) return -ENOMEM;
	m->dev=&pdev->dev;
	mutex_init(&m->command);spin_lock_init(&m->io_lock);
	hrtimer_setup(&m->guard,guard_stop,CLOCK_MONOTONIC,HRTIMER_MODE_REL);
	/* No step edges or power-up: shut VM off first, then disable ENN. */
	for(i=0;i<NPINS;i++) {
		m->gpio[i]=devm_gpiod_get(&pdev->dev,names[i],idle[i]?GPIOD_OUT_HIGH:GPIOD_OUT_LOW);
		if(IS_ERR(m->gpio[i])) return dev_err_probe(&pdev->dev,PTR_ERR(m->gpio[i]),"GPIO %s\n",names[i]);
		if(gpiod_cansleep(m->gpio[i])) return -EOPNOTSUPP;
	}
	/* Shared with pinctrl: no exclusive request_mem_region. GPIO ownership
	 * still belongs to this device via the descriptors above. */
	m->regs=devm_ioremap(&pdev->dev,GPIO_PA,0x1000);
	if(!m->regs)return -ENOMEM;
	/* Validate DOUT aliases with DIR only, VM off and ENN high. No STEP
	 * transitions. Reject if either alias changes any other bank bit. */
	saved=readl(m->regs+DOUT5);
	if((saved&BIT(3))||!(saved&BIT(6))||(saved&BIT(8)))return -EIO;
	pin(m,DIR,1);changed=readl(m->regs+DOUT5);
	if(changed!=(saved|BIT(7)))m->io_error=true;
	pin(m,DIR,0);changed=readl(m->regs+DOUT5);
	if(changed!=(saved&~BIT(7)))m->io_error=true;
	/* Also verify DIR-bank aliases on the disabled driver's DIR pin. */
	saved=readl(m->regs+0x50);
	writel(BIT(7),m->regs+0x58);
	if(readl(m->regs+0x50)!=(saved&~BIT(7)))m->io_error=true;
	writel(BIT(7),m->regs+0x54);
	if(readl(m->regs+0x50)!=saved)m->io_error=true;
	if(m->io_error){
        /* Alias verification failed: use the known native GPIO path. */
        gpiod_set_value(m->gpio[VM],0);
        gpiod_set_value(m->gpio[ENN],1);
        gpiod_set_value(m->gpio[STEP],0);
        return -EIO;
    }
	m->remaining=min_t(unsigned,initial_budget,MOVE_BUDGET);
	platform_set_drvdata(pdev,m);
	ret=sysfs_create_groups(&pdev->dev.kobj,motor_groups);
	if(ret){off(m);return ret;}
	dev_info(&pdev->dev,"ready, motor OFF, no homing; limit=%d steps, budget=%d, GPIO aliases verified with power off\n",MAX_STEPS,MOVE_BUDGET);
	return 0;
}
static void motor_remove(struct platform_device *pdev)
{
	struct motor *m=platform_get_drvdata(pdev);
	unsigned long flags;
	sysfs_remove_groups(&pdev->dev.kobj,motor_groups);
	hrtimer_cancel(&m->guard);
	spin_lock_irqsave(&m->io_lock,flags);off(m);spin_unlock_irqrestore(&m->io_lock,flags);
}
static void motor_shutdown(struct platform_device *pdev)
{
	struct motor *m=platform_get_drvdata(pdev);
	unsigned long flags;
	spin_lock_irqsave(&m->io_lock,flags);
	m->aborted=true;m->armed=false;off(m);
	spin_unlock_irqrestore(&m->io_lock,flags);
	hrtimer_cancel(&m->guard);
}
static const struct of_device_id motor_match[]={{.compatible="relmon,ms35774"},{}};
MODULE_DEVICE_TABLE(of,motor_match);
static struct platform_driver motor_driver={
	.probe=motor_probe,.remove=motor_remove,.shutdown=motor_shutdown,
	.driver={.name="r1-ms35774",.of_match_table=motor_match}
};
static int __init motor_init(void)
{
	int ret;
	if(!of_machine_is_compatible("mediatek,MT6765") && !of_machine_is_compatible("mediatek,mt6765"))return -ENODEV;
	gpiod_add_lookup_table(&lookup);
	ret=platform_driver_register(&motor_driver);
	if(ret)gpiod_remove_lookup_table(&lookup);
	return ret;
}
static void __exit motor_exit(void)
{
	platform_driver_unregister(&motor_driver);gpiod_remove_lookup_table(&lookup);
}
module_init(motor_init);module_exit(motor_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 MS35774 bounded bring-up driver; no automatic motion");
