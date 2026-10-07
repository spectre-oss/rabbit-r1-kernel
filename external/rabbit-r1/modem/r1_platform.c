// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <mt-plat/mtk-clkbuf-bridge.h>
#include <mtk_pbm.h>
static void __iomem *spm;
static bool flightmode = true;
static bool modem_budget_active;
int r1_platform_prepare(void)
{
 struct device_node *np = of_find_node_by_path("/sleep@10006000");
 if (!np) return -ENODEV;
 spm = of_iomap(np, 0);
 of_node_put(np);
 return spm ? 0 : -ENOMEM;
}
bool spm_is_md1_sleep(void)
{
 /* Stock MT6765 pcm_def.h: R13_MD1_SRCCLKENA bit 2, REG13 offset 0x134. */
 return spm && !(readl(spm + 0x134) & BIT(2));
}
u32 mt_irq_get_pending(unsigned int irq)
{
 bool pending;
 int ret = irq_get_irqchip_state(irq, IRQCHIP_STATE_PENDING, &pending);
 if (ret) { pr_err("r1_eccci: IRQ pending query failed %d\n", ret); return 1; }
 return pending;
}
/* MT6765 clkbuf_v1 has no platform BBLPM hook; maintain its software state.
 * Oscillator gating is retained from LK, not advertised as managed PM.
 */
enum clk_buf_ret_type clk_buf_set_by_flightmode(bool on)
{
 flightmode = on;
 return CLK_BUF_OK;
}
/* PBM is vendor CPU/GPU budget policy, not the hardware MD power switch.
 * This bring-up kernel has no DVFS policy. Expose state without claiming it.
 */
void kicker_pbm_by_md(enum pbm_kicker kicker, bool on)
{
 modem_budget_active = on;
 pr_info("r1_eccci: MD budget active=%d; dynamic power budgeting unavailable\n", on);
}
int switch_sim_mode(int id, char *buf, unsigned int len) { return -EOPNOTSUPP; }
unsigned int get_sim_switch_type(void) { return 0; } /* no switch driver, stock default */
int mbim_start_xmit(struct sk_buff *skb, int ifid)
{
 dev_kfree_skb_any(skb);
 return -EOPNOTSUPP; /* No MBIM USB gadget, never silently report transmission. */
}
int set_rps_map(struct netdev_rx_queue *queue, unsigned long mask)
{
 return -EOPNOTSUPP; /* Vendor RPS tuning is not implemented. */
}
