/* SPDX-License-Identifier: GPL-2.0 */
#ifndef R1_CCCI_GPIO_H
#define R1_CCCI_GPIO_H
#include <linux/of.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/consumer.h>
/* Legacy lookup only: preserve provider-relative -> global ID translation.
 * Does not request, drive or reconfigure a GPIO. Ownership belongs to caller.
 */
static inline int r1_of_get_named_gpio(struct device_node *np, const char *prop, int index)
{
 struct of_phandle_args args;
 struct gpio_device *gdev;
 struct gpio_desc *desc;
 int ret = of_parse_phandle_with_args(np, prop, "#gpio-cells", index, &args);
 if (ret) return ret;
 if (args.args_count != 2) { of_node_put(args.np); return -EINVAL; }
 gdev = gpio_device_find_by_fwnode(of_fwnode_handle(args.np));
 of_node_put(args.np);
 if (!gdev) return -EPROBE_DEFER;
 desc = gpio_device_get_desc(gdev, args.args[0]);
 ret = IS_ERR(desc) ? PTR_ERR(desc) : desc_to_gpio(desc);
 gpio_device_put(gdev);
 return ret;
}
#endif
