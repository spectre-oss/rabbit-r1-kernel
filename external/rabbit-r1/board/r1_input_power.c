// SPDX-License-Identifier: GPL-2.0-only
/* Replace the broker's raw VLDO28 enable with native regulator ownership.
 * The existing stock DT names this rail VLDO28; do not change its voltage.
 */
#include <linux/module.h>
#include <linux/regulator/consumer.h>

static struct regulator *vtouch;

static int __init r1_input_power_init(void)
{
	int ret;

	vtouch = regulator_get_optional(NULL, "VLDO28");
	if (IS_ERR(vtouch))
		return PTR_ERR(vtouch);
	ret = regulator_enable(vtouch);
	if (ret) {
		regulator_put(vtouch);
		return ret;
	}
	pr_info("r1_input_power: VLDO28 held by native regulator\n");
	return 0;
}

static void __exit r1_input_power_exit(void)
{
	regulator_disable(vtouch);
	regulator_put(vtouch);
}

module_init(r1_input_power_init);
module_exit(r1_input_power_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 userspace input rail ownership");
