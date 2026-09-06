/*
 * Copyright (c) 2026 Radu Pirea
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT xlnx_zynq_reset

#include <zephyr/device.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/syscon.h>
#include <zephyr/dt-bindings/reset/xlnx-zynq-reset.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(reset_xlnx_zynq);

/*
 * A reset identifier holds the number of the control register it lives in and
 * the bit within that register.
 */
#define RESET_BANK(id) ((id) / 32U)
#define RESET_BIT(id)  BIT((id) % 32U)

struct reset_xlnx_zynq_config {
	const struct device *syscon;
	uint32_t base;
};

/**
 * @brief Read the reset control register holding a reset
 *
 * @param dev Reset controller device
 * @param id  Identifier of the reset
 * @param val Where to store the contents of the register
 * @retval 0 on success
 * @retval -EINVAL The identifier names a register the controller does not cover
 * @retval -errno as reported by the system level control registers
 */
static int reset_xlnx_zynq_read(const struct device *dev, uint32_t id, uint32_t *val)
{
	const struct reset_xlnx_zynq_config *config = dev->config;

	if (RESET_BANK(id) >= XLNX_ZYNQ_RESET_BANKS) {
		LOG_ERR("%s: reset %u is out of range", dev->name, id);
		return -EINVAL;
	}

	return syscon_read_reg(config->syscon, config->base + (RESET_BANK(id) * 4U), val);
}

/**
 * @brief Drive the bit of a reset
 *
 * The registers hold unrelated resets side by side, so the bit is changed in
 * place rather than the register written outright.
 *
 * @param dev    Reset controller device
 * @param id     Identifier of the reset
 * @param assert Whether to hold the peripheral in reset
 * @retval 0 on success
 * @retval -EINVAL The identifier names a register the controller does not cover
 * @retval -errno as reported by the system level control registers
 */
static int reset_xlnx_zynq_set(const struct device *dev, uint32_t id, bool assert)
{
	const struct reset_xlnx_zynq_config *config = dev->config;
	uint32_t val;
	int err;

	err = reset_xlnx_zynq_read(dev, id, &val);
	if (err != 0) {
		return err;
	}

	if (assert) {
		val |= RESET_BIT(id);
	} else {
		val &= ~RESET_BIT(id);
	}

	return syscon_write_reg(config->syscon, config->base + (RESET_BANK(id) * 4U), val);
}

static int reset_xlnx_zynq_status(const struct device *dev, uint32_t id, uint8_t *status)
{
	uint32_t val;
	int err;

	err = reset_xlnx_zynq_read(dev, id, &val);
	if (err != 0) {
		return err;
	}

	*status = ((val & RESET_BIT(id)) != 0U) ? 1U : 0U;

	return 0;
}

static int reset_xlnx_zynq_line_assert(const struct device *dev, uint32_t id)
{
	return reset_xlnx_zynq_set(dev, id, true);
}

static int reset_xlnx_zynq_line_deassert(const struct device *dev, uint32_t id)
{
	return reset_xlnx_zynq_set(dev, id, false);
}

static int reset_xlnx_zynq_line_toggle(const struct device *dev, uint32_t id)
{
	int err;

	err = reset_xlnx_zynq_line_assert(dev, id);
	if (err != 0) {
		return err;
	}

	return reset_xlnx_zynq_line_deassert(dev, id);
}

static int reset_xlnx_zynq_init(const struct device *dev)
{
	const struct reset_xlnx_zynq_config *config = dev->config;

	if (!device_is_ready(config->syscon)) {
		LOG_ERR("%s: system level control registers not ready", dev->name);
		return -ENODEV;
	}

	/*
	 * The reset registers are write protected until the system level
	 * control registers are unlocked, which the SoC does on reset.
	 */

	return 0;
}

static DEVICE_API(reset, reset_xlnx_zynq_driver_api) = {
	.status = reset_xlnx_zynq_status,
	.line_assert = reset_xlnx_zynq_line_assert,
	.line_deassert = reset_xlnx_zynq_line_deassert,
	.line_toggle = reset_xlnx_zynq_line_toggle,
};

#define RESET_XLNX_ZYNQ_INIT(n)                                                                    \
	static const struct reset_xlnx_zynq_config reset_xlnx_zynq_config_##n = {                  \
		.syscon = DEVICE_DT_GET(DT_INST_PHANDLE(n, syscon)),                               \
		.base = DT_INST_REG_ADDR(n),                                                       \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, reset_xlnx_zynq_init, NULL, NULL, &reset_xlnx_zynq_config_##n,    \
			      PRE_KERNEL_1, CONFIG_RESET_XLNX_ZYNQ_INIT_PRIORITY,                  \
			      &reset_xlnx_zynq_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RESET_XLNX_ZYNQ_INIT)
