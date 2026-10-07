// SPDX-License-Identifier: GPL-2.0-only
/*
 * Orient Chip OCH1970 — Rabbit R1 side-wheel hall sensor.
 *
 * Stock rabbitOS binds this on i2c6 @ 0x0d (reset GPIO 173) as
 * input name och1970_holl_key and reports KEY_UP / KEY_DOWN (103/108).
 * GSI keylayout maps those to volume. Algorithm matches vendor
 * drivers/input/touchscreen/och1970.c (XYZ → degree → keys).
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/of.h>

#define OCH1970_REG_WIA		0x00
#define OCH1970_REG_DATAXYZ	0x17
#define OCH1970_REG_CNTL2	0x21

#define OCH1970_STANDBY		0x00
#define OCH1970_CONT_100HZ	0x0C
#define OCH1970_CNTL2_SDR	BIT(4)	/* 0 = low noise */
#define OCH1970_CNTL2_SMR	BIT(5)	/* 1 = high range */

#define MAX_X_VALUE		7200
#define DEG_STEP		30
#define STALE_MS		3000

struct och1970 {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset;
	struct delayed_work work;
	int degree;
	u64 pre_ms;
	int x, y, z;
};

/* sin(270°) … sin(89°) * 10000 — same table as stock och1970.c */
static const int sin_mul10000[180] = {
	-10000, -9998, -9994, -9986, -9976, -9962, -9945, -9925, -9903, -9877,
	-9848, -9816, -9781, -9744, -9703, -9659, -9613, -9563, -9511, -9455,
	-9397, -9336, -9272, -9205, -9135, -9063, -8988, -8910, -8829, -8746,
	-8660, -8572, -8480, -8387, -8290, -8192, -8090, -7986, -7880, -7771,
	-7660, -7547, -7431, -7314, -7193, -7071, -6947, -6820, -6691, -6561,
	-6428, -6293, -6157, -6018, -5878, -5736, -5592, -5446, -5299, -5150,
	-5000, -4848, -4695, -4540, -4384, -4226, -4067, -3907, -3746, -3584,
	-3420, -3256, -3090, -2924, -2756, -2588, -2419, -2250, -2079, -1908,
	-1736, -1564, -1392, -1219, -1045, -872, -698, -523, -349, -175,
	0, 175, 349, 523, 698, 872, 1045, 1219, 1392, 1564,
	1736, 1908, 2079, 2250, 2419, 2588, 2756, 2924, 3090, 3256,
	3420, 3584, 3746, 3907, 4067, 4226, 4384, 4540, 4695, 4848,
	5000, 5150, 5299, 5446, 5592, 5736, 5878, 6018, 6157, 6293,
	6428, 6561, 6691, 6820, 6947, 7071, 7193, 7314, 7431, 7547,
	7660, 7771, 7880, 7986, 8090, 8192, 8290, 8387, 8480, 8572,
	8660, 8746, 8829, 8910, 8988, 9063, 9135, 9205, 9272, 9336,
	9397, 9455, 9511, 9563, 9613, 9659, 9703, 9744, 9781, 9816,
	9848, 9877, 9903, 9925, 9945, 9962, 9976, 9986, 9994, 9998,
};

static int och_write(struct och1970 *och, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = {
		.addr = och->client->addr,
		.buf = buf,
		.len = 2,
	};
	int ret = i2c_transfer(och->client->adapter, &msg, 1);

	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static int och_read(struct och1970 *och, u8 reg, u8 *data, int len)
{
	struct i2c_msg msg[2] = {
		{
			.addr = och->client->addr,
			.buf = &reg,
			.len = 1,
		},
		{
			.addr = och->client->addr,
			.flags = I2C_M_RD,
			.buf = data,
			.len = len,
		},
	};
	int ret = i2c_transfer(och->client->adapter, msg, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int och_xyz_to_degree(int x, int z)
{
	int sinx, index;

	if (x > MAX_X_VALUE)
		x = MAX_X_VALUE;
	else if (x < -MAX_X_VALUE)
		x = -MAX_X_VALUE;
	sinx = x * 10000 / MAX_X_VALUE;
	for (index = 0; index < 179 && sinx > sin_mul10000[index]; index++)
		;
	if (index < 90) {
		if (z < 0)
			return 270 - index;
		return index + 270;
	}
	if (z < 0)
		return 270 - index;
	return index - 90;
}

static int och_key_for_delta(int old_deg, int new_deg)
{
	int d;

	if (old_deg < DEG_STEP) {
		if (new_deg >= old_deg + DEG_STEP &&
		    new_deg < old_deg + 180)
			return KEY_DOWN;
		if (new_deg <= old_deg + 360 - DEG_STEP &&
		    new_deg >= old_deg + 180)
			return KEY_UP;
		return -1;
	}
	if (old_deg + DEG_STEP >= 360) {
		if (new_deg >= old_deg + DEG_STEP - 360 &&
		    new_deg <= old_deg - 180)
			return KEY_DOWN;
		if (new_deg <= old_deg - DEG_STEP &&
		    new_deg > old_deg - 180)
			return KEY_UP;
		return -1;
	}
	d = abs(old_deg - new_deg);
	if (d < DEG_STEP)
		return -1;
	if (new_deg > old_deg)
		return (new_deg - old_deg <= 180) ? KEY_DOWN : KEY_UP;
	return (old_deg - new_deg <= 180) ? KEY_UP : KEY_DOWN;
}

static void och1970_work(struct work_struct *work)
{
	struct och1970 *och = container_of(work, struct och1970, work.work);
	u8 buf[8];
	int x, y, z, deg, key;
	u64 now;

	if (och_read(och, OCH1970_REG_DATAXYZ, buf, 8))
		goto again;

	x = (s16)((buf[2] << 8) | buf[3]);
	y = (s16)((buf[4] << 8) | buf[5]);
	z = (s16)((buf[6] << 8) | buf[7]);
	och->x = x;
	och->y = y;
	och->z = z;
	deg = och_xyz_to_degree(x, z);
	now = ktime_to_ms(ktime_get());

	input_report_abs(och->input, ABS_X, x);
	input_report_abs(och->input, ABS_Y, y);
	input_report_abs(och->input, ABS_Z, z);
	input_report_abs(och->input, ABS_MISC, deg);
	input_sync(och->input);

	if (och->degree == 360) {
		och->degree = deg;
		och->pre_ms = now;
		goto again;
	}

	if (now - och->pre_ms >= STALE_MS) {
		och->degree = deg;
		och->pre_ms = now;
		goto again;
	}

	key = och_key_for_delta(och->degree, deg);
	if (key >= 0) {
		input_report_key(och->input, key, 1);
		input_sync(och->input);
		input_report_key(och->input, key, 0);
		input_sync(och->input);
		input_report_rel(och->input, REL_WHEEL,
				 key == KEY_UP ? 1 : -1);
		input_sync(och->input);
		dev_info(&och->client->dev, "wheel %s deg=%d xyz=%d,%d,%d\n",
			 key == KEY_UP ? "UP" : "DOWN", deg, x, y, z);
		och->degree = deg;
		och->pre_ms = now;
	}

again:
	schedule_delayed_work(&och->work, HZ / 20);
}

static void och1970_reset(struct och1970 *och)
{
	if (!och->reset)
		return;
	/* Stock: line 0 (assert) then 1 (release). Overlay flags = 0. */
	gpiod_set_value_cansleep(och->reset, 0);
	msleep(10);
	gpiod_set_value_cansleep(och->reset, 1);
	msleep(10);
}

static int och1970_setup(struct och1970 *och)
{
	u8 cntl;
	int err;

	err = och_read(och, OCH1970_REG_CNTL2, &cntl, 1);
	if (err)
		return err;
	cntl = (cntl & 0xf0) | OCH1970_STANDBY;
	err = och_write(och, OCH1970_REG_CNTL2, cntl);
	if (err)
		return err;
	cntl = (cntl & 0xf0) | OCH1970_CONT_100HZ;
	cntl &= ~OCH1970_CNTL2_SDR;
	cntl |= OCH1970_CNTL2_SMR;
	return och_write(och, OCH1970_REG_CNTL2, cntl);
}

static int och1970_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct och1970 *och;
	struct input_dev *input;
	u8 wia[4] = { };
	int err;

	och = devm_kzalloc(dev, sizeof(*och), GFP_KERNEL);
	if (!och)
		return -ENOMEM;
	och->client = client;
	och->degree = 360;
	i2c_set_clientdata(client, och);

	och->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(och->reset)) {
		dev_warn(dev, "reset-gpios: %ld (continue without)\n",
			 PTR_ERR(och->reset));
		och->reset = NULL;
	}
	och1970_reset(och);

	err = och_read(och, OCH1970_REG_WIA, wia, 4);
	if (err) {
		dev_err(dev, "WIA read failed: %d (i2c6 0x%02x)\n",
			err, client->addr);
		return err;
	}
	dev_info(dev, "WIA %02x %02x %02x %02x\n",
		 wia[0], wia[1], wia[2], wia[3]);

	err = och1970_setup(och);
	if (err)
		return dev_err_probe(dev, err, "CNTL2 setup\n");

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	och->input = input;
	input->name = "och1970_holl_key";
	input->phys = "i2c/och1970";
	input->id.bustype = BUS_I2C;
	input_set_drvdata(input, och);
	__set_bit(EV_KEY, input->evbit);
	__set_bit(KEY_UP, input->keybit);
	__set_bit(KEY_DOWN, input->keybit);
	__set_bit(EV_REL, input->evbit);
	__set_bit(REL_WHEEL, input->relbit);
	input_set_abs_params(input, ABS_X, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_Y, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_Z, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_MISC, 0, 359, 0, 0);

	err = input_register_device(input);
	if (err)
		return err;

	INIT_DELAYED_WORK(&och->work, och1970_work);
	schedule_delayed_work(&och->work, HZ / 5);
	dev_info(dev, "wheel hall @ 0x%02x, poll 50ms, KEY_UP/DOWN\n",
		 client->addr);
	return 0;
}

static void och1970_remove(struct i2c_client *client)
{
	struct och1970 *och = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&och->work);
}

static const struct i2c_device_id och1970_id[] = {
	{ "och1970", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, och1970_id);

static const struct of_device_id och1970_of[] = {
	{ .compatible = "orientchip,och1970" },
	{ .compatible = "och1970" },
	{ }
};
MODULE_DEVICE_TABLE(of, och1970_of);

static struct i2c_driver och1970_driver = {
	.driver = {
		.name = "och1970",
		.of_match_table = och1970_of,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = och1970_id,
	.probe = och1970_probe,
	.remove = och1970_remove,
};
module_i2c_driver(och1970_driver);

MODULE_AUTHOR("rabbit r1 bring-up");
MODULE_DESCRIPTION("Orient Chip OCH1970 hall scroll wheel");
MODULE_LICENSE("GPL");

/*
 * Attribution retained from the vendor reference used for the wheel algorithm:
 * drivers/input/touchscreen/och1970.c (SPDX-License-Identifier: GPL-2.0+)
 * Copyright (c) 2015-2018 Red Hat Inc.
 * Red Hat authors: Hans de Goede <hdegoede@redhat.com>
 * Reference SHA256: 956969380bcf5c3b5143c67130e8eb41ad1f59e1dcdce5074e7d3eb05e7ab64b
 */
