// SPDX-License-Identifier: GPL-2.0
/* Rabbit R1 ICM-42607-P: polling, native SPI4 and pinctrl, no raw MMIO. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/spi/spi.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/machine.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>

struct r1_imu {
	struct spi_device *spi;
	struct mutex lock;
	bool powered;
	/* kmalloc-backed, DMA-safe even if the controller changes its threshold. */
	u8 tx[16] __aligned(IIO_DMA_MINALIGN);
	u8 rx[16] __aligned(IIO_DMA_MINALIGN);
};
static struct spi_device *client;
static struct platform_device *pins_device;
static struct pinctrl *pins;
static struct pinctrl_state *active_pins, *stock_pins;
static int probe_result = -ENODEV;
#define MAP(state, pin, fn) PIN_MAP_MUX_GROUP("r1-imu-pins", state, \
	"10005000.pinctrl", "GPIO" #pin, "func" #fn)
static const struct pinctrl_map pin_maps[] = {
	MAP("active",13,2), MAP("active",14,2),
	MAP("active",15,2), MAP("active",16,2),
	MAP("stock",13,3), MAP("stock",14,3),
	MAP("stock",15,3), MAP("stock",16,3),
};

/* Caller holds lock, or is in exclusive probe/remove. Full duplex keeps CS. */
static int read_regs(struct r1_imu *s, u8 reg, u8 *out, size_t n)
{
	struct spi_transfer t = { .tx_buf = s->tx, .rx_buf = s->rx, .len = n + 1 };
	int ret;
	if (n > 15) return -EINVAL;
	memset(s->tx, 0, sizeof(s->tx));
	s->tx[0] = reg | 0x80;
	ret = spi_sync_transfer(s->spi, &t, 1);
	if (!ret) memcpy(out, s->rx + 1, n);
	return ret;
}
static int write_reg(struct r1_imu *s, u8 reg, u8 val)
{
	s->tx[0] = reg;
	s->tx[1] = val;
	return spi_write(s->spi, s->tx, 2);
}
static int power_off(struct r1_imu *s)
{
	u8 value;
	int ret = write_reg(s, 0x1f, 0);
	s->powered = false;
	msleep(150);
	if (!ret) {
		ret = read_regs(s, 0x1f, &value, 1);
		if (!ret && value != 0) ret = -EIO;
	}
	return ret;
}
static int start_sensor(struct r1_imu *s)
{
	u8 value;
	int ret, i;
	ret = read_regs(s, 0x75, &value, 1);
	if (ret) return ret;
	if (value != 0x60) return -ENODEV;
	ret = write_reg(s, 0x02, 0x10);
	if (ret) return ret;
	usleep_range(1000, 1500);
	for (i = 0; i < 10; i++) {
		ret = read_regs(s, 0x3a, &value, 1);
		if (ret) return ret;
		if (value & BIT(4)) break;
		usleep_range(1000, 1500);
	}
	if (i == 10) return -ETIMEDOUT;
	ret = read_regs(s, 0x75, &value, 1);
	if (ret || value != 0x60) return ret ? ret : -ENODEV;
	ret = read_regs(s, 0x35, &value, 1);
	if (ret || (value & 0x30) != 0x30) return ret ? ret : -EIO;
	ret = write_reg(s, 0x20, 0x06);
	if (ret) return ret;
	ret = write_reg(s, 0x21, 0x06);
	if (ret) return ret;
	s->powered = true; /* Also attempt shutdown after an uncertain write. */
	ret = write_reg(s, 0x1f, 0x0f);
	if (ret) return ret;
	msleep(50);
	ret = read_regs(s, 0x1f, &value, 1);
	if (ret || value != 0x0f) return ret ? ret : -EIO;
	return 0;
}
static int sample(struct r1_imu *s, s16 values[7])
{
	u8 raw[14];
	int i, ret;
	if (!s->powered) return -EHOSTDOWN;
	ret = read_regs(s, 0x09, raw, sizeof(raw));
	if (ret) return ret;
	for (i = 0; i < 7; i++) {
		values[i] = (s16)get_unaligned_be16(raw + 2 * i);
		if (values[i] == -32768) return -EAGAIN;
	}
	return 0;
}
static int imu_read_raw(struct iio_dev *indio, const struct iio_chan_spec *ch,
		int *val, int *val2, long mask)
{
	struct r1_imu *s = iio_priv(indio);
	s16 v[7];
	int ret;
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&s->lock);
		ret = sample(s, v);
		mutex_unlock(&s->lock);
		if (ret) return ret;
		*val = v[ch->address];
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		if (ch->type == IIO_TEMP) { *val = 7; *val2 = 812500; }
		else if (ch->type == IIO_ACCEL) { *val = 0; *val2 = 4788403; }
		else { *val = 0; *val2 = 1064225; }
		return ch->type == IIO_TEMP ? IIO_VAL_INT_PLUS_MICRO : IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_OFFSET:
		*val = 3200;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = 800;
		return IIO_VAL_INT;
	default: return -EINVAL;
	}
}
static ssize_t sample_raw_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio = dev_to_iio_dev(dev);
	struct r1_imu *s = iio_priv(indio);
	s16 v[7];
	s64 timestamp;
	int ret;
	mutex_lock(&s->lock);
	ret = sample(s, v);
	timestamp = iio_get_time_ns(indio);
	mutex_unlock(&s->lock);
	if (ret) return ret;
	return sysfs_emit(buf, "%lld %d %d %d %d %d %d %d\n", timestamp,
		v[1],v[2],v[3],v[4],v[5],v[6],v[0]);
}
static ssize_t who_am_i_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct r1_imu *s = iio_priv(dev_to_iio_dev(dev));
	u8 value;
	int ret;
	mutex_lock(&s->lock);
	ret = read_regs(s, 0x75, &value, 1);
	mutex_unlock(&s->lock);
	return ret ? ret : sysfs_emit(buf, "0x%02x\n", value);
}
static IIO_DEVICE_ATTR_RO(sample_raw, 0);
static IIO_DEVICE_ATTR_RO(who_am_i, 0);
static struct attribute *imu_attrs[] = {
	&iio_dev_attr_sample_raw.dev_attr.attr,
	&iio_dev_attr_who_am_i.dev_attr.attr, NULL,
};
static const struct attribute_group imu_group = { .attrs = imu_attrs };
static const struct iio_info imu_info = { .read_raw = imu_read_raw, .attrs = &imu_group };
#define AXIS(t, mod, addr) { .type = t, .modified = 1, .channel2 = mod, \
	.address = addr, .info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_SAMP_FREQ) }
static const struct iio_chan_spec channels[] = {
	AXIS(IIO_ACCEL,IIO_MOD_X,1), AXIS(IIO_ACCEL,IIO_MOD_Y,2), AXIS(IIO_ACCEL,IIO_MOD_Z,3),
	AXIS(IIO_ANGL_VEL,IIO_MOD_X,4), AXIS(IIO_ANGL_VEL,IIO_MOD_Y,5), AXIS(IIO_ANGL_VEL,IIO_MOD_Z,6),
	{ .type = IIO_TEMP, .address = 0, .info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
		BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET) },
};
static int imu_probe(struct spi_device *spi)
{
	struct iio_dev *indio;
	struct r1_imu *s;
	int ret;
	indio = devm_iio_device_alloc(&spi->dev, sizeof(*s));
	if (!indio) return probe_result = -ENOMEM;
	s = iio_priv(indio);
	s->spi = spi;
	mutex_init(&s->lock);
	spi_set_drvdata(spi, indio);
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret) return probe_result = ret;
	ret = start_sensor(s);
	if (ret) goto fail;
	indio->name = "icm42607p";
	indio->info = &imu_info;
	indio->modes = INDIO_DIRECT_MODE;
	indio->channels = channels;
	indio->num_channels = ARRAY_SIZE(channels);
	ret = iio_device_register(indio);
	if (ret) goto fail;
	dev_info(&spi->dev, "ICM-42607-P WHO_AM_I=0x60, six axes at 800 Hz, polling\n");
	return probe_result = 0;
fail:
	if (s->powered) power_off(s);
	return probe_result = ret;
}
static void imu_remove(struct spi_device *spi)
{
	struct iio_dev *indio = spi_get_drvdata(spi);
	struct r1_imu *s = iio_priv(indio);
	iio_device_unregister(indio);
	mutex_lock(&s->lock);
	if (power_off(s)) dev_err(&spi->dev, "sensor power-down failed\n");
	else dev_info(&spi->dev, "sensor power-down readback=0x00\n");
	mutex_unlock(&s->lock);
}
static int imu_suspend(struct device *dev)
{
	struct r1_imu *s = iio_priv(spi_get_drvdata(to_spi_device(dev)));
	int ret;
	mutex_lock(&s->lock);
	ret = power_off(s);
	mutex_unlock(&s->lock);
	return ret;
}
static void imu_shutdown(struct spi_device *spi)
{
	if (imu_suspend(&spi->dev))
		dev_err(&spi->dev, "shutdown power-down failed\n");
}
static int imu_resume(struct device *dev)
{
	struct r1_imu *s = iio_priv(spi_get_drvdata(to_spi_device(dev)));
	int ret;
	mutex_lock(&s->lock);
	ret = start_sensor(s);
	if (ret && s->powered) power_off(s);
	mutex_unlock(&s->lock);
	return ret;
}
static DEFINE_SIMPLE_DEV_PM_OPS(imu_pm, imu_suspend, imu_resume);
static const struct spi_device_id imu_ids[] = { {"r1_icm42607",0}, {} };
MODULE_DEVICE_TABLE(spi, imu_ids);
static struct spi_driver imu_driver = {
	.driver = { .name = "r1_icm42607", .pm = pm_sleep_ptr(&imu_pm) },
	.probe = imu_probe, .remove = imu_remove, .shutdown = imu_shutdown, .id_table = imu_ids,
};
static void release_pins(void)
{
	if (!IS_ERR_OR_NULL(pins)) {
		if (!IS_ERR_OR_NULL(stock_pins) && pinctrl_select_state(pins, stock_pins))
			pr_err("r1_icm42607: cannot restore stock pinmux\n");
		pinctrl_put(pins);
	}
	if (!IS_ERR_OR_NULL(pins_device)) platform_device_unregister(pins_device);
	pinctrl_unregister_mappings(pin_maps);
}
static int __init imu_init(void)
{
	struct device_node *node;
	struct platform_device *pdev;
	struct spi_controller *ctlr;
	struct spi_board_info info = { .modalias = "r1_icm42607", .max_speed_hz = 1000000,
		.chip_select = 0, .mode = SPI_MODE_0 };
	u32 clocks[6], pad;
	int ret;
	/* Reject a claimed SCP, wrong clock cell or unexpected platform. */
	node = of_find_node_by_path("/scp@10500000");
	if (!node) return -ENODEV;
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (pdev) {
		bool busy = pdev->dev.driver != NULL;
		put_device(&pdev->dev);
		if (busy) return -EBUSY;
	}
	node = of_find_node_by_path("/spi@11014000");
	if (!node) return -ENODEV;
	ret = of_property_read_u32_array(node, "clocks", clocks, 6);
	if (!ret) ret = of_property_read_u32(node, "mediatek,pad-select", &pad);
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (ret || clocks[5] != 48 || pad != 0 || !pdev) {
		if (pdev) put_device(&pdev->dev);
		return -ENODEV;
	}
	device_lock(&pdev->dev);
	if (!pdev->dev.driver || strcmp(pdev->dev.driver->name, "mtk-spi")) {
		ret = -ENODEV;
		goto unlock;
	}
	ctlr = spi_controller_get(platform_get_drvdata(pdev));
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	if (!ctlr) return -ENODEV;
	ret = pinctrl_register_mappings(pin_maps, ARRAY_SIZE(pin_maps));
	if (ret) goto put_ctlr;
	pins_device = platform_device_register_simple("r1-imu-pins", -1, NULL, 0);
	if (IS_ERR(pins_device)) { ret = PTR_ERR(pins_device); goto pins_fail; }
	pins = pinctrl_get(&pins_device->dev);
	if (IS_ERR(pins)) { ret = PTR_ERR(pins); goto pins_fail; }
	stock_pins = pinctrl_lookup_state(pins, "stock");
	active_pins = pinctrl_lookup_state(pins, "active");
	if (IS_ERR(stock_pins) || IS_ERR(active_pins)) { ret = -EINVAL; goto pins_fail; }
	ret = pinctrl_select_state(pins, active_pins);
	if (ret) goto pins_fail;
	ret = spi_register_driver(&imu_driver);
	if (ret) goto pins_fail;
	client = spi_new_device(ctlr, &info);
	ret = client ? probe_result : -ENODEV;
	if (!ret) { spi_controller_put(ctlr); return 0; }
	if (client) spi_unregister_device(client);
	spi_unregister_driver(&imu_driver);
pins_fail:
	release_pins();
put_ctlr:
	spi_controller_put(ctlr);
	return ret;
unlock:
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	return ret;
}
static void __exit imu_exit(void)
{
	spi_unregister_device(client);
	spi_unregister_driver(&imu_driver);
	release_pins();
}
module_init(imu_init);
module_exit(imu_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 ICM-42607-P native SPI4 polling IIO driver");
