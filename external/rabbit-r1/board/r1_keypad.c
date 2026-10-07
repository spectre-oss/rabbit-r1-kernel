// SPDX-License-Identifier: GPL-2.0-only
/* MT6765 keypad path recovered from shipped 0.8.293 Image and DTBO.
 * Stock kpd_keymap_handler reads five 16-bit banks; the overlay maps
 * hardware position 1 to KEY_POWER. No PMIC register accesses here.
 */
#include <linux/clk.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>

#define R1_KP_KEYS 72
#define R1_KP_BANKS 5
#define R1_KP_MEM 0x04
#define R1_KP_DEBOUNCE 0x18

struct r1_keypad {
	void __iomem *base;
	struct input_dev *input;
	struct clk *clk;
	u32 keymap[R1_KP_KEYS];
	u16 state[R1_KP_BANKS];
};
static unsigned int irqs, events, mem1;
module_param(irqs, uint, 0444);
module_param(events, uint, 0444);
module_param(mem1, uint, 0444);

static irqreturn_t r1_keypad_irq(int irq, void *data)
{
	struct r1_keypad *kp = data;
	unsigned int i, bit, code;
	bool changed = false;

	irqs++;
	for (i = 0; i < R1_KP_BANKS; i++) {
		u16 state = readw(kp->base + R1_KP_MEM + 4 * i);
		u16 diff = state ^ kp->state[i];

		if (!i)
			mem1 = state;
		for (bit = 0; bit < 16 && i * 16 + bit < R1_KP_KEYS; bit++) {
			if (!(diff & BIT(bit)))
				continue;
			code = kp->keymap[i * 16 + bit];
			if (!code)
				continue;
			input_report_key(kp->input, code, !(state & BIT(bit)));
			dev_info(kp->input->dev.parent,
				 "hardware key %u code=%u %s MEM%u=%04x\n",
				 i * 16 + bit, code,
				 state & BIT(bit) ? "release" : "press", i + 1, state);
			events++;
			changed = true;
		}
		kp->state[i] = state;
	}
	if (changed)
		input_sync(kp->input);
	return IRQ_HANDLED;
}

static void r1_keypad_clock_off(void *data)
{
	clk_disable_unprepare(data);
}

static int r1_keypad_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct r1_keypad *kp;
	struct pinctrl *pins;
	u32 count, debounce;
	int i, irq, ret;

	kp = devm_kzalloc(dev, sizeof(*kp), GFP_KERNEL);
	if (!kp)
		return -ENOMEM;
	ret = of_property_read_u32(dev->of_node, "mediatek,kpd-hw-map-num", &count);
	if (ret || !count || count > R1_KP_KEYS)
		return -EINVAL;
	ret = of_property_read_u32_array(dev->of_node, "mediatek,kpd-hw-init-map",
					kp->keymap, count);
	if (ret)
		return ret;
	for (i = 0; i < count; i++)
		if (kp->keymap[i] > KEY_MAX)
			return -EINVAL;
	ret = of_property_read_u32(dev->of_node, "mediatek,kpd-key-debounce", &debounce);
	if (ret)
		return ret;
	kp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(kp->base))
		return PTR_ERR(kp->base);
	kp->clk = devm_clk_get(dev, "kpd");
	if (IS_ERR(kp->clk))
		return dev_err_probe(dev, PTR_ERR(kp->clk), "keypad clock\n");
	ret = clk_prepare_enable(kp->clk);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, r1_keypad_clock_off, kp->clk);
	if (ret)
		return ret;
	pins = devm_pinctrl_get_select_default(dev);
	if (IS_ERR(pins))
		return dev_err_probe(dev, PTR_ERR(pins), "keypad pinctrl\n");
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	kp->input = devm_input_allocate_device(dev);
	if (!kp->input)
		return -ENOMEM;
	kp->input->name = "r1-keypad";
	kp->input->phys = "mt6765/keypad";
	kp->input->id.bustype = BUS_HOST;
	for (i = 0; i < count; i++)
		if (kp->keymap[i])
			input_set_capability(kp->input, EV_KEY, kp->keymap[i]);
	/* Match shipped halfword accesses and debounce value; preserve KP_EN/SEL. */
	writew(debounce & 0x3fff, kp->base + R1_KP_DEBOUNCE);
	for (i = 0; i < R1_KP_BANKS; i++)
		kp->state[i] = readw(kp->base + R1_KP_MEM + 4 * i);
	mem1 = kp->state[0];
	ret = input_register_device(kp->input);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(dev, irq, NULL, r1_keypad_irq,
					IRQF_ONESHOT, "r1-keypad", kp);
	if (ret)
		return dev_err_probe(dev, ret, "keypad IRQ\n");
	platform_set_drvdata(pdev, kp);
	dev_info(dev, "stock keypad: irq=%d map[1]=%u debounce=%x MEM1=%04x\n",
		 irq, kp->keymap[1], debounce & 0x3fff, kp->state[0]);
	return 0;
}

static const struct of_device_id r1_keypad_match[] = {
	{ .compatible = "mediatek,kp" },
	{ }
};
MODULE_DEVICE_TABLE(of, r1_keypad_match);
static struct platform_driver r1_keypad_driver = {
	.probe = r1_keypad_probe,
	.driver = {
		.name = "r1-keypad",
		.of_match_table = r1_keypad_match,
	},
};
module_platform_driver(r1_keypad_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 stock-derived MT6765 keypad");
