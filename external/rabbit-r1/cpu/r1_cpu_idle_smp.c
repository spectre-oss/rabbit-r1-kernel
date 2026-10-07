// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/cpuidle.h>
#include <linux/cpuhotplug.h>
#include <linux/of.h>
#include <asm/cpuidle.h>
static struct cpuidle_device __percpu *devices;
static int hp_state;
static int enter_wfi(struct cpuidle_device *dev,struct cpuidle_driver *drv,int index)
{
 struct arm_cpuidle_irq_context context;
 arm_cpuidle_save_irq_context(&context);dsb(sy);wfi();
 arm_cpuidle_restore_irq_context(&context);return index;
}
/* As in haltpoll: exit removes devices synchronously, no self-pinning owner. */
static struct cpuidle_driver driver={
 .name="r1-smp-wfi",.state_count=1,
 .states={{.name="WFI",.desc="Architectural WFI",.exit_latency=1,
 .target_residency=1,.enter=enter_wfi}},
};
static int online(unsigned int cpu)
{
 struct cpuidle_device *d=per_cpu_ptr(devices,cpu);
 d->cpu=cpu;return cpuidle_register_device(d);
}
static int offline(unsigned int cpu)
{
 cpuidle_unregister_device(per_cpu_ptr(devices,cpu));return 0;
}
static int __init init(void)
{
 int ret;
 if(!of_machine_is_compatible("mediatek,MT6765") || num_possible_cpus()!=8)return -ENODEV;
 devices=alloc_percpu(struct cpuidle_device);if(!devices)return -ENOMEM;
 driver.cpumask=(struct cpumask *)cpu_possible_mask;
 ret=cpuidle_register_driver(&driver);if(ret)goto free;
 hp_state=cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,"cpuidle/r1-wfi:online",online,offline);
 if(hp_state<0){ret=hp_state;cpuidle_unregister_driver(&driver);goto free;}
 return 0;
free:free_percpu(devices);return ret;
}
static void __exit fini(void)
{
 cpuhp_remove_state(hp_state);cpuidle_unregister_driver(&driver);free_percpu(devices);
}
module_init(init);module_exit(fini);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit multicore architectural WFI accounting");
