// SPDX-License-Identifier: GPL-2.0-only
/* MT6765 vendor hps_v3 post-PSCI power-down completion. */
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/of.h>
#include <linux/arm-smccc.h>
#include "r1_cpu_power.h"
static int hp_state=-1;
bool r1_cpu_power_ready(void) { return hp_state>=0; }
EXPORT_SYMBOL_GPL(r1_cpu_power_ready);
static long smc(u32 fn,unsigned long arg)
{
 struct arm_smccc_res r;
 arm_smccc_smc(fn,arg,0,0,0,0,0,0,&r);return (long)r.a0;
}
static int dead(unsigned int cpu)
{
 unsigned int i,cluster=cpu/4;
 long ret;
 if(cpu>=8 || cpu_online(cpu))return -EINVAL;
 /* This BP callback follows generic PSCI kill verification. Never issue
  * vendor power-down against an online or still-running core. */
 ret=smc(0xc4000004,((cpu/4)<<8)|(cpu%4));
 if(ret!=1){pr_err("r1-cpu-power: CPU%u not firmware OFF (%ld)\n",cpu,ret);return -EBUSY;}
 ret=smc(0xc2000212,cpu);
 pr_info("r1-cpu-power: CPU%u power-down completion=%ld\n",cpu,ret);
 if(ret)return -EIO;
 for(i=cluster*4;i<cluster*4+4;i++)if(cpu_online(i))return 0;
 ret=smc(0xc2000210,cluster);
 pr_info("r1-cpu-power: cluster%u power-down completion=%ld\n",cluster,ret);
 return ret ? -EIO : 0;
}
static int __init power_init(void)
{
 if(!of_machine_is_compatible("mediatek,MT6765") || num_possible_cpus()!=8)return -ENODEV;
 hp_state=cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN,"r1/cpu-power",NULL,dead);
 return hp_state<0?hp_state:0;
}
static void __exit power_exit(void){cpuhp_remove_state_nocalls(hp_state);}
module_init(power_init);module_exit(power_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6765 post-PSCI core and cluster power-down completion");
