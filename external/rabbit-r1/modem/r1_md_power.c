// SPDX-License-Identifier: GPL-2.0
/* MT6765 MD1 sequence from stock clk-mt6765-pg.c, under native genpd,
 * regulator and regmap ownership. Every hardware wait is bounded.
 */
#include <linux/module.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/delay.h>
static struct generic_pm_domain md_domain;
static struct regmap *md_spm, *md_infra;
static struct regulator *md_supply, *core_supply;
static struct device *md_device;
static bool prepared, active;
static DEFINE_MUTEX(md_lock);
/* A running modem retains firmware and network state across AP suspend.
 * System suspend must not perform the destructive runtime shutdown sequence.
 * Explicit modem stop still uses runtime PM and powers the domain off. */
static bool md_system_power_down_ok(struct dev_pm_domain *domain)
{
 return !READ_ONCE(active);
}
static struct dev_power_governor md_governor = {
 .system_power_down_ok = md_system_power_down_ok,
};
#define CHECK(expr) do { int e = (expr); if (e) return e; } while (0)
static int status_wait(unsigned int off, u32 mask, u32 expected)
{
 u32 value;
 return regmap_read_poll_timeout(md_spm, off, value, (value & mask) == expected, 10, 100000);
}
static int protect(unsigned int set, unsigned int ack, u32 mask)
{
 u32 value;
 CHECK(regmap_write(md_infra, set, mask));
 return regmap_read_poll_timeout(md_infra, ack, value, (value & mask) == mask, 10, 100000);
}
static int domain_off(struct generic_pm_domain *domain)
{
 CHECK(protect(0x2a0, 0x228, BIT(7)));
 CHECK(protect(0x2a0, 0x228, BIT(3) | BIT(4)));
 CHECK(protect(0x2a8, 0x258, BIT(6)));
 CHECK(regmap_set_bits(md_spm, 0x398, BIT(0)));
 CHECK(regmap_set_bits(md_spm, 0x320, BIT(4)));
 CHECK(regmap_set_bits(md_spm, 0x320, BIT(1)));
 CHECK(regmap_clear_bits(md_spm, 0x394, BIT(0)));
 CHECK(regmap_set_bits(md_spm, 0x320, BIT(8)));
 CHECK(regmap_clear_bits(md_spm, 0x320, BIT(2)));
 CHECK(regmap_clear_bits(md_spm, 0x320, BIT(3)));
 CHECK(status_wait(0x180, BIT(0), 0));
 CHECK(status_wait(0x184, BIT(0), 0));
 CHECK(regmap_clear_bits(md_spm, 0x320, BIT(0)));
 pr_info("r1_eccci: MD1 domain off\n");
 return 0;
}
static int domain_on(struct generic_pm_domain *domain)
{
 int ret;
 CHECK(regmap_clear_bits(md_spm, 0x320, BIT(0)));
 CHECK(regmap_set_bits(md_spm, 0x320, BIT(2)));
 CHECK(regmap_set_bits(md_spm, 0x320, BIT(3)));
 ret = status_wait(0x180, BIT(0), BIT(0));
 if (!ret) ret = status_wait(0x184, BIT(0), BIT(0));
 if (ret) { domain_off(domain); return ret; }
 CHECK(regmap_clear_bits(md_spm, 0x320, BIT(8)));
 CHECK(regmap_set_bits(md_spm, 0x394, BIT(0)));
 CHECK(regmap_clear_bits(md_spm, 0x320, BIT(1)));
 CHECK(regmap_clear_bits(md_spm, 0x320, BIT(4)));
 CHECK(regmap_set_bits(md_spm, 0x320, BIT(0)));
 CHECK(regmap_clear_bits(md_spm, 0x398, BIT(0)));
 /* Stock specifically does not wait for release ACK. */
 CHECK(regmap_write(md_infra, 0x2a4, BIT(3) | BIT(4)));
 CHECK(regmap_write(md_infra, 0x2ac, BIT(6)));
 CHECK(regmap_write(md_infra, 0x2a4, BIT(7)));
 pr_info("r1_eccci: MD1 domain on\n");
 return 0;
}
static struct regmap *map_node(const char *path)
{
 struct device_node *node = of_find_node_by_path(path);
 struct regmap *map;
 if (!node) return ERR_PTR(-ENODEV);
 map = syscon_node_to_regmap(node);
 of_node_put(node);
 return map;
}
int r1_md_power_prepare(struct device *dev)
{
 struct device_node *other;
 unsigned int status, status2;
 int ret;
 if (prepared) return md_device == dev ? 0 : -EBUSY;
 /* Do not compete with a domain registered by the primary provider. */
 other = of_find_node_by_path("/sleep@10006000/power-controller/power-domain@0");
 if (other) { of_node_put(other); return -EBUSY; }
 md_spm = map_node("/sleep@10006000");
 if (IS_ERR(md_spm)) return PTR_ERR(md_spm);
 md_infra = map_node("/infracfg_ao@10001000");
 if (IS_ERR(md_infra)) return PTR_ERR(md_infra);
 CHECK(regmap_read(md_spm, 0x180, &status));
 CHECK(regmap_read(md_spm, 0x184, &status2));
 if ((status | status2) & BIT(0)) return -EBUSY;
 /* These exact native provider names were checked on this unit. Optional-get
  * refuses the dummy regulator. No old underscore DT supply aliases are used. */
 md_supply = devm_regulator_get_optional(dev, "VMODEM");
 if (IS_ERR(md_supply)) return PTR_ERR(md_supply);
 core_supply = devm_regulator_get_optional(dev, "r1-buck-vcore");
 if (IS_ERR(core_supply)) return PTR_ERR(core_supply);
 CHECK(regulator_set_voltage(md_supply, 800000, 800000));
 CHECK(regulator_set_voltage(core_supply, 800000, 900000));
 /* Stock MT6765 AP power code does not own VSIM; MD firmware sequences
  * SIM power and voltage negotiation. Do not force a rail from the AP. */
 md_domain.name = "r1-md1";
 md_domain.power_on = domain_on;
 md_domain.power_off = domain_off;
 ret = pm_genpd_init(&md_domain, &md_governor, true);
 if (ret) return ret;
 ret = pm_genpd_add_device(&md_domain, dev);
 if (ret) { pm_genpd_remove(&md_domain); return ret; }
 pm_runtime_set_suspended(dev);
 pm_runtime_enable(dev);
 md_device = dev;
 prepared = true;
 pr_info("r1_eccci: native MD1 power prepared, domain remains off\n");
 return 0;
}
int r1_md_power_on(struct device *dev)
{
 int ret;
 mutex_lock(&md_lock);
 if (!prepared || md_device != dev) { ret = -ENODEV; goto done; }
 if (active) { ret = -EALREADY; goto done; }
 ret = regulator_enable(core_supply);
 if (ret) goto done;
 ret = regulator_enable(md_supply);
 if (ret) goto disable_core;
 ret = pm_runtime_resume_and_get(dev);
 if (ret < 0) goto disable_md;
 active = true;
 ret = 0;
 goto done;
disable_md:
 regulator_disable(md_supply);
disable_core:
 regulator_disable(core_supply);
done:
 mutex_unlock(&md_lock);
 return ret;
}
int r1_md_power_off(struct device *dev)
{
 int ret = 0;
 mutex_lock(&md_lock);
 if (active && md_device == dev) {
  ret = pm_runtime_put_sync_suspend(dev);
  if (ret < 0) goto done; /* retain rails if domain could not be isolated */
  regulator_disable(md_supply);
  regulator_disable(core_supply);
  active = false;
  ret = 0;
 }
done:
 mutex_unlock(&md_lock);
 return ret;
}
