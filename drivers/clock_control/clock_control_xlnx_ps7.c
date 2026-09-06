/*
 * Copyright (c) 2026 Radu Pirea
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT xlnx_ps7_clkc

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/syscon.h>
#include <zephyr/dt-bindings/clock/xlnx-ps7-clkc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(clock_control_xlnx_ps7, CONFIG_CLOCK_CONTROL_LOG_LEVEL);

/*
 * Register offsets relative to the node's own base, which the devicetree places
 * at offset 0x100 of the system level control registers (Zynq-7000 TRM UG585
 * v1.13, appendix B.28 slcr).
 */
#define ARM_PLL_CTRL   0x00
#define DDR_PLL_CTRL   0x04
#define IO_PLL_CTRL    0x08
#define ARM_CLK_CTRL   0x20
#define DCI_CLK_CTRL   0x28
#define APER_CLK_CTRL  0x2c
#define GEM0_CLK_CTRL  0x40
#define GEM1_CLK_CTRL  0x44
#define SMC_CLK_CTRL   0x48
#define LQSPI_CLK_CTRL 0x4c
#define SDIO_CLK_CTRL  0x50
#define UART_CLK_CTRL  0x54
#define SPI_CLK_CTRL   0x58
#define CAN_CLK_CTRL   0x5c
#define DBG_CLK_CTRL   0x64
#define PCAP_CLK_CTRL  0x68
#define FPGA0_CLK_CTRL 0x70
#define FPGA1_CLK_CTRL 0x80
#define FPGA2_CLK_CTRL 0x90
#define FPGA3_CLK_CTRL 0xa0
#define CLK_621_TRUE   0xc4

/* WDT_CLK_SEL lives outside this node's window, at 0x304 of the SLCR */
#define WDT_CLK_SEL        0x204
#define WDT_CLK_SEL_EXTERN BIT(0)

/* PLL control registers */
#define PLL_CTRL_FDIV_MASK  GENMASK(18, 12)
#define PLL_CTRL_FDIV_SHIFT 12

/* Clock control registers */
#define CLK_CTRL_DIVISOR1_MASK    GENMASK(25, 20)
#define CLK_CTRL_DIVISOR1_SHIFT   20
#define CLK_CTRL_DIVISOR0_MASK    GENMASK(13, 8)
#define CLK_CTRL_DIVISOR0_SHIFT   8
#define CLK_CTRL_SRCSEL_MASK      GENMASK(5, 4)
#define CLK_CTRL_SRCSEL_SHIFT     4
#define GEM_CLK_CTRL_SRCSEL_MASK  GENMASK(6, 4)
#define GEM_CLK_CTRL_SRCSEL_SHIFT 4

#define CLK_621_TRUE_621 BIT(0)

/*
 * Source of a clock, as encoded in its SRCSEL field. The CPU clock and the
 * peripheral clocks number their sources differently: the CPU clock falls back
 * to the ARM PLL, a peripheral to the IO PLL.
 */
#define SRCSEL_CPU_DDR_PLL 0x2U
#define SRCSEL_CPU_IO_PLL  0x3U
#define SRCSEL_PER_ARM_PLL 0x2U
#define SRCSEL_PER_DDR_PLL 0x3U
#define SRCSEL_GEM_EMIO    0x4U

struct clock_control_xlnx_ps7_config {
	const struct device *syscon;
	uint32_t base;
	uint32_t ps_clk_frequency;
};

/**
 * @brief Describe a clock the controller derives with one divider register
 *
 * @param reg     Offset of the clock control register
 * @param divisors Number of divider fields the register holds, 1 or 2
 * @param act_bit Bit of the clock control register gating the clock, or
 *                CLK_NO_GATE when the clock cannot be gated on its own
 */
#define CLK_NO_GATE 0xffU

struct clock_desc {
	uint8_t reg;
	uint8_t divisors;
	uint8_t act_bit;
	bool gem_srcsel;
};

static const struct clock_desc clock_descs[XLNX_PS7_CLK_NUM] = {
	[XLNX_PS7_CLK_LQSPI] = {LQSPI_CLK_CTRL, 1, 0, false},
	[XLNX_PS7_CLK_SMC] = {SMC_CLK_CTRL, 1, 0, false},
	[XLNX_PS7_CLK_PCAP] = {PCAP_CLK_CTRL, 1, 0, false},
	[XLNX_PS7_CLK_GEM0] = {GEM0_CLK_CTRL, 2, 0, true},
	[XLNX_PS7_CLK_GEM1] = {GEM1_CLK_CTRL, 2, 0, true},
	[XLNX_PS7_CLK_FCLK0] = {FPGA0_CLK_CTRL, 2, CLK_NO_GATE, false},
	[XLNX_PS7_CLK_FCLK1] = {FPGA1_CLK_CTRL, 2, CLK_NO_GATE, false},
	[XLNX_PS7_CLK_FCLK2] = {FPGA2_CLK_CTRL, 2, CLK_NO_GATE, false},
	[XLNX_PS7_CLK_FCLK3] = {FPGA3_CLK_CTRL, 2, CLK_NO_GATE, false},
	[XLNX_PS7_CLK_CAN0] = {CAN_CLK_CTRL, 1, 0, false},
	[XLNX_PS7_CLK_CAN1] = {CAN_CLK_CTRL, 1, 1, false},
	[XLNX_PS7_CLK_SDIO0] = {SDIO_CLK_CTRL, 1, 0, false},
	[XLNX_PS7_CLK_SDIO1] = {SDIO_CLK_CTRL, 1, 1, false},
	[XLNX_PS7_CLK_UART0] = {UART_CLK_CTRL, 1, 0, false},
	[XLNX_PS7_CLK_UART1] = {UART_CLK_CTRL, 1, 1, false},
	[XLNX_PS7_CLK_SPI0] = {SPI_CLK_CTRL, 1, 0, false},
	[XLNX_PS7_CLK_SPI1] = {SPI_CLK_CTRL, 1, 1, false},
	[XLNX_PS7_CLK_DCI] = {DCI_CLK_CTRL, 2, 0, false},
};

/* Bit of APER_CLK_CTRL gating each AMBA peripheral clock */
static const uint8_t aper_bits[XLNX_PS7_CLK_NUM] = {
	[XLNX_PS7_CLK_DMA] = 0,
	[XLNX_PS7_CLK_USB0_APER] = 2,   [XLNX_PS7_CLK_USB1_APER] = 3,
	[XLNX_PS7_CLK_GEM0_APER] = 6,   [XLNX_PS7_CLK_GEM1_APER] = 7,
	[XLNX_PS7_CLK_SDIO0_APER] = 10, [XLNX_PS7_CLK_SDIO1_APER] = 11,
	[XLNX_PS7_CLK_SPI0_APER] = 14,  [XLNX_PS7_CLK_SPI1_APER] = 15,
	[XLNX_PS7_CLK_CAN0_APER] = 16,  [XLNX_PS7_CLK_CAN1_APER] = 17,
	[XLNX_PS7_CLK_I2C0_APER] = 18,  [XLNX_PS7_CLK_I2C1_APER] = 19,
	[XLNX_PS7_CLK_UART0_APER] = 20, [XLNX_PS7_CLK_UART1_APER] = 21,
	[XLNX_PS7_CLK_GPIO_APER] = 22,  [XLNX_PS7_CLK_LQSPI_APER] = 23,
	[XLNX_PS7_CLK_SMC_APER] = 24,
};

/*
 * The clock the DMA controller drives its AXI master with is gated in the same
 * register as the AMBA peripheral clocks, but runs at CPU_2X rather than at
 * CPU_1X, so it shares the gating path and not the rate one.
 */
static bool clock_is_aper(uint32_t id)
{
	return id == XLNX_PS7_CLK_DMA ||
	       (id >= XLNX_PS7_CLK_USB0_APER && id <= XLNX_PS7_CLK_SMC_APER);
}

static int ps7_read(const struct device *dev, uint16_t offset, uint32_t *val)
{
	const struct clock_control_xlnx_ps7_config *config = dev->config;

	return syscon_read_reg(config->syscon, config->base + offset, val);
}

static int ps7_write(const struct device *dev, uint16_t offset, uint32_t val)
{
	const struct clock_control_xlnx_ps7_config *config = dev->config;

	return syscon_write_reg(config->syscon, config->base + offset, val);
}

/**
 * @brief Read the output frequency of one of the three PLLs
 *
 * A PLL multiplies the PS reference clock by the feedback divisor of its
 * control register.
 *
 * @param dev  Clock controller device
 * @param reg  Offset of the PLL's control register
 * @param rate Where to store the frequency in Hz
 * @retval 0 on success
 * @retval -errno as reported by the system level control registers
 */
static int ps7_pll_rate(const struct device *dev, uint16_t reg, uint32_t *rate)
{
	const struct clock_control_xlnx_ps7_config *config = dev->config;
	uint32_t val;
	int err;

	err = ps7_read(dev, reg, &val);
	if (err != 0) {
		return err;
	}

	*rate = config->ps_clk_frequency * ((val & PLL_CTRL_FDIV_MASK) >> PLL_CTRL_FDIV_SHIFT);

	return 0;
}

/**
 * @brief Read the frequency of the PLL a clock control register selects
 *
 * The CPU clock and the peripheral clocks encode their source differently, and
 * a GEM may take its reference from the programmable logic instead of a PLL.
 *
 * @param dev    Clock controller device
 * @param srcsel Value of the register's source select field
 * @param cpu    Whether the field follows the encoding of the CPU clock
 * @param rate   Where to store the frequency in Hz
 * @retval 0 on success
 * @retval -ENOTSUP The clock is sourced from the programmable logic, whose
 *                  frequency the controller cannot know
 * @retval -errno as reported by the system level control registers
 */
static int ps7_src_rate(const struct device *dev, uint32_t srcsel, bool cpu, uint32_t *rate)
{
	if (cpu) {
		switch (srcsel) {
		case SRCSEL_CPU_DDR_PLL:
			return ps7_pll_rate(dev, DDR_PLL_CTRL, rate);
		case SRCSEL_CPU_IO_PLL:
			return ps7_pll_rate(dev, IO_PLL_CTRL, rate);
		default:
			return ps7_pll_rate(dev, ARM_PLL_CTRL, rate);
		}
	}

	if ((srcsel & SRCSEL_GEM_EMIO) != 0U) {
		return -ENOTSUP;
	}

	switch (srcsel & GENMASK(1, 0)) {
	case SRCSEL_PER_ARM_PLL:
		return ps7_pll_rate(dev, ARM_PLL_CTRL, rate);
	case SRCSEL_PER_DDR_PLL:
		return ps7_pll_rate(dev, DDR_PLL_CTRL, rate);
	default:
		return ps7_pll_rate(dev, IO_PLL_CTRL, rate);
	}
}

/**
 * @brief Read the frequency of one of the four CPU clocks
 *
 * All four are derived from the same divided PLL output, in a ratio the
 * CLK_621_TRUE register selects.
 *
 * @param dev  Clock controller device
 * @param id   Identifier of the CPU clock
 * @param rate Where to store the frequency in Hz
 * @retval 0 on success
 * @retval -errno as reported by the system level control registers
 */
static int ps7_cpu_rate(const struct device *dev, uint32_t id, uint32_t *rate)
{
	uint32_t ctrl;
	uint32_t ratio;
	uint32_t div;
	uint32_t src;
	int err;

	err = ps7_read(dev, ARM_CLK_CTRL, &ctrl);
	if (err != 0) {
		return err;
	}

	err = ps7_src_rate(dev, (ctrl & CLK_CTRL_SRCSEL_MASK) >> CLK_CTRL_SRCSEL_SHIFT, true, &src);
	if (err != 0) {
		return err;
	}

	div = (ctrl & CLK_CTRL_DIVISOR0_MASK) >> CLK_CTRL_DIVISOR0_SHIFT;
	if (div == 0U) {
		return -EIO;
	}

	err = ps7_read(dev, CLK_621_TRUE, &ratio);
	if (err != 0) {
		return err;
	}

	*rate = src / div;

	switch (id) {
	case XLNX_PS7_CLK_CPU_6OR4X:
		break;
	case XLNX_PS7_CLK_CPU_3OR2X:
		*rate /= 2U;
		break;
	case XLNX_PS7_CLK_CPU_2X:
		*rate /= ((ratio & CLK_621_TRUE_621) != 0U) ? 3U : 2U;
		break;
	default:
		*rate /= ((ratio & CLK_621_TRUE_621) != 0U) ? 6U : 4U;
		break;
	}

	return 0;
}

static int clock_control_xlnx_ps7_get_rate(const struct device *dev, clock_control_subsys_t sys,
					   uint32_t *rate)
{
	uint32_t id = (uint32_t)(uintptr_t)sys;
	const struct clock_desc *desc;
	uint32_t ctrl;
	uint32_t src;
	uint32_t div;
	int err;

	if (id >= XLNX_PS7_CLK_NUM) {
		return -EINVAL;
	}

	switch (id) {
	case XLNX_PS7_CLK_ARMPLL:
		return ps7_pll_rate(dev, ARM_PLL_CTRL, rate);
	case XLNX_PS7_CLK_DDRPLL:
		return ps7_pll_rate(dev, DDR_PLL_CTRL, rate);
	case XLNX_PS7_CLK_IOPLL:
		return ps7_pll_rate(dev, IO_PLL_CTRL, rate);
	case XLNX_PS7_CLK_CPU_6OR4X:
	case XLNX_PS7_CLK_CPU_3OR2X:
	case XLNX_PS7_CLK_CPU_2X:
	case XLNX_PS7_CLK_CPU_1X:
		return ps7_cpu_rate(dev, id, rate);
	case XLNX_PS7_CLK_DMA:
		return ps7_cpu_rate(dev, XLNX_PS7_CLK_CPU_2X, rate);
	default:
		break;
	}

	/*
	 * Every AMBA peripheral clock and the watchdog, whose source register
	 * selects between an external clock and this one, run at CPU_1X.
	 */
	if (clock_is_aper(id)) {
		return ps7_cpu_rate(dev, XLNX_PS7_CLK_CPU_1X, rate);
	}

	if (id == XLNX_PS7_CLK_SWDT) {
		err = ps7_read(dev, WDT_CLK_SEL, &ctrl);
		if (err != 0) {
			return err;
		}
		if ((ctrl & WDT_CLK_SEL_EXTERN) != 0U) {
			return -ENOTSUP;
		}

		return ps7_cpu_rate(dev, XLNX_PS7_CLK_CPU_1X, rate);
	}

	desc = &clock_descs[id];
	if (desc->reg == 0U) {
		return -ENOTSUP;
	}

	err = ps7_read(dev, desc->reg, &ctrl);
	if (err != 0) {
		return err;
	}

	if (desc->gem_srcsel) {
		src = (ctrl & GEM_CLK_CTRL_SRCSEL_MASK) >> GEM_CLK_CTRL_SRCSEL_SHIFT;
	} else {
		src = (ctrl & CLK_CTRL_SRCSEL_MASK) >> CLK_CTRL_SRCSEL_SHIFT;
	}

	err = ps7_src_rate(dev, src, false, &src);
	if (err != 0) {
		return err;
	}

	div = (ctrl & CLK_CTRL_DIVISOR0_MASK) >> CLK_CTRL_DIVISOR0_SHIFT;
	if (desc->divisors == 2U) {
		div *= (ctrl & CLK_CTRL_DIVISOR1_MASK) >> CLK_CTRL_DIVISOR1_SHIFT;
	}

	/*
	 * A divider left at zero out of reset means the clock has never been
	 * given a frequency, which is not the same as one running at an unknown
	 * rate: nothing has configured it yet.
	 */
	if (div == 0U) {
		LOG_WRN("%s: clock %u has an unprogrammed divider", dev->name, id);
		return -ENODATA;
	}

	*rate = src / div;

	return 0;
}

/**
 * @brief Gate or ungate a clock
 *
 * @param dev Clock controller device
 * @param id  Identifier of the clock
 * @param on  Whether to let the clock run
 * @retval 0 on success
 * @retval -ENOTSUP The clock has no gate of its own
 * @retval -EINVAL The identifier does not name a clock
 * @retval -errno as reported by the system level control registers
 */
static int ps7_gate(const struct device *dev, uint32_t id, bool on)
{
	const struct clock_desc *desc;
	uint16_t reg;
	uint8_t bit;
	uint32_t val;
	int err;

	if (id >= XLNX_PS7_CLK_NUM) {
		return -EINVAL;
	}

	if (clock_is_aper(id)) {
		reg = APER_CLK_CTRL;
		bit = aper_bits[id];
	} else {
		desc = &clock_descs[id];
		if (desc->reg == 0U || desc->act_bit == CLK_NO_GATE) {
			return -ENOTSUP;
		}
		reg = desc->reg;
		bit = desc->act_bit;
	}

	err = ps7_read(dev, reg, &val);
	if (err != 0) {
		return err;
	}

	if (on) {
		val |= BIT(bit);
	} else {
		val &= ~BIT(bit);
	}

	return ps7_write(dev, reg, val);
}

static int clock_control_xlnx_ps7_on(const struct device *dev, clock_control_subsys_t sys)
{
	return ps7_gate(dev, (uint32_t)(uintptr_t)sys, true);
}

static int clock_control_xlnx_ps7_off(const struct device *dev, clock_control_subsys_t sys)
{
	return ps7_gate(dev, (uint32_t)(uintptr_t)sys, false);
}

static enum clock_control_status clock_control_xlnx_ps7_get_status(const struct device *dev,
								   clock_control_subsys_t sys)
{
	uint32_t id = (uint32_t)(uintptr_t)sys;
	const struct clock_desc *desc;
	uint16_t reg;
	uint8_t bit;
	uint32_t val;

	if (id >= XLNX_PS7_CLK_NUM) {
		return CLOCK_CONTROL_STATUS_UNKNOWN;
	}

	if (clock_is_aper(id)) {
		reg = APER_CLK_CTRL;
		bit = aper_bits[id];
	} else {
		desc = &clock_descs[id];
		if (desc->reg == 0U || desc->act_bit == CLK_NO_GATE) {
			/* Nothing gates it, so it runs whenever the PLLs do */
			return CLOCK_CONTROL_STATUS_ON;
		}
		reg = desc->reg;
		bit = desc->act_bit;
	}

	if (ps7_read(dev, reg, &val) != 0) {
		return CLOCK_CONTROL_STATUS_UNKNOWN;
	}

	return ((val & BIT(bit)) != 0U) ? CLOCK_CONTROL_STATUS_ON : CLOCK_CONTROL_STATUS_OFF;
}

static int clock_control_xlnx_ps7_init(const struct device *dev)
{
	const struct clock_control_xlnx_ps7_config *config = dev->config;

	if (!device_is_ready(config->syscon)) {
		LOG_ERR("%s: system level control registers not ready", dev->name);
		return -ENODEV;
	}

	return 0;
}

static DEVICE_API(clock_control, clock_control_xlnx_ps7_api) = {
	.on = clock_control_xlnx_ps7_on,
	.off = clock_control_xlnx_ps7_off,
	.get_rate = clock_control_xlnx_ps7_get_rate,
	.get_status = clock_control_xlnx_ps7_get_status,
};

#define CLOCK_CONTROL_XLNX_PS7_INIT(n)                                                             \
	static const struct clock_control_xlnx_ps7_config clock_control_xlnx_ps7_config_##n = {    \
		.syscon = DEVICE_DT_GET(DT_INST_PHANDLE(n, syscon)),                               \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.ps_clk_frequency = DT_INST_PROP(n, ps_clk_frequency),                             \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, clock_control_xlnx_ps7_init, NULL, NULL,                          \
			      &clock_control_xlnx_ps7_config_##n, PRE_KERNEL_1,                    \
			      CONFIG_CLOCK_CONTROL_XLNX_PS7_INIT_PRIORITY,                         \
			      &clock_control_xlnx_ps7_api);

DT_INST_FOREACH_STATUS_OKAY(CLOCK_CONTROL_XLNX_PS7_INIT)
