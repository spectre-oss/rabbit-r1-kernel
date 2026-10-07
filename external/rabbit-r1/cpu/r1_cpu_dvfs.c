// SPDX-License-Identifier: GPL-2.0-only
/* Rabbit #45: two fixed-PLL divider policies, one serialized shared rail pair.
 * CCI remains 600 MHz. Board OPP ceilings for CCI/LL are 768750 uV; use at
 * least 775000 uV. CPU0 cluster uses the proven 800000 uV at its 1500 MHz cap.
 */
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include "r1_cpu_voltage.h"
#define LEVELS 6
struct cluster {
 struct clk *pll;
 unsigned long rate;
 u32 offset, boot_divider;
 unsigned int index;
 bool enabled;
 struct cpufreq_frequency_table table[LEVELS+1];
};
static struct cluster clusters[2] = {{.offset=0x7a0},{.offset=0x7a4}};
static struct clk *cci;
static bool cci_enabled;
static void __iomem *mcu;
static struct regulator *vproc, *vsram;
static DEFINE_MUTEX(dvfs_lock);
static const struct { u8 code,num,den; } dividers[LEVELS] = {
 {18,3,5},{26,4,6},{9,3,4},{17,4,5},{25,5,6},{8,1,1}
};
static struct clk *get_pll(unsigned int index)
{
 struct of_phandle_args spec={};
 struct clk *clk;
 spec.np=of_find_compatible_node(NULL,NULL,"mediatek,mt6765-apmixedsys");
 if(!spec.np)return ERR_PTR(-ENODEV);
 spec.args_count=1;spec.args[0]=index;
 clk=of_clk_get_from_provider(&spec);of_node_put(spec.np);
 return clk;
}
static bool valid_opp(const char *path,u64 hz,u32 uv)
{
 struct device_node *np=of_find_node_by_path(path);
 u64 found_hz;u32 found_uv;
 bool valid=np && !of_property_read_u64(np,"opp-hz",&found_hz) &&
  !of_property_read_u32(np,"opp-microvolt",&found_uv) && found_hz==hz && found_uv==uv;
 of_node_put(np);return valid;
}
static int rails(int target)
{
 int op,value,ret,steps=0;
 for(;;) {
  op=r1_voltage_next(regulator_get_voltage(vproc),regulator_get_voltage(vsram),target,&value);
  if(op<=0)return op ? -ERANGE : 0;
  if(++steps>3)return -EIO;
  ret=regulator_set_voltage(op==1?vproc:vsram,value,value);
  if(ret)return ret;
  /* Vendor MT6357 slew 8.33mV/us plus 5us; our maximum step is 25mV. */
  usleep_range(20,30);
 }
}
static unsigned int read_frequency(unsigned int cpu)
{
 struct cluster *c=&clusters[cpu>=4];
 u32 code=(readl(mcu+c->offset)>>17)&31;
 unsigned int i;
 if(cpu>=8)return 0;
 if(code==0)return DIV_ROUND_CLOSEST(c->rate,1000);
 for(i=0;i<LEVELS;i++)if(code==dividers[i].code)return c->table[i].frequency;
 return 0;
}
static int write_divider(struct cluster *c,u32 code)
{
 u32 v=readl(mcu+c->offset);
 if(((v>>9)&3)!=1)return -EIO;
 writel((v&~GENMASK(21,17))|(code<<17),mcu+c->offset);
 return ((readl(mcu+c->offset)>>17)&31)==code ? 0 : -EIO;
}
static int target(struct cpufreq_policy *policy,unsigned int index)
{
 unsigned int id=policy->cpu>=4;
 struct cluster *c=&clusters[id];
 int voltage,ret;
 mutex_lock(&dvfs_lock);
 if(index>=LEVELS || clk_get_rate(cci)<599998000 || clk_get_rate(cci)>600002000 ||
    clk_get_rate(clusters[0].pll)!=clusters[0].rate ||
    clk_get_rate(clusters[1].pll)!=clusters[1].rate) {ret=-ERANGE;goto out;}
 /* Both clusters and CCI vote; LL's entire bounded range fits the CCI floor. */
 voltage=(id==0 ? index : clusters[0].index)==LEVELS-1 ? 800000 : 775000;
 if(voltage>regulator_get_voltage(vproc)) {
  ret=rails(voltage);if(ret)goto out;
 }
 ret=write_divider(c,dividers[index].code);if(ret)goto out;
 c->index=index;
 ret=rails(voltage);
 if(ret) {
  /* Applied slower/same clock remains valid with the retained higher rail.
   * Report the actual clock transition; never speed up after a failed raise.
   */
  pr_err_ratelimited("r1-dvfs: rail adjustment failed %d; clock applied, retaining safe voltage\n",ret);
  ret=0;
 }
out:
 mutex_unlock(&dvfs_lock);return ret;
}
static int policy_init(struct cpufreq_policy *policy)
{
 unsigned int id=policy->cpu>=4,cpu;
 if(policy->cpu>=8)return -ENODEV;
 policy->freq_table=clusters[id].table;
 policy->cpuinfo.transition_latency=300000;
 cpumask_clear(policy->cpus);
 for(cpu=id*4;cpu<id*4+4;cpu++)if(cpu_possible(cpu))cpumask_set_cpu(cpu,policy->cpus);
 policy->min=policy->max=clusters[id].table[LEVELS-1].frequency;
 return 0;
}
static struct cpufreq_driver driver={
 .name="r1-dvfs",.flags=CPUFREQ_NEED_INITIAL_FREQ_CHECK,
 .verify=cpufreq_generic_frequency_table_verify,.init=policy_init,
 .get=read_frequency,.target_index=target,
};
static void release(void)
{
 unsigned int i;
 for(i=0;i<2;i++) {
  if(clusters[i].enabled)clk_disable_unprepare(clusters[i].pll);
  if(!IS_ERR_OR_NULL(clusters[i].pll))clk_put(clusters[i].pll);
 }
 if(cci_enabled)clk_disable_unprepare(cci);
 if(!IS_ERR_OR_NULL(cci))clk_put(cci);
 if(!IS_ERR_OR_NULL(vproc))regulator_put(vproc);
 if(!IS_ERR_OR_NULL(vsram))regulator_put(vsram);
 if(mcu)iounmap(mcu);
}
static int __init init(void)
{
 unsigned int i,j;int ret;u32 v;
 if(!of_machine_is_compatible("mediatek,MT6765") || num_possible_cpus()!=8)return -ENODEV;
 if(!valid_opp("/opp_table0/opp6",1617000000ULL,768750) ||
    !valid_opp("/opp_table1/opp6",948000000ULL,768750) ||
    !valid_opp("/opp_table2/opp06",658000000ULL,768750))return -EINVAL;
 mcu=ioremap(0x10200000,0x1000);if(!mcu)return -ENOMEM;
 for(i=0;i<2;i++) {
  struct cluster *c=&clusters[i];
  v=readl(mcu+c->offset);c->boot_divider=(v>>17)&31;
  if(((v>>9)&3)!=1 || c->boot_divider!=0){ret=-EINVAL;goto fail;}
  c->pll=get_pll(i);if(IS_ERR(c->pll)){ret=PTR_ERR(c->pll);goto fail;}
  c->rate=clk_get_rate(c->pll);
  if(abs((long)c->rate-(i?920000000L:1500000000L))>2000){ret=-ERANGE;goto fail;}
  c->index=LEVELS-1;
  for(j=0;j<LEVELS;j++)c->table[j].frequency=DIV_ROUND_CLOSEST_ULL((u64)c->rate*dividers[j].num,1000*dividers[j].den);
  c->table[LEVELS].frequency=CPUFREQ_TABLE_END;
  ret=clk_prepare_enable(c->pll);if(ret)goto fail;c->enabled=true;
 }
 cci=get_pll(2);if(IS_ERR(cci)){ret=PTR_ERR(cci);goto fail;}
 if(abs((long)clk_get_rate(cci)-600000000L)>2000){ret=-ERANGE;goto fail;}
 ret=clk_prepare_enable(cci);if(ret)goto fail;cci_enabled=true;
 vproc=regulator_get_optional(NULL,"VPROC");if(IS_ERR(vproc)){ret=PTR_ERR(vproc);goto fail;}
 vsram=regulator_get_optional(NULL,"VSRAM_PROC");if(IS_ERR(vsram)){ret=PTR_ERR(vsram);goto fail;}
 if(regulator_get_voltage(vproc)!=800000 || regulator_get_voltage(vsram)!=900000){ret=-ERANGE;goto fail;}
 ret=regulator_set_voltage(vsram,900000,900000);if(ret)goto fail;
 ret=regulator_set_voltage(vproc,800000,800000);if(ret)goto fail;
 ret=cpufreq_register_driver(&driver);if(ret)goto fail;
 pr_info("r1-dvfs: two divider policies; shared VPROC 775-800mV, VSRAM +100mV; fixed CCI 600MHz\n");
 return 0;
fail:release();return ret;
}
static void __exit fini(void)
{
 int ret;unsigned int i;
 cpufreq_unregister_driver(&driver);
 mutex_lock(&dvfs_lock);
 ret=rails(800000);
 if(!ret)for(i=0;i<2;i++) {
  ret=write_divider(&clusters[i],clusters[i].boot_divider);
  if(ret)break;
 }
 if(ret)pr_err("r1-dvfs: safe boot restoration failed %d; retain current dividers, reboot required\n",ret);
 mutex_unlock(&dvfs_lock);
 release();
}
module_init(init);module_exit(fini);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit dual-cluster divider and coordinated shared-rail CPU DVFS");
