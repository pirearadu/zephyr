/*
 * Author: Radu Nicolae Pirea <pirea.radu@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT raspberrypi_rp2_pwm

#include <errno.h>
#include <drivers/pwm.h>
#include <hardware/gpio.h>
#include <logging/log.h>
LOG_MODULE_REGISTER(pwm_rpi, CONFIG_PWM_LOG_LEVEL);

#define RPI_PWM_MAX_PINS	29
#define RPI_PWM_MAX_CHANS	8
#define RPI_PWM_MAX_CYCLES	GENMASK(15, 0)
struct pwm_rpi_config {
	mm_reg_t regs_base;
	uint32_t prescaler;
};

struct pwm_rpi_data {
	uint8_t pins[RPI_PWM_MAX_CHANS];
	uint8_t chans;
};

#define CHAN_OFF	0x14
#define CSR_OFF		0x00
#define DIV_OFF		0x04
#define CTR_OFF		0x08
#define CC_OFF		0x0C
#define TOP_OFF		0x10

#define CSR_CHAN_EN	BIT(0)
#define CSR_CHAN_INV_A	BIT(2)
#define CSR_CHAN_INV_B	BIT(3)

#define REG_ADDR(base, chan_id, reg) (base + CHAN_OFF * chan_id + reg##_OFF)

#define DEV_CFG(dev) \
	((const struct pwm_rpi_config * const)(dev)->config)

#define PWM_CHAN_ID(pin) ((pwm >> 1) & 0x07)

#define RPI_PWM_CLK_FREQ	125000000ULL

static int pwm_rpi_get_cycles_per_sec(const struct device *dev, uint32_t pwm,
				      uint64_t *cycles)
{
	uint32_t prescaler = DEV_CFG(dev)->prescaler;

	*cycles = RPI_PWM_CLK_FREQ / (prescaler + 1);
	return 0;
}

static int pwm_rpi_pin_set(const struct device *dev, uint32_t pwm,
			   uint32_t period, uint32_t pulse, pwm_flags_t flags)
{
	mm_reg_t regs_base = DEV_CFG(dev)->regs_base;
	struct pwm_rpi_data *data = dev->data;
	uint32_t chan_id = PWM_CHAN_ID(pwm);
	uint32_t csr;

	if (pwm > RPI_PWM_MAX_PINS) {
		LOG_ERR("invalid PWM pin.\n");
		return -EINVAL;
	}

	if (period > RPI_PWM_MAX_CYCLES) {
		LOG_ERR("period > %lu\n", RPI_PWM_MAX_CYCLES);
		return -EINVAL;
	}

	if (period < pulse) {
		LOG_ERR("period < pulses --- %u < %u\n", period, pulse);
		return -EINVAL;
	}

	if (data->chans & BIT(chan_id)) {
		if (data->pins[chan_id] != pwm)
			return -EBUSY;
	} else {
		data->chans |= BIT(chan_id);
		data->pins[chan_id] = pwm;
	}

	sys_write32(period, REG_ADDR(regs_base, chan_id, TOP));

	if (pwm % 2) {
		pulse <<= 16;
		sys_write32(pulse, REG_ADDR(regs_base, chan_id, CC));
	} else {
		sys_write32(pulse, REG_ADDR(regs_base, chan_id, CC));
	}

	csr = sys_read32(REG_ADDR(regs_base, chan_id, CSR));
	csr |= CSR_CHAN_EN;

	if (flags & PWM_POLARITY_INVERTED) {
		if (pwm % 2)
			csr |= CSR_CHAN_INV_B;
		else
			csr |= CSR_CHAN_INV_A;
	} else {
		if (pwm % 2)
			csr &= ~CSR_CHAN_INV_B;
		else
			csr &= ~CSR_CHAN_INV_A;
	}

	sys_write32(csr, REG_ADDR(regs_base, chan_id, CSR));
	gpio_set_function(pwm, GPIO_FUNC_PWM);

	return 0;
}

static int pwm_rpi_init(const struct device *dev)
{
	mm_reg_t regs_base = DEV_CFG(dev)->regs_base;
	uint32_t prescaler = DEV_CFG(dev)->prescaler;
	struct pwm_rpi_data *data = dev->data;
	int chan_id;

	data->chans = 0;
	for (chan_id = 0; chan_id < 8; chan_id++) {
		data->pins[chan_id] = 0;
		sys_write32(prescaler << 4, REG_ADDR(regs_base, chan_id, DIV));
	}

	return 0;
}

static const struct pwm_driver_api pwm_rpi_driver_api = {
	.pin_set = pwm_rpi_pin_set,
	.get_cycles_per_sec = pwm_rpi_get_cycles_per_sec,
};

#define PWM_RPI_INST_INIT(inst)						\
	static const struct pwm_rpi_config pwm_rpi_config_##inst = {	\
		.regs_base = (mm_reg_t)DT_INST_REG_ADDR(inst),		\
		.prescaler = (uint8_t)DT_INST_PROP(inst, prescaler),	\
	};								\
									\
	static struct pwm_rpi_data pwm_rpi_data_##inst;			\
									\
	DEVICE_DT_INST_DEFINE(inst,					\
			      &pwm_rpi_init, NULL,			\
			      &pwm_rpi_data_##inst,			\
			      &pwm_rpi_config_##inst,			\
			      POST_KERNEL,				\
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	\
			      &pwm_rpi_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PWM_RPI_INST_INIT)
