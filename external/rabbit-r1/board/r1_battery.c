// SPDX-License-Identifier: GPL-2.0
/* Rabbit R1 battery power supply on the MT6357 PMIC.
 *
 * Voltage and NTC temperature come from the native MT6357 AUXADC IIO
 * channels referenced by the stock "mediatek,mt6357-gauge" node. Current is
 * the MT6357 FGADC instantaneous reading using the shipped register sequence.
 * State of charge is seeded from the shipped temperature-interpolated profiles,
 * then advanced by FGADC CAR deltas. Rested voltage can re-anchor the estimate.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/iio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6357/registers.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/ktime.h>

#define AUXADC_CHAN_BATADC	0
#define AUXADC_CHAN_BAT_TEMP	3
#define FG_ON			BIT(0)
#define FG_SW_READ_PRE		BIT(0)
#define FG_SW_CLEAR		BIT(3)
#define FG_LATCHDATA_ST		BIT(15)
#define RGS_CHRDET		BIT(4)
#define RGS_BATON_UNDET		BIT(1)
#define UNIT_FGCURRENT_NA	314331
#define TABLES			4
#define POLL_MS			5000
#define FULL_CURRENT_UA		60000
#define CHARGING_CURRENT_UA	20000
#define REST_CURRENT_UA		20000
#define REST_SECONDS		300
#define DESIGN_UAH		1000000
#define VOLTAGE_MIN_DESIGN_UV	3400000
#define VOLTAGE_MAX_DESIGN_UV	4450000

struct profile_row { u32 mah; u32 volt; u32 res; };
struct profile { int temp; unsigned int n; struct profile_row *rows; };

struct r1_battery {
	struct device *dev;
	struct regmap *map;
	struct power_supply *psy;
	struct iio_channel *chan_v;
	struct iio_channel *chan_t;
	struct profile tables[TABLES];
	unsigned int ntables;
	u32 r_fg, meter_res, pull_up_r, pull_up_mv, car_tune;
	struct mutex lock;
	struct delayed_work poll;
	int volt_uv, cur_ua, temp_dc, soc, soc_filtered_x100, status, health;
	bool present, online, fg_io_error, have_temp;
	unsigned long updates;
	s64 charge_nah, car_nah, last_car_nah, car_delta_nah, last_car_sec;
	s64 rest_start_sec, full_start_sec;
	int rest_min_uv, rest_max_uv, ocv_soc_x100;
	bool car_valid, seeded, rested;
	u32 car_raw;
	unsigned int reseeds, discontinuities;

};

static const struct { int temp; u32 ohm; } ntc_10k[] = {
	{ -40, 195652 }, { -35, 148171 }, { -30, 113347 }, { -25, 87559 },
	{ -20, 68237 }, { -15, 53650 }, { -10, 42506 }, { -5, 33892 },
	{ 0, 27219 }, { 5, 22021 }, { 10, 17926 }, { 15, 14674 },
	{ 20, 12081 }, { 25, 10000 }, { 30, 8315 }, { 35, 6948 },
	{ 40, 5834 }, { 45, 4917 }, { 50, 4161 }, { 55, 3535 }, { 60, 3014 },
};

static int adc_mv(struct iio_channel *chan, int *mv)
{
	int raw, val, val2 = 1, ret, bits;
	s64 tmp;

	ret = iio_read_channel_raw(chan, &raw);
	if (ret < 0)
		return ret;
	ret = iio_read_channel_scale(chan, &val, &val2);
	if (ret < 0)
		return ret;
	if (ret != IIO_VAL_FRACTIONAL)
		val2 = 1;
	bits = chan->channel->scan_type.realbits;
	tmp = (s64)raw * val;
	tmp = div_s64(tmp, val2 ? val2 : 1);
	*mv = (int)(tmp >> bits);
	return 0;
}

static int fg_latch(struct r1_battery *b, bool want)
{
	unsigned int v;
	int i;

	for (i = 0; i < 1000; i++) {
		if (regmap_read(b->map, MT6357_FGADC_CON1, &v))
			return -EIO;
		if (!!(v & FG_LATCHDATA_ST) == want)
			return 0;
		usleep_range(20, 50);
	}
	return -ETIMEDOUT;
}

/* Vendor mt6357_gauge.c: bits 30:11, sign bit 31, one's-complement
 * negative magnitude. One retained count = 11176 nAh (vendor 0.1 mAh
 * conversion multiplies by 11176 / 100000). Do not use the stale 0.085
 * uAh comment or round the absolute counter to 0.1 mAh before subtracting.
 */
static s64 decode_car_nah(u32 raw, u32 r_fg, u32 tune)
{
	s64 counts = (raw >> 11) & 0xfffff;

	if (raw & BIT(31))
		counts -= 0xfffff;
	return div_s64(div_s64(counts * 11176 * 100, r_fg) * tune, 1000);
}

static int read_fg(struct r1_battery *b, int *ua, s64 *car, u32 *raw)
{
	unsigned int reg, lo = 0, hi = 0;
	int ret, err;
	s64 tmp;

	ret = regmap_update_bits(b->map, MT6357_FGADC_CON1, FG_SW_READ_PRE, FG_SW_READ_PRE);
	if (ret)
		return ret;
	ret = fg_latch(b, true);
	if (!ret)
		ret = regmap_read(b->map, MT6357_FGADC_CUR_CON0, &reg);
	if (!ret)
		ret = regmap_read(b->map, MT6357_FGADC_CAR_CON0, &lo);
	if (!ret)
		ret = regmap_read(b->map, MT6357_FGADC_CAR_CON1, &hi);
	err = regmap_update_bits(b->map, MT6357_FGADC_CON1, FG_SW_CLEAR, FG_SW_CLEAR);
	err |= regmap_update_bits(b->map, MT6357_FGADC_CON1, FG_SW_READ_PRE, 0);
	err |= fg_latch(b, false);
	err |= regmap_update_bits(b->map, MT6357_FGADC_CON1, FG_SW_CLEAR, 0);
	if (ret)
		return ret;
	if (err)
		return -EIO;
	/* The instantaneous current is also one's-complement, per vendor. */
	tmp = (reg & 0xffff);
	if (tmp & 0x8000)
		tmp -= 0xffff;
	tmp = div_s64(tmp * UNIT_FGCURRENT_NA * 100, b->r_fg);
	tmp = div_s64(tmp * b->car_tune, 1000);
	*ua = (int)div_s64(tmp, 1000);
	*raw = ((hi & 0xffff) << 16) | (lo & 0xffff);
	*car = decode_car_nah(*raw, b->r_fg, b->car_tune);
	return 0;
}

static int ntc_temp_dc(struct r1_battery *b, int mv)
{
	u64 r;
	int i, span;

	if (mv <= 0 || mv >= (int)b->pull_up_mv)
		return mv <= 0 ? 600 : -400;
	r = (u64)b->pull_up_r * mv;
	r = div_u64(r, b->pull_up_mv - mv);
	if (r >= ntc_10k[0].ohm)
		return ntc_10k[0].temp * 10;
	for (i = 1; i < ARRAY_SIZE(ntc_10k); i++) {
		if (r >= ntc_10k[i].ohm) {
			span = ntc_10k[i - 1].ohm - ntc_10k[i].ohm;
			return ntc_10k[i].temp * 10 -
			       (int)div_u64((r - ntc_10k[i].ohm) * 50, span);
		}
	}
	return ntc_10k[ARRAY_SIZE(ntc_10k) - 1].temp * 10;
}

static int table_soc_x100(struct r1_battery *b, const struct profile *t, int volt_uv, int cur_ua)
{
	unsigned int i;
	s64 ocv;
	u32 qmax;

	if (!t->n)
		return -ENODATA;
	qmax = t->rows[t->n - 1].mah;
	if (!qmax)
		return -ENODATA;
	for (i = 0; i < t->n; i++) {
		const struct profile_row *row = &t->rows[i];
		s64 r_mohm = ((s64)row->res + b->r_fg + b->meter_res);
		s64 tv_uv = (s64)row->volt * 100;

		ocv = volt_uv - div_s64((s64)cur_ua * r_mohm, 10000);
		if (ocv >= tv_uv) {
			if (i == 0)
				return 10000;
			{
				const struct profile_row *hi = &t->rows[i - 1];
				s64 hv = (s64)hi->volt * 100, lv = tv_uv;
				s64 mah;

				if (hv == lv)
					return clamp_t(s64, 10000 - div_s64((s64)row->mah * 10000, qmax), 0, 10000);
				mah = hi->mah + div_s64((s64)(row->mah - hi->mah) * (hv - ocv), hv - lv);

				return (int)clamp_t(s64, 10000 - div_s64(mah * 10000, qmax), 0, 10000);
			}
		}
	}
	return 0;
}

/* Bracket even if DT tables are not ordered; clamp outside the endpoints. */
static void bracket_tables(struct r1_battery *b, int temp_dc,
			   const struct profile **lo, const struct profile **hi)
{
	unsigned int i;

	*lo = *hi = NULL;
	for (i = 0; i < b->ntables; i++) {
		const struct profile *t = &b->tables[i];
		if (t->temp * 10 <= temp_dc && (!*lo || t->temp > (*lo)->temp))
			*lo = t;
		if (t->temp * 10 >= temp_dc && (!*hi || t->temp < (*hi)->temp))
			*hi = t;
	}
	if (!*lo)
		*lo = *hi;
	if (!*hi)
		*hi = *lo;
}

static int blend(int a, int z, int temp_dc,
		 const struct profile *lo, const struct profile *hi)
{
	if (lo == hi)
		return a;
	return a + div_s64((s64)(z - a) * (temp_dc - lo->temp * 10),
			  (hi->temp - lo->temp) * 10);
}

static int lookup_soc_x100(struct r1_battery *b, int volt_uv, int cur_ua, int temp_dc)
{
	const struct profile *lo, *hi;
	int a, z;

	bracket_tables(b, temp_dc, &lo, &hi);
	a = table_soc_x100(b, lo, volt_uv, cur_ua);
	z = table_soc_x100(b, hi, volt_uv, cur_ua);
	if (a < 0 || z < 0)
		return -ENODATA;
	return blend(a, z, temp_dc, lo, hi);
}

static int full_voltage_uv(struct r1_battery *b)
{
	const struct profile *lo, *hi;

	bracket_tables(b, b->temp_dc, &lo, &hi);
	return blend(lo->rows[0].volt * 100, hi->rows[0].volt * 100,
		     b->temp_dc, lo, hi);
}

static void seed_soc(struct r1_battery *b, int soc)
{
	b->charge_nah = (s64)DESIGN_UAH * 1000 * soc / 10000;
	b->seeded = true;
	b->reseeds++;
}

static void advance_soc(struct r1_battery *b, s64 now, bool valid)
{
	s64 dt, delta;
	int soc;

	if (!valid) {
		/* A failed poll must not count toward a continuous rest interval.
		 * Retain the last good CAR baseline to bridge short read failures. */
		b->rest_start_sec = 0;
		b->rested = false;
		return;
	}
	soc = lookup_soc_x100(b, b->volt_uv, b->cur_ua, b->temp_dc);
	if (soc < 0)
		return;
	b->ocv_soc_x100 = soc;
	b->car_delta_nah = 0;
	if (!b->seeded) {
		/* At module load the cell may be loaded: provisional compensated
		 * OCV seed, explicitly not a claim of a rested measurement. */
		seed_soc(b, soc);
	} else if (b->car_valid) {
		dt = now - b->last_car_sec;
		delta = b->car_nah - b->last_car_nah;
		/* A 2 A envelope plus two count quantization allowance rejects
		 * counter resets/wraps. Re-anchor rather than integrating a jump.
		 * Long gaps (> one hour) cannot be validated by this envelope. */
		if (dt < 0 || dt > 3600 ||
		    abs(delta) > div_s64(2000000000LL * dt, 3600) + 22352) {
			b->discontinuities++;
			seed_soc(b, soc);
			b->rest_start_sec = 0;
			b->rested = false;
		} else {
			b->car_delta_nah = delta;
			b->charge_nah += delta;
		}
	}
	b->last_car_nah = b->car_nah;
	b->last_car_sec = now;
	b->car_valid = true;
	if (abs(b->cur_ua) <= REST_CURRENT_UA) {
		if (!b->rest_start_sec) {
			b->rest_start_sec = now;
			b->rest_min_uv = b->rest_max_uv = b->volt_uv;
		}
		b->rest_min_uv = min(b->rest_min_uv, b->volt_uv);
		b->rest_max_uv = max(b->rest_max_uv, b->volt_uv);
		if (b->rest_max_uv - b->rest_min_uv > 10000) {
			b->rest_start_sec = now;
			b->rest_min_uv = b->rest_max_uv = b->volt_uv;
			b->rested = false;
		} else if (!b->rested && now - b->rest_start_sec >= REST_SECONDS) {
			seed_soc(b, lookup_soc_x100(b, b->volt_uv, 0, b->temp_dc));
			b->rested = true;
		}
	} else {
		b->rest_start_sec = 0;
		b->rested = false;
	}
	b->charge_nah = clamp_t(s64, b->charge_nah, 0, (s64)DESIGN_UAH * 1000);
	b->soc_filtered_x100 = div_s64(b->charge_nah * 10000, (s64)DESIGN_UAH * 1000);
	b->soc = (b->soc_filtered_x100 + 50) / 100;
}

/* Require a full minute of valid, detected, tapered input at the profile's
 * full voltage. A transient high ADC sample must not fill the estimate. */
static int battery_status(struct r1_battery *b, s64 now, bool valid, bool chrdet)
{
	bool full = valid && chrdet && b->volt_uv >= full_voltage_uv(b) &&
		    b->cur_ua >= 0 && b->cur_ua < FULL_CURRENT_UA;

	if (!full)
		b->full_start_sec = 0;
	else if (!b->full_start_sec)
		b->full_start_sec = now;
	if (!valid)
		return POWER_SUPPLY_STATUS_UNKNOWN;
	if (!chrdet)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	if (full && now - b->full_start_sec >= 60)
		return POWER_SUPPLY_STATUS_FULL;
	if (b->cur_ua > CHARGING_CURRENT_UA)
		return POWER_SUPPLY_STATUS_CHARGING;
	return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static void update(struct r1_battery *b)
{
	unsigned int reg;
	int mv, cur = 0, status;
	bool chrdet = false, present = false, valid, det_valid;

	mutex_lock(&b->lock);
	valid = !adc_mv(b->chan_v, &mv);
	if (valid)
		b->volt_uv = mv * 1000;
	if (b->have_temp) {
		if (!adc_mv(b->chan_t, &mv))
			b->temp_dc = ntc_temp_dc(b, mv);
		else
			valid = false;
	}
	b->fg_io_error = !!read_fg(b, &cur, &b->car_nah, &b->car_raw);
	if (!b->fg_io_error)
		b->cur_ua = cur;
	else
		valid = false;
	det_valid = !regmap_read(b->map, MT6357_CHR_TOP_CON0, &reg);
	if (det_valid)
		chrdet = reg & RGS_CHRDET;
	if (!regmap_read(b->map, MT6357_BATON_ANA_CON0, &reg))
		present = !(reg & RGS_BATON_UNDET);
	else
		valid = false;
	b->present = present;
	b->online = chrdet;

	status = battery_status(b, ktime_get_boottime_seconds(),
				valid && det_valid && present, chrdet);
	b->status = status;

	advance_soc(b, ktime_get_boottime_seconds(), valid && present);
	if (status == POWER_SUPPLY_STATUS_FULL && b->seeded) {
		b->charge_nah = (s64)DESIGN_UAH * 1000;
		b->soc_filtered_x100 = 10000;
		b->soc = 100;
	}
	if (b->temp_dc > 600)
		b->health = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (b->temp_dc < -100)
		b->health = POWER_SUPPLY_HEALTH_COLD;
	else if (b->volt_uv > VOLTAGE_MAX_DESIGN_UV + 100000)
		b->health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	else
		b->health = POWER_SUPPLY_HEALTH_GOOD;
	b->updates++;
	mutex_unlock(&b->lock);
}

static void poll_work(struct work_struct *work)
{
	struct r1_battery *b = container_of(work, struct r1_battery, poll.work);
	int soc = b->soc, status = b->status;
	bool online = b->online;

	update(b);
	if (soc != b->soc || status != b->status || online != b->online)
		power_supply_changed(b->psy);
	schedule_delayed_work(&b->poll, msecs_to_jiffies(POLL_MS));
}

static enum power_supply_property props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
};

static int get_property(struct power_supply *psy, enum power_supply_property psp,
			union power_supply_propval *val)
{
	struct r1_battery *b = power_supply_get_drvdata(psy);

	mutex_lock(&b->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = b->status;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = b->health;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = b->present;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = b->online;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = b->volt_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = VOLTAGE_MAX_DESIGN_UV;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = VOLTAGE_MIN_DESIGN_UV;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = b->cur_ua;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = b->soc;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		if (b->status == POWER_SUPPLY_STATUS_FULL)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		else if (b->soc <= 5)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		else if (b->soc <= 15)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else if (b->soc >= 95)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
		else
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = DESIGN_UAH;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = (int)div_u64((u64)DESIGN_UAH * b->soc_filtered_x100, 10000);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = b->temp_dc;
		break;
	default:
		mutex_unlock(&b->lock);
		return -EINVAL;
	}
	mutex_unlock(&b->lock);
	return 0;
}

static ssize_t debug_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct r1_battery *b = power_supply_get_drvdata(dev_get_drvdata(dev));
	const struct profile *lo, *hi;
	unsigned int con0 = 0, top = 0, baton = 0;
	int vmv = 0, tmv = 0;
	ssize_t len;

	mutex_lock(&b->lock);
	regmap_read(b->map, MT6357_FGADC_CON0, &con0);
	regmap_read(b->map, MT6357_CHR_TOP_CON0, &top);
	regmap_read(b->map, MT6357_BATON_ANA_CON0, &baton);
	adc_mv(b->chan_v, &vmv);
	if (b->have_temp)
		adc_mv(b->chan_t, &tmv);
	bracket_tables(b, b->temp_dc, &lo, &hi);
	len = sysfs_emit(buf,
		"updates=%lu fg_on=%u fgadc_con0=0x%04x chr_top_con0=0x%04x baton_ana_con0=0x%04x bat_mv=%d ntc_mv=%d pull_up_r=%u pull_up_mv=%u r_fg=%u meter_res=%u tables=%u table_lo=%d table_hi=%d temp_dc=%d soc_x100=%d ocv_soc_x100=%d fg_io_error=%u car_raw=0x%08x car_nah=%lld delta_nah=%lld charge_nah=%lld seeded=%u rested=%u rest_seconds=%lld reseeds=%u discontinuities=%u\n",
		b->updates, !!(con0 & FG_ON), con0, top, baton, vmv, tmv,
		b->pull_up_r, b->pull_up_mv, b->r_fg, b->meter_res, b->ntables,
		lo->temp, hi->temp, b->temp_dc, b->soc_filtered_x100,
		b->ocv_soc_x100, b->fg_io_error, b->car_raw, b->car_nah,
		b->car_delta_nah, b->charge_nah, b->seeded, b->rested,
		b->rest_start_sec ? (s64)ktime_get_boottime_seconds() - b->rest_start_sec : 0,
		b->reseeds, b->discontinuities);
	mutex_unlock(&b->lock);
	return len;
}
static DEVICE_ATTR_RO(debug);
static struct attribute *r1_battery_attrs[] = { &dev_attr_debug.attr, NULL };
ATTRIBUTE_GROUPS(r1_battery);

static const struct power_supply_desc desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = props,
	.num_properties = ARRAY_SIZE(props),
	.get_property = get_property,
};

static int parse_tables(struct r1_battery *b, struct device_node *np)
{
	char name[40];
	unsigned int i, j, n, col;
	int len, temp;

	for (i = 0; i < TABLES; i++) {
		struct profile *t = &b->tables[b->ntables];

		snprintf(name, sizeof(name), "TEMPERATURE_T%u", i);
		if (of_property_read_s32(np, name, &temp))
			break;
		snprintf(name, sizeof(name), "battery0_profile_t%u_num", i);
		if (of_property_read_u32(np, name, &n) || !n || n > 100)
			break;
		snprintf(name, sizeof(name), "battery0_profile_t%u_col", i);
		if (of_property_read_u32(np, name, &col))
			col = 3;
		if (col < 3 || col > 4)
			return -EINVAL;
		snprintf(name, sizeof(name), "battery0_profile_t%u", i);
		len = of_property_count_u32_elems(np, name);
		if (len < (int)(n * col))
			return -EINVAL;
		t->rows = devm_kcalloc(b->dev, n, sizeof(*t->rows), GFP_KERNEL);
		if (!t->rows)
			return -ENOMEM;
		for (j = 0; j < n; j++) {
			of_property_read_u32_index(np, name, j * col, &t->rows[j].mah);
			of_property_read_u32_index(np, name, j * col + 1, &t->rows[j].volt);
			of_property_read_u32_index(np, name, j * col + 2, &t->rows[j].res);
		}
		/* Reject malformed profiles before any lookup can divide by zero. */
		if (temp < -100 || temp > 100 || t->rows[0].mah != 0 ||
		    !t->rows[n - 1].mah)
			return -EINVAL;
		for (j = 1; j < n; j++)
			if (t->rows[j].volt > t->rows[j - 1].volt ||
			    t->rows[j].mah < t->rows[j - 1].mah)
				return -EINVAL;
		t->temp = temp;
		t->n = n;
		b->ntables++;
	}
	return b->ntables ? 0 : -ENODATA;
}

/* The stock io-channels indices follow the vendor numbering, which differs
 * from the mainline mediatek,mt6357-auxadc binding; select channels by the
 * mainline channel number on the same IIO device instead.
 */
static struct iio_channel *pick_channel(struct device *dev, struct iio_channel *ref, int number)
{
	struct iio_dev *indio_dev = ref->indio_dev;
	struct iio_channel *chan;
	int i;

	for (i = 0; i < indio_dev->num_channels; i++) {
		const struct iio_chan_spec *spec = &indio_dev->channels[i];

		if (spec->channel != number)
			continue;
		chan = devm_kzalloc(dev, sizeof(*chan), GFP_KERNEL);
		if (!chan)
			return ERR_PTR(-ENOMEM);
		chan->indio_dev = indio_dev;
		chan->channel = spec;
		return chan;
	}
	return ERR_PTR(-ENOENT);
}

static struct device *find_pmic(struct device_node **np_out)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "mediatek,mt6357");
	struct platform_device *pdev;

	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	if (np_out)
		*np_out = np;
	else
		of_node_put(np);
	return pdev ? &pdev->dev : NULL;
}

static int r1_battery_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct r1_battery *b;
	struct device *pmic;
	struct mt6397_chip *chip;
	struct iio_channel *ref;
	struct power_supply_config cfg = {};
	unsigned int con0;
	int ret;

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	mutex_init(&b->lock);
	pmic = find_pmic(NULL);
	if (!pmic)
		return -ENODEV;
	chip = dev_get_drvdata(pmic);
	put_device(pmic);
	if (!chip || !chip->regmap)
		return -ENODEV;
	b->map = chip->regmap;

	ref = devm_iio_channel_get(dev, "pmic_battery_temp");
	if (IS_ERR(ref))
		return dev_err_probe(dev, PTR_ERR(ref), "PMIC AUXADC channels\n");
	b->chan_v = pick_channel(dev, ref, AUXADC_CHAN_BATADC);
	if (IS_ERR(b->chan_v))
		return dev_err_probe(dev, PTR_ERR(b->chan_v), "BATADC channel\n");
	b->chan_t = pick_channel(dev, ref, AUXADC_CHAN_BAT_TEMP);
	if (IS_ERR(b->chan_t))
		dev_warn(dev, "no battery temperature channel: %ld\n", PTR_ERR(b->chan_t));
	else
		b->have_temp = true;

	if (of_property_read_u32(np, "R_FG_VALUE", &b->r_fg))
		b->r_fg = 10;
	b->r_fg *= 10;
	if (!b->r_fg || b->r_fg > 10000)
		return -EINVAL;
	if (of_property_read_u32(np, "CAR_TUNE_VALUE", &b->car_tune))
		b->car_tune = 100;
	if (!b->car_tune || b->car_tune > 1000)
		return -EINVAL;
	b->car_tune *= 10;
	if (of_property_read_u32(np, "FG_METER_RESISTANCE", &b->meter_res))
		b->meter_res = 75;
	if (of_property_read_u32(np, "RBAT_PULL_UP_R", &b->pull_up_r))
		b->pull_up_r = 16900;
	if (of_property_read_u32(np, "RBAT_PULL_UP_VOLT", &b->pull_up_mv))
		b->pull_up_mv = 1800;
	ret = parse_tables(b, np);
	if (ret)
		return dev_err_probe(dev, ret, "battery profile tables\n");

	ret = regmap_read(b->map, MT6357_FGADC_CON0, &con0);
	if (ret)
		return ret;
	if (!(con0 & FG_ON)) {
		dev_info(dev, "enabling FGADC (FGADC_CON0=0x%04x)\n", con0);
		ret = regmap_update_bits(b->map, MT6357_FGADC_CON0, FG_ON, FG_ON);
		if (ret)
			return ret;
	}

	b->temp_dc = 250;
	b->soc = 0;
	update(b);
	cfg.drv_data = b;
	cfg.fwnode = of_fwnode_handle(np);
	cfg.attr_grp = r1_battery_groups;
	b->psy = devm_power_supply_register(dev, &desc, &cfg);
	if (IS_ERR(b->psy))
		return dev_err_probe(dev, PTR_ERR(b->psy), "power supply\n");
	platform_set_drvdata(pdev, b);

	INIT_DELAYED_WORK(&b->poll, poll_work);
	schedule_delayed_work(&b->poll, msecs_to_jiffies(POLL_MS));
	dev_info(dev, "%d uV %d uA %d.%d C soc=%d%% status=%d online=%u present=%u tables=%u\n",
		 b->volt_uv, b->cur_ua, b->temp_dc / 10, abs(b->temp_dc % 10), b->soc,
		 b->status, b->online, b->present, b->ntables);
	return 0;
}

static void r1_battery_remove(struct platform_device *pdev)
{
	struct r1_battery *b = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&b->poll);
}

static const struct of_device_id r1_battery_of_match[] = {
	{ .compatible = "mediatek,mt6357-gauge" },
	{}
};
MODULE_DEVICE_TABLE(of, r1_battery_of_match);

static struct platform_driver r1_battery_driver = {
	.probe = r1_battery_probe,
	.remove = r1_battery_remove,
	.driver = {
		.name = "r1_battery",
		.of_match_table = r1_battery_of_match,
	},
};

static struct platform_device *gauge_pdev;

static int __init r1_battery_init(void)
{
	struct device_node *pmic_np = NULL, *np;
	struct platform_device *existing;
	struct device *pmic;
	int ret;

	pmic = find_pmic(&pmic_np);
	if (!pmic) {
		of_node_put(pmic_np);
		return -ENODEV;
	}
	np = of_get_compatible_child(pmic_np, "mediatek,mt6357-gauge");
	of_node_put(pmic_np);
	if (!np) {
		put_device(pmic);
		return -ENODEV;
	}
	ret = platform_driver_register(&r1_battery_driver);
	if (ret)
		goto out;
	existing = of_find_device_by_node(np);
	if (existing) {
		put_device(&existing->dev);
	} else {
		gauge_pdev = of_platform_device_create(np, "r1-battery", pmic);
		if (!gauge_pdev) {
			platform_driver_unregister(&r1_battery_driver);
			ret = -ENODEV;
		}
	}
out:
	of_node_put(np);
	put_device(pmic);
	return ret;
}

static void __exit r1_battery_exit(void)
{
	if (gauge_pdev)
		of_platform_device_destroy(&gauge_pdev->dev, NULL);
	platform_driver_unregister(&r1_battery_driver);
}

module_init(r1_battery_init);
module_exit(r1_battery_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rabbit R1 MT6357 battery power supply");
