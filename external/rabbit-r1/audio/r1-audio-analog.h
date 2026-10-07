// SPDX-License-Identifier: GPL-2.0
/* R1 AW87390FCR + MT6357 lineout PCM analog sequence. Bitbang i2c0 GPIOs 83/82.
 * Adapted from the proven tone path: PCM input, SGEN disabled, -10 dB lineout. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/jiffies.h>
#include <linux/hrtimer.h>
#include <linux/wait.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6357/registers.h>
#include <linux/regmap.h>

#define PIN_SCL 83
#define PIN_SDA 82
#define GPIO_PA 0x10005000
#define MODE_BASE 0x300
#define I2C_ADDR 0x58
#define REG_ID 0x00
#define CHIP_ID 0x76
#define REG_SYSCTRL 0x01
#define DELAY_REG 0xfe
#ifndef MAX_MS
#define MAX_MS 500
#endif
#ifndef PLAY_BUDGET
#define PLAY_BUDGET 2
#endif
#ifndef GUARD_MS
#define GUARD_MS 600
#endif
#if MAX_MS < 1 || MAX_MS > 500 || PLAY_BUDGET < 1 || PLAY_BUDGET > 4 || GUARD_MS < MAX_MS
#error "Unsupported speaker test limits"
#endif
#define COOLDOWN_MS 1000
#define ARM_MS 30000
#define DL_GAIN_0DB_REG 0x0408
#define DL_GAIN_N40_REG 0x0f9f

struct amp {
	struct device *dev;
	struct gpio_desc *scl, *sda;
	void __iomem *regs;
	struct regmap *pmic;
	struct mutex lock;
	struct hrtimer guard;
	wait_queue_head_t wait;
	unsigned long arm_until, next_play;
	bool armed, playing, aborted, analog_ok;
	int id, nack, io_error, last_result, last_ms;
	unsigned remaining, commands;
	unsigned sysctrl, sgen0, ana0, ana4, top;
	u8 off_sysctrl;
};

/* Stock aw87xxx_acf.bin Music profile (AW_BIN_TYPE_HDR_REG), 0xfe = delay ms. */
static const u8 music_acf[] = {
	0x02, 0x07, 0x02, 0x00, 0x02, 0x0c, 0x66, 0x38, 0x01, 0x03, 0xfe, 0x02,
	0x67, 0x23, 0x02, 0x07, 0x02, 0x00, 0x02, 0x1c, 0x03, 0x08, 0x04, 0x05,
	0x05, 0x0c, 0x06, 0x07, 0x07, 0x4e, 0x08, 0x06, 0x09, 0x08, 0x0a, 0x4b,
	0x61, 0xb3, 0x62, 0x24, 0x63, 0x09, 0x64, 0x24, 0x65, 0x15, 0x79, 0x7a,
	0x7a, 0x6c, 0x78, 0x80, 0x66, 0x38, 0x76, 0x00, 0x78, 0x00, 0x68, 0x1b,
	0x69, 0x5b, 0x70, 0x1c, 0x71, 0x00, 0x72, 0xfe, 0x73, 0x4f, 0x74, 0x24,
	0x75, 0x02, 0x01, 0x07
};

static void gpio_mode0(struct amp *a, unsigned pin)
{
	unsigned off = MODE_BASE + (pin / 8) * 0x10;
	unsigned sh = (pin % 8) * 4;
	writel(0xfu << sh, a->regs + off + 8);
}

static void od(struct gpio_desc *g, int hi)
{
	if (hi)
		gpiod_direction_input(g);
	else {
		gpiod_direction_output(g, 0);
		gpiod_set_value(g, 0);
	}
}

static void bb_delay(void)
{
	udelay(5);
}

static void bb_start(struct amp *a)
{
	od(a->sda, 1);
	od(a->scl, 1);
	bb_delay();
	od(a->sda, 0);
	bb_delay();
	od(a->scl, 0);
	bb_delay();
}

static void bb_stop(struct amp *a)
{
	od(a->sda, 0);
	bb_delay();
	od(a->scl, 1);
	bb_delay();
	od(a->sda, 1);
	bb_delay();
}

static int bb_wr_byte(struct amp *a, u8 v)
{
	int b, ack;

	for (b = 7; b >= 0; b--) {
		od(a->sda, (v >> b) & 1);
		bb_delay();
		od(a->scl, 1);
		bb_delay();
		od(a->scl, 0);
		bb_delay();
	}
	od(a->sda, 1);
	bb_delay();
	od(a->scl, 1);
	bb_delay();
	ack = gpiod_get_value(a->sda);
	od(a->scl, 0);
	bb_delay();
	return ack ? -1 : 0;
}

static u8 bb_rd_byte(struct amp *a, int nack)
{
	int b;
	u8 v = 0;

	od(a->sda, 1);
	for (b = 7; b >= 0; b--) {
		bb_delay();
		od(a->scl, 1);
		bb_delay();
		if (gpiod_get_value(a->sda))
			v |= (u8)BIT(b);
		od(a->scl, 0);
	}
	od(a->sda, nack);
	bb_delay();
	od(a->scl, 1);
	bb_delay();
	od(a->scl, 0);
	od(a->sda, 1);
	bb_delay();
	return v;
}

static int i2c_write(struct amp *a, u8 reg, u8 val)
{
	bb_start(a);
	if (bb_wr_byte(a, I2C_ADDR << 1) || bb_wr_byte(a, reg) ||
	    bb_wr_byte(a, val)) {
		bb_stop(a);
		a->io_error = 1;
		return -EIO;
	}
	bb_stop(a);
	return 0;
}

static int i2c_read(struct amp *a, u8 reg, u8 *val)
{
	bb_start(a);
	if (bb_wr_byte(a, I2C_ADDR << 1)) {
		bb_stop(a);
		return -ENXIO;
	}
	if (bb_wr_byte(a, reg)) {
		bb_stop(a);
		return -EIO;
	}
	bb_start(a);
	if (bb_wr_byte(a, (I2C_ADDR << 1) | 1)) {
		bb_stop(a);
		return -ENXIO;
	}
	*val = bb_rd_byte(a, 1);
	bb_stop(a);
	return 0;
}

static int amp_apply(struct amp *a, const u8 *seq, size_t len)
{
	size_t i;

	for (i = 0; i + 1 < len; i += 2) {
		if (seq[i] == DELAY_REG)
			usleep_range(seq[i + 1] * 1000, seq[i + 1] * 1000 + 200);
		else if (i2c_write(a, seq[i], seq[i + 1]))
			return -EIO;
	}
	return 0;
}

static int pmic_upd(struct amp *a, unsigned int reg, unsigned int val,
		    unsigned int mask)
{
	int ret;

	if (!a->pmic)
		return -ENODEV;
	ret = regmap_update_bits(a->pmic, reg, mask, val);
	if (ret)
		a->io_error = 1;
	return ret;
}

static int pmic_wr(struct amp *a, unsigned int reg, unsigned int val)
{
	int ret;

	if (!a->pmic)
		return -ENODEV;
	ret = regmap_write(a->pmic, reg, val);
	if (ret)
		a->io_error = 1;
	return ret;
}

static void codec_off(struct amp *a)
{
	if (!a->pmic)
		return;
	pmic_upd(a, MT6357_AUDDEC_ANA_CON4, 0, 0x3 << 2);
	pmic_upd(a, MT6357_AUDDEC_ANA_CON2, 0x400, 0x400);
	pmic_upd(a, MT6357_AUDDEC_ANA_CON0, 0, 0x000f);
	pmic_upd(a, MT6357_AUDDEC_ANA_CON11, 0, 0x1);
	pmic_wr(a, MT6357_ZCD_CON1, DL_GAIN_N40_REG);
	pmic_upd(a, MT6357_AUDDEC_ANA_CON4, 0, 0x3);
	pmic_upd(a, MT6357_AUDDEC_ANA_CON6, 0x200, 0xff00);
	pmic_wr(a, MT6357_AUDDEC_ANA_CON7, 0xa8);
	pmic_upd(a, MT6357_AUDDEC_ANA_CON10, 0x100, 0x100);
	pmic_wr(a, MT6357_ZCD_CON0, 0);
	pmic_upd(a, MT6357_AUDDEC_ANA_CON13, 0, 0x1);
	pmic_upd(a, MT6357_AUDDEC_ANA_CON12, 0, 0x1055);
	pmic_upd(a, MT6357_AUDNCP_CLKDIV_CON3, 1, 1);
	pmic_wr(a, MT6357_AFE_SGEN_CFG0, 0);
	pmic_wr(a, MT6357_AFE_TOP_CON0, 0);
	pmic_wr(a, MT6357_AFE_DL_SRC2_CON0_L, 0);
	pmic_wr(a, MT6357_AFUNC_AUD_CON2, 0);
	pmic_wr(a, MT6357_AFUNC_AUD_CON0, 0xcba0);
	pmic_upd(a, MT6357_AFE_UL_DL_CON0, 0, 1);
	pmic_upd(a, MT6357_AUDIO_TOP_CON0, 0x00df, 0x00df);
	pmic_upd(a, MT6357_AFE_AUD_PAD_TOP, 0, 0xff);
	pmic_upd(a, MT6357_AUD_TOP_CKPDN_CON0, 0x66, 0x66);
	pmic_upd(a, MT6357_AUDENC_ANA_CON6, 0, 1);
	pmic_upd(a, MT6357_AUDDEC_ANA_CON11, 0x10, 0x10);
	pmic_upd(a, MT6357_DCXO_CW14, 0, BIT(13));
	pmic_wr(a, MT6357_GPIO_MODE2, 0);
	pmic_upd(a, MT6357_GPIO_DIR0, 0, 0x0f00);
}

static int codec_on(struct amp *a)
{
	int ret;
	if (!a->pmic)
		return -ENODEV;
	/* TurnOnDacPower(SPEAKERL) + Speaker_Amp_Change(true) + DL SGEN */
	if ((ret = pmic_wr(a, MT6357_GPIO_MODE2, 0x0249)))
		return ret;
	if ((ret = pmic_upd(a, MT6357_SMT_CON1, 0x0ff0, 0x0ff0)))
		return ret;

	if ((ret = pmic_upd(a, MT6357_DCXO_CW14, BIT(13), BIT(13))))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON6, 0x0280)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON7, 0x0028)))
		return ret;
	if ((ret = pmic_upd(a, MT6357_AUDDEC_ANA_CON11, 0, BIT(4))))
		return ret;
	if ((ret = pmic_upd(a, MT6357_AUDDEC_ANA_CON2, 0x400, 0x400)))
		return ret;
	if ((ret = pmic_upd(a, MT6357_AUDENC_ANA_CON6, 1, 1)))
		return ret;
	if ((ret = pmic_upd(a, MT6357_AUD_TOP_CKPDN_CON0, 0, 0x66)))
		return ret;
	usleep_range(250, 350);
	if ((ret = pmic_upd(a, MT6357_AUDIO_TOP_CON0, 0, 0x00c4)))
		return ret; /* normal playback clocks; no PMIC test generator */
	usleep_range(250, 350);
	if ((ret = pmic_wr(a, MT6357_AFUNC_AUD_CON2, 0x0006)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AFUNC_AUD_CON0, 0xCBA1)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AFUNC_AUD_CON2, 0x0003)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AFUNC_AUD_CON2, 0x000B)))
		return ret;
	if ((ret = pmic_upd(a, MT6357_AFE_UL_DL_CON0, 1, 1)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AFE_DL_SRC2_CON0_L, 1)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AFE_TOP_CON0, 0x0000)))
		return ret; /* select normal MTKAIF input */
	/* Keep the PMIC sine generator disabled for actual PCM playback. */
	if ((ret = pmic_wr(a, MT6357_AFE_SGEN_CFG0, 0x0000)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AFE_SGEN_CFG1, 0x0101)))
		return ret;
	if ((ret = pmic_upd(a, MT6357_AFE_AUD_PAD_TOP, 0x0031, 0x00ff)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON0, 0x3000)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON3, 0x0010)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON4, 0x0010)))
		return ret;
	if ((ret = pmic_upd(a, MT6357_AUDDEC_ANA_CON2, 0x200, 0x200)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_ZCD_CON1, DL_GAIN_N40_REG)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDNCP_CLKDIV_CON1, 0x0001)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDNCP_CLKDIV_CON2, 0x002c)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDNCP_CLKDIV_CON0, 0x0001)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDNCP_CLKDIV_CON4, 0x0002)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDNCP_CLKDIV_CON3, 0x0000)))
		return ret;
	usleep_range(250, 350);
	if ((ret = pmic_upd(a, MT6357_AUDDEC_ANA_CON12, 0x1055, 0x1055)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON13, 0x0001)))
		return ret;
	usleep_range(100, 150);
	if ((ret = pmic_upd(a, MT6357_AUDDEC_ANA_CON8, 0, 0x7)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_ZCD_CON0, 0x010b)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON10, 0x0055)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON9, 0x0092)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON4, 0x0110)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON4, 0x0112)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON4, 0x0113)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_ZCD_CON1, 0x0912)))
		return ret; /* L/R -10 dB, native codec TLV */
	if ((ret = pmic_upd(a, MT6357_AUDDEC_ANA_CON11, 1, 1)))
		return ret;
	/* Stock Speaker_Amp_Change lineout path only. */
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON0, 0x3009)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON6, 0x0201)))
		return ret;
	if ((ret = pmic_wr(a, MT6357_AUDDEC_ANA_CON4, 0x011b)))
		return ret;
	if ((ret = pmic_upd(a, MT6357_AUDDEC_ANA_CON2, 0, 0x400)))
		return ret;
	return a->io_error ? -EIO : 0;
}

/* Vendor reference attribution retained for MT6357/MT6765 sequences:
 * Copyright (c) 2019 MediaTek Inc.
 * Author: Michael Hsiao <michael.hsiao@mediatek.com>
 * Codec reference also credits Chipeng Chang.
 * See Documentation/rabbit-r1/external-sources.json for reference hashes.
 */
