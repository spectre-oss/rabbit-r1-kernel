// SPDX-License-Identifier: GPL-2.0
/* GPIO-only access to the R1 Type-C I2C bus. No charger probe, IRQ or MMIO. */
#include <linux/module.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/machine.h>
#include <linux/platform_device.h>

static struct gpio_device *gdev;
static struct gpio_desc *scl, *sda;
static struct platform_device *pinsdev;
static struct pinctrl *pins;
static const struct pinctrl_map maps[] = {
 PIN_MAP_MUX_GROUP("r1-typec-pins", "stock", "10005000.pinctrl", "GPIO48", "func1"),
 PIN_MAP_MUX_GROUP("r1-typec-pins", "stock", "10005000.pinctrl", "GPIO49", "func1"),
};
static void setsda(void *data, int value) { gpiod_set_value(sda, value); }
static void setscl(void *data, int value) { gpiod_set_value(scl, value); }
static int getsda(void *data) { return gpiod_get_value(sda); }
static int getscl(void *data) { return gpiod_get_value(scl); }
static struct i2c_algo_bit_data bit = {
 .setsda=setsda, .setscl=setscl, .getsda=getsda, .getscl=getscl,
 .udelay=10, .timeout=HZ/10,
};
static struct i2c_adapter adapter = {
 .owner=THIS_MODULE, .name="R1 Type-C GPIO I2C", .algo_data=&bit,
};
static void cleanup(void)
{
 if (!IS_ERR_OR_NULL(sda)) gpiochip_free_own_desc(sda);
 if (!IS_ERR_OR_NULL(scl)) gpiochip_free_own_desc(scl);
 if (!IS_ERR_OR_NULL(pins)) {
  struct pinctrl_state *s = pinctrl_lookup_state(pins, "stock");
  if (!IS_ERR(s)) pinctrl_select_state(pins, s);
  pinctrl_put(pins);
 }
 if (!IS_ERR_OR_NULL(pinsdev)) platform_device_unregister(pinsdev);
 pinctrl_unregister_mappings(maps);
 if (gdev) gpio_device_put(gdev);
}
static int __init start(void)
{
 struct gpio_chip *chip;
 int ret;
 gdev=gpio_device_find_by_label("pinctrl_paris");
 if (!gdev) return -ENODEV;
 chip=gpio_device_get_chip(gdev);
 ret=pinctrl_register_mappings(maps, ARRAY_SIZE(maps));
 if (ret) { gpio_device_put(gdev); return ret; }
 pinsdev=platform_device_register_simple("r1-typec-pins", -1, NULL, 0);
 if (IS_ERR(pinsdev)) { ret=PTR_ERR(pinsdev); goto fail; }
 pins=pinctrl_get(&pinsdev->dev);
 if (IS_ERR(pins)) { ret=PTR_ERR(pins); goto fail; }
 scl=gpiochip_request_own_desc(chip,48,"r1-typec-scl",GPIO_OPEN_DRAIN,GPIOD_OUT_HIGH);
 if (IS_ERR(scl)) { ret=PTR_ERR(scl); goto fail; }
 sda=gpiochip_request_own_desc(chip,49,"r1-typec-sda",GPIO_OPEN_DRAIN,GPIOD_OUT_HIGH);
 if (IS_ERR(sda)) { ret=PTR_ERR(sda); goto fail; }
 if (!getscl(NULL) || !getsda(NULL)) { ret=-EBUSY; goto fail; }
 adapter.dev.parent=&pinsdev->dev;
 ret=i2c_bit_add_bus(&adapter);
 if (ret) goto fail;
 pr_info("r1_typec_bus: i2c-%d ready; no clients probed\n",adapter.nr);
 return 0;
fail:
 cleanup(); return ret;
}
static void __exit stop(void) { i2c_del_adapter(&adapter); cleanup(); }
module_init(start);
module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("R1 bounded GPIO I2C bus for Type-C bring-up");
