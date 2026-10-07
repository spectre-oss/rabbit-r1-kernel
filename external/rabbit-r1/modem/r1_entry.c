// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
static bool enable;
module_param(enable, bool, 0400);
MODULE_PARM_DESC(enable, "Explicit one-shot registration; clean reboot required for teardown");
int r1_platform_prepare(void);
int r1_prepare_modem_nodes(void);
int __init ccci_init(void);
int __init md_clk_init(void);
int __init ccci_rtc_init(void);
int __init ccci_auxadc_init(void);
int __init ccci_cldma_init(void);
int __init ccci_hif_ccif_init(void);
int __init modem_cd_init(void);
static int __init r1_eccci_init(void)
{
 int ret;
 if (!enable) return -EPERM;
 ret = r1_prepare_modem_nodes();
 if (ret) return ret;
 ret = r1_platform_prepare();
 if (ret) return ret;
 ret = ccci_init();
 if (ret) { pr_err("r1_eccci: ccci_init failed %d; reboot required\n", ret); return 0; }
 ret = md_clk_init();
 if (ret) { pr_err("r1_eccci: md_clk_init failed %d; reboot required\n", ret); return 0; }
 ret = ccci_rtc_init();
 if (ret) { pr_err("r1_eccci: ccci_rtc_init failed %d; reboot required\n", ret); return 0; }
 ret = ccci_auxadc_init();
 if (ret) { pr_err("r1_eccci: ccci_auxadc_init failed %d; reboot required\n", ret); return 0; }
 ret = ccci_cldma_init();
 if (ret) { pr_err("r1_eccci: ccci_cldma_init failed %d; reboot required\n", ret); return 0; }
 ret = ccci_hif_ccif_init();
 if (ret) { pr_err("r1_eccci: ccci_hif_ccif_init failed %d; reboot required\n", ret); return 0; }
 ret = modem_cd_init();
 if (ret) { pr_err("r1_eccci: modem_cd_init failed %d; reboot required\n", ret); return 0; }
 pr_info("r1_eccci: drivers registered, not modem ready\n");
 return 0;
}
module_init(r1_eccci_init);
MODULE_LICENSE("GPL");
