// SPDX-License-Identifier: GPL-2.0
/* R1 bring-up: native genpd/clock ownership, fixed shared rails, no GED/DVFS.
 * Board power stays on for the device lifetime. Firmware idle/power logic
 * remains in RGX; optional platform automatic power management is disabled.
 */
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include "mtk_mfgsys.h"

static bool r1_enable;
module_param(r1_enable, bool, 0400);
MODULE_PARM_DESC(r1_enable, "Explicitly enable the experimental R1 GPU probe");
static struct device *r1_dev;
static struct clk *r1_core;
static bool r1_power_owned;

/* No GED governor or global telemetry is installed by this platform. */
PVRSRV_ERROR MTKMFGSystemInit(void) { return PVRSRV_OK; }
void MTKMFGSystemDeInit(void) {}

int MTKRGXDeviceInit(PVRSRV_DEVICE_CONFIG *config)
{
	struct device *dev = config->pvOSDevice;
	struct regulator *core, *sram;
	RGX_DATA *data = config->hDevData;
	unsigned long rate;
	int vcore, vsram, ret;

	if (!r1_enable || r1_dev) {
		dev_err(dev, "r1-rgx: probe requires r1_enable=1 and one GPU\n");
		return PVRSRV_ERROR_INIT_FAILURE;
	}
	if (!dev->pm_domain) {
		dev_err(dev, "r1-rgx: native GPU power domain missing\n");
		return PVRSRV_ERROR_INIT_FAILURE;
	}
	r1_core = devm_clk_get(dev, "core");
	if (IS_ERR(r1_core))
		return PVRSRV_ERROR_INIT_FAILURE;
	core = devm_regulator_get(dev, "vcore");
	sram = devm_regulator_get(dev, "vsram");
	if (IS_ERR(core) || IS_ERR(sram))
		return PVRSRV_ERROR_INIT_FAILURE;
	vcore = regulator_get_voltage(core);
	vsram = regulator_get_voltage(sram);
	rate = clk_get_rate(r1_core);
	/* This experimental DT pins the already measured rails. Refuse a
	 * different operating point instead of changing a shared CPU rail.
	 */
	if (vcore != 800000 || vsram != 900000 ||
	    regulator_is_enabled(core) != 1 || regulator_is_enabled(sram) != 1 ||
	    rate < 26000000 || rate > 500000000) {
		dev_err(dev, "r1-rgx: unsupported rails/clock %d/%d uV %lu Hz\n",
			vcore, vsram, rate);
		return PVRSRV_ERROR_INIT_FAILURE;
	}
	ret = clk_prepare_enable(r1_core);
	if (ret)
		return PVRSRV_ERROR_INIT_FAILURE;
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		pm_runtime_disable(dev);
		clk_disable_unprepare(r1_core);
		return PVRSRV_ERROR_INIT_FAILURE;
	}
	r1_dev = dev;
	r1_power_owned = true;
	data->psRGXTimingInfo->ui32CoreClockSpeed = rate;
	dev_info(dev, "r1-rgx: native power active, %lu Hz, rails %d/%d uV\n",
		rate, vcore, vsram);
	return PVRSRV_OK;
}

int MTKRGXDeviceDeInit(PVRSRV_DEVICE_CONFIG *config)
{
	int ret;

	if (!r1_power_owned)
		return 0;
	ret = pm_runtime_put_sync_suspend(r1_dev);
	if (ret < 0) {
		dev_err(r1_dev, "r1-rgx: power release failed: %d\n", ret);
		return ret;
	}
	pm_runtime_disable(r1_dev);
	clk_disable_unprepare(r1_core);
	r1_power_owned = false;
	r1_dev = NULL;
	return 0;
}
MODULE_IMPORT_NS("DMA_BUF");

/*
 * Copyright (C) 2019 MediaTek Inc.
 */
