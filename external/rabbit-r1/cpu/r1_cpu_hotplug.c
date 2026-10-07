// SPDX-License-Identifier: GPL-2.0-only
/* Balance the Rabbit boot lock; runtime offlining requires vendor completion. */
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/of.h>
#include <linux/utsname.h>
#include <linux/mutex.h>
#include "r1_cpu_power.h"
static bool sealed, initialized, runtime;
/* Enable only together with r1_cpu_power's post-PSCI completion callback. */
module_param(runtime,bool,0400);
static DEFINE_MUTEX(seal_lock);
static int set_sealed(const char *value,const struct kernel_param *kp)
{
 bool requested;int ret=kstrtobool(value,&requested);
 if(ret)return ret;
 mutex_lock(&seal_lock);
 if(!requested) { ret=sealed?-EPERM:0;goto out; }
 if(sealed)goto out;
 if(!initialized){ret=-EINVAL;goto out;}
 cpu_hotplug_disable();
 if(num_online_cpus()!=8){cpu_hotplug_enable();ret=-EBUSY;goto out;}
 sealed=true;
 pr_info("r1-cpu-hotplug: all eight cores sealed online; architectural WFI remains available\n");
out:mutex_unlock(&seal_lock);return ret;
}
static const struct kernel_param_ops seal_ops={.set=set_sealed,.get=param_get_bool};
module_param_cb(sealed,&seal_ops,&sealed,0600);
static int __init init(void)
{
 if(runtime && !r1_cpu_power_ready())return -ENODEV;
 if(!of_machine_is_compatible("mediatek,MT6765") ||
    (!strstr(init_utsname()->version,"#45 ") &&
     !strstr(init_utsname()->version,"#46 ") &&
     !strstr(init_utsname()->version,"#47 SMP PREEMPT Mon Sep 28 11:10:56 CEST 2026") &&
     !strstr(init_utsname()->version,"#48 SMP PREEMPT Mon Sep 28 11:17:10 CEST 2026") &&
     !strstr(init_utsname()->version,"#49 SMP PREEMPT Mon Sep 28 11:50:35 CEST 2026")) || num_possible_cpus()!=8 ||
    !cpu_online(0) || (!runtime && num_online_cpus()!=1 && num_online_cpus()!=8))return -ENODEV;
 if(num_online_cpus()==8 && !runtime) {
  /* Prior sealed instance retained the boot lock; do not reopen it on restart. */
  sealed=true;initialized=true;return 0;
 }
 cpu_hotplug_enable();initialized=true;
 pr_info("r1-cpu-hotplug: balanced Rabbit #45 lock for initial CPU onlining\n");
 return 0;
}
static void __exit fini(void)
{
 if(!sealed)cpu_hotplug_disable();
 pr_info("r1-cpu-hotplug: hotplug lock retained; online CPUs unchanged\n");
}
module_init(init);module_exit(fini);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit #45 initial SMP onlining with guarded WFI-only residency");
