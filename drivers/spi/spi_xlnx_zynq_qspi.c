/*
 * Copyright (c) 2026 Radu Pirea
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT xlnx_zynq_qspi_1_0

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_xlnx_zynq_qspi, CONFIG_SPI_LOG_LEVEL);

#include "spi_context.h"
#include "spi_rtio.h"

/* Register offsets (Zynq-7000 TRM UG585 v1.13, appendix B.19 quad_spi) */
#define QSPI_CONFIG      0x00
#define QSPI_INT_STATUS  0x04
#define QSPI_INT_ENABLE  0x08
#define QSPI_INT_DISABLE 0x0c
#define QSPI_ENABLE      0x14
#define QSPI_TXD0        0x1c
#define QSPI_RXD         0x20
#define QSPI_TX_THRESH   0x28
#define QSPI_RX_THRESH   0x2c
#define QSPI_TXD1        0x80
#define QSPI_TXD2        0x84
#define QSPI_TXD3        0x88
#define QSPI_LINEAR_CFG  0xa0

/*
 * Configuration register. The bits the driver deliberately leaves clear are
 * named as well: the flash memory interface mode, the big endian access format
 * of the data registers, manual start and its enable, and the reserved
 * reference clock bit.
 */
#define QSPI_CONFIG_IFMODE     BIT(31)
#define QSPI_CONFIG_ENDIAN_BE  BIT(26)
#define QSPI_CONFIG_HOLDB_DR   BIT(19)
#define QSPI_CONFIG_KEEP       BIT(17)
#define QSPI_CONFIG_MANSTRT    BIT(16)
#define QSPI_CONFIG_MANSTRTEN  BIT(15)
#define QSPI_CONFIG_SSFORCE    BIT(14)
#define QSPI_CONFIG_PCS        BIT(10)
#define QSPI_CONFIG_REF_CLK    BIT(8)
#define QSPI_CONFIG_FWIDTH_32  GENMASK(7, 6)
#define QSPI_CONFIG_BAUD_MASK  GENMASK(5, 3)
#define QSPI_CONFIG_BAUD_SHIFT 3
#define QSPI_CONFIG_CPHA       BIT(2)
#define QSPI_CONFIG_CPOL       BIT(1)
#define QSPI_CONFIG_MSTREN     BIT(0)

/* Interrupt status/enable/disable registers */
#define QSPI_IXR_TX_UNDERFLOW BIT(6)
#define QSPI_IXR_RX_FULL      BIT(5)
#define QSPI_IXR_RX_NOT_EMPTY BIT(4)
#define QSPI_IXR_TX_FULL      BIT(3)
#define QSPI_IXR_TX_NOT_FULL  BIT(2)
#define QSPI_IXR_RX_OVERFLOW  BIT(0)
#define QSPI_IXR_ALL                                                                               \
	(QSPI_IXR_TX_UNDERFLOW | QSPI_IXR_RX_FULL | QSPI_IXR_RX_NOT_EMPTY | QSPI_IXR_TX_FULL |     \
	 QSPI_IXR_TX_NOT_FULL | QSPI_IXR_RX_OVERFLOW)

/* Enable register */
#define QSPI_ENABLE_ENABLE BIT(0)

/* Linear configuration register */
#define QSPI_LCFG_U_PAGE BIT(28)

/* Depth of the TX and RX FIFOs, in 32-bit words */
#define QSPI_FIFO_DEPTH 63

/*
 * Number of words pushed into the TX FIFO before the words shifted in are read
 * back out of the RX FIFO. A word is only added to the RX FIFO once its TX
 * counterpart has been shifted out, so keeping a burst well below the depth of
 * the FIFOs leaves margin against overrunning either of them.
 */
#define QSPI_BURST_WORDS 32

/* Smallest and largest supported SCLK divider, both powers of two */
#define QSPI_BAUD_DIV_MIN 2U
#define QSPI_BAUD_DIV_MAX 256U

/*
 * A word pushed into the TX FIFO is shifted out and the word shifted in takes
 * its place in the RX FIFO, so a transfer never takes longer than the time it
 * takes to shift QSPI_FIFO_DEPTH words out at the lowest supported frequency.
 */
#define QSPI_XFER_TIMEOUT_US 100000

struct spi_xlnx_zynq_qspi_config {
	DEVICE_MMIO_ROM;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

struct spi_xlnx_zynq_qspi_data {
	DEVICE_MMIO_RAM;
	struct spi_context ctx;
};

static inline uint32_t qspi_read(const struct device *dev, uint16_t offset)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + offset);
}

static inline void qspi_write(const struct device *dev, uint16_t offset, uint32_t val)
{
	sys_write32(val, DEVICE_MMIO_GET(dev) + offset);
}

/*
 * Both FIFO status bits compare the FIFO level against the matching threshold
 * register, which the initialization sets to one word: the RX bit then reads as
 * set while the RX FIFO holds at least one word, and the TX bit while the TX
 * FIFO holds none (UG585 v1.13, appendix B.26, Intr_status_REG).
 */
static inline bool qspi_rx_not_empty(const struct device *dev)
{
	return (qspi_read(dev, QSPI_INT_STATUS) & QSPI_IXR_RX_NOT_EMPTY) != 0;
}

static inline bool qspi_tx_empty(const struct device *dev)
{
	return (qspi_read(dev, QSPI_INT_STATUS) & QSPI_IXR_TX_NOT_FULL) != 0;
}

/**
 * @brief Assert or de-assert the controller's chip select line
 *
 * The chip select line is active low and is driven by the PCS bit of the
 * configuration register while manual chip select is enabled.
 *
 * @param dev    QSPI controller device
 * @param slave  Index of the chip select line to drive
 * @param assert Whether to assert the chip select line
 */
static void qspi_cs_control(const struct device *dev, uint16_t slave, bool assert)
{
	uint32_t reg;

	reg = qspi_read(dev, QSPI_LINEAR_CFG);
	if (slave == 0U) {
		reg &= ~QSPI_LCFG_U_PAGE;
	} else {
		reg |= QSPI_LCFG_U_PAGE;
	}
	qspi_write(dev, QSPI_LINEAR_CFG, reg);

	reg = qspi_read(dev, QSPI_CONFIG);
	if (assert) {
		reg &= ~QSPI_CONFIG_PCS;
	} else {
		reg |= QSPI_CONFIG_PCS;
	}
	qspi_write(dev, QSPI_CONFIG, reg);
}

/**
 * @brief Drain whatever the RX FIFO still holds
 *
 * Called before a transfer so that the word count in the RX FIFO matches the
 * number of words pushed into the TX FIFO by that transfer.
 *
 * @param dev QSPI controller device
 */
static void qspi_rx_drain(const struct device *dev)
{
	while (qspi_rx_not_empty(dev)) {
		(void)qspi_read(dev, QSPI_RXD);
	}

	qspi_write(dev, QSPI_INT_STATUS, QSPI_IXR_ALL);
}

/**
 * @brief Wait for the RX FIFO to hold at least one word
 *
 * @param dev QSPI controller device
 * @retval 0 A word is available in the RX FIFO
 * @retval -ETIMEDOUT No word became available before the transfer timeout
 */
static int qspi_rx_wait(const struct device *dev)
{
	if (!WAIT_FOR(qspi_rx_not_empty(dev), QSPI_XFER_TIMEOUT_US, k_busy_wait(1))) {
		LOG_ERR("%s: timeout waiting for RX data", dev->name);
		return -ETIMEDOUT;
	}

	/*
	 * The status bit is raised before the word it announces can actually be
	 * read, by a latency the controller documentation attributes to a clock
	 * domain crossing and expects the entry into an interrupt handler to
	 * cover (UG585 v1.13, section 12.2.4). Nothing covers it while polling,
	 * so wait it out.
	 */
	k_busy_wait(1);

	return 0;
}

/**
 * @brief Wait for the TX FIFO to drain
 *
 * @param dev QSPI controller device
 * @retval 0 The TX FIFO is empty
 * @retval -ETIMEDOUT The TX FIFO did not drain before the transfer timeout
 */
static int qspi_tx_wait_empty(const struct device *dev)
{
	if (!WAIT_FOR(qspi_tx_empty(dev), QSPI_XFER_TIMEOUT_US, k_busy_wait(1))) {
		LOG_ERR("%s: timeout waiting for the TX FIFO to drain", dev->name);
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * @brief Shift a buffer of up to four bytes out and the reply in
 *
 * The controller determines the number of bytes to shift out from the TX data
 * register that is written: the register for a full word, or one of the three
 * registers taking one, two or three bytes, from the least significant end of
 * the value written (UG585 v1.13, table 12-1).
 *
 * A received byte enters the RX data register at its most significant end and
 * pushes the bytes before it down, so after a partial word the bytes sit at the
 * top of the register, oldest first, and are read back from an offset within
 * it. The examples in UG585 v1.13, section 12.3.5 spell this out: two bytes
 * received as 0x00 then 0x03 read back as 0x0300_0000, and four received as
 * 0xEF, 0xAC, 0x68, 0x24 read back as 0x2468_ACEF.
 *
 * The controller requires an empty TX FIFO both before and after an access to
 * one of the partial word registers, so this function waits for the FIFO to
 * drain before writing and leaves it empty by reading the reply back.
 *
 * @param dev QSPI controller device
 * @param tx  Buffer to shift out, or NULL to shift out zeroes
 * @param rx  Buffer to store the reply in, or NULL to discard it
 * @param len Number of bytes to transfer, 1 to 4
 * @retval 0 on success
 * @retval -ETIMEDOUT if the controller did not return the shifted in word
 */
static int qspi_xfer_partial(const struct device *dev, const uint8_t *tx, uint8_t *rx, size_t len)
{
	static const uint16_t txd_offset[4] = {QSPI_TXD1, QSPI_TXD2, QSPI_TXD3, QSPI_TXD0};
	uint32_t word = 0U;
	int err;

	__ASSERT_NO_MSG(len >= 1U && len <= 4U);

	if (tx != NULL) {
		memcpy(&word, tx, len);
	}

	err = qspi_tx_wait_empty(dev);
	if (err != 0) {
		return err;
	}

	qspi_write(dev, txd_offset[len - 1U], word);

	err = qspi_rx_wait(dev);
	if (err != 0) {
		return err;
	}

	word = qspi_read(dev, QSPI_RXD);

	if (rx != NULL) {
		memcpy(rx, (uint8_t *)&word + (4U - len), len);
	}

	return 0;
}

/**
 * @brief Shift a buffer out and the reply in
 *
 * Whole words are shifted out in bursts. Any trailing bytes are shifted
 * out one partial word at a time, which requires an empty TX FIFO and is
 * therefore only done once the words ahead of them have been shifted in.
 *
 * @param dev QSPI controller device
 * @param tx  Buffer to shift out, or NULL to shift out zeroes
 * @param rx  Buffer to store the reply in, or NULL to discard it
 * @param len Number of bytes to transfer
 * @retval 0 on success
 * @retval -ETIMEDOUT if the controller did not return a shifted in word
 */
static int qspi_xfer(const struct device *dev, const uint8_t *tx, uint8_t *rx, size_t len)
{
	int err;

	while (len >= sizeof(uint32_t)) {
		size_t words = MIN(len / sizeof(uint32_t), (size_t)QSPI_BURST_WORDS);
		size_t i;

		for (i = 0U; i < words; i++) {
			uint32_t word = 0U;

			if (tx != NULL) {
				memcpy(&word, &tx[i * sizeof(uint32_t)], sizeof(uint32_t));
			}

			qspi_write(dev, QSPI_TXD0, word);
		}

		for (i = 0U; i < words; i++) {
			uint32_t word;

			err = qspi_rx_wait(dev);
			if (err != 0) {
				return err;
			}

			word = qspi_read(dev, QSPI_RXD);

			if (rx != NULL) {
				memcpy(&rx[i * sizeof(uint32_t)], &word, sizeof(uint32_t));
			}
		}

		len -= words * sizeof(uint32_t);
		if (tx != NULL) {
			tx += words * sizeof(uint32_t);
		}
		if (rx != NULL) {
			rx += words * sizeof(uint32_t);
		}
	}

	if (len > 0U) {
		err = qspi_xfer_partial(dev, tx, rx, len);
		if (err != 0) {
			return err;
		}
	}

	return 0;
}

/**
 * @brief Apply an SPI configuration to the controller
 *
 * @param dev    QSPI controller device
 * @param config Requested SPI configuration
 * @retval 0 on success
 * @retval -ENOTSUP The configuration requests an unsupported mode
 * @retval -EINVAL The configuration requests an unusable chip select or
 *                 frequency
 */
static int qspi_configure(const struct device *dev, const struct spi_config *config)
{
	const struct spi_xlnx_zynq_qspi_config *dev_config = dev->config;
	struct spi_xlnx_zynq_qspi_data *data = dev->data;
	uint32_t reference;
	uint32_t divider;
	uint32_t baud;
	uint32_t reg;
	int err;

	if (spi_context_configured(&data->ctx, config)) {
		return 0;
	}

	if (SPI_OP_MODE_GET(config->operation) != SPI_OP_MODE_CONTROLLER) {
		LOG_ERR("%s: peripheral mode is not supported", dev->name);
		return -ENOTSUP;
	}

	if ((config->operation & SPI_MODE_LOOP) != 0U) {
		LOG_ERR("%s: loopback mode is not supported", dev->name);
		return -ENOTSUP;
	}

	if ((config->operation & SPI_TRANSFER_LSB) != 0U) {
		LOG_ERR("%s: LSB first is not supported", dev->name);
		return -ENOTSUP;
	}

	if (SPI_WORD_SIZE_GET(config->operation) != 8) {
		LOG_ERR("%s: only 8 bit words are supported", dev->name);
		return -ENOTSUP;
	}

	/*
	 * The controller only supports the two clock modes in which the clock
	 * phase and the clock polarity match (UG585 v1.13, appendix B.26,
	 * Config_reg), so SPI modes 1 and 2 cannot be reached.
	 */
	if (((config->operation & SPI_MODE_CPHA) != 0U) !=
	    ((config->operation & SPI_MODE_CPOL) != 0U)) {
		LOG_ERR("%s: only SPI modes 0 and 3 are supported", dev->name);
		return -ENOTSUP;
	}

	if (IS_ENABLED(CONFIG_SPI_EXTENDED_MODES) &&
	    (config->operation & SPI_LINES_MASK) != SPI_LINES_SINGLE) {
		LOG_ERR("%s: only single line transfers are supported", dev->name);
		return -ENOTSUP;
	}

	if (config->peripheral > 1U) {
		LOG_ERR("%s: invalid chip select %u", dev->name, config->peripheral);
		return -EINVAL;
	}

	if (config->frequency == 0U) {
		LOG_ERR("%s: invalid frequency", dev->name);
		return -EINVAL;
	}

	err = clock_control_get_rate(dev_config->clock_dev, dev_config->clock_subsys, &reference);
	if (err != 0) {
		LOG_ERR("%s: cannot read the reference clock (err %d)", dev->name, err);
		return err;
	}

	/*
	 * The divider field selects a power of two between 2 and 256. Round the
	 * requested frequency down, so the resulting SCLK frequency never
	 * exceeds it.
	 */
	baud = 0U;
	for (divider = QSPI_BAUD_DIV_MIN; divider < QSPI_BAUD_DIV_MAX; divider <<= 1) {
		if ((reference / divider) <= config->frequency) {
			break;
		}
		baud++;
	}

	if ((reference / divider) > config->frequency) {
		LOG_ERR("%s: cannot reach %u Hz from a %u Hz reference clock", dev->name,
			config->frequency, reference);
		return -EINVAL;
	}

	reg = qspi_read(dev, QSPI_CONFIG);
	reg &= ~(QSPI_CONFIG_BAUD_MASK | QSPI_CONFIG_CPHA | QSPI_CONFIG_CPOL);
	reg |= baud << QSPI_CONFIG_BAUD_SHIFT;

	if ((config->operation & SPI_MODE_CPHA) != 0U) {
		reg |= QSPI_CONFIG_CPHA;
	}
	if ((config->operation & SPI_MODE_CPOL) != 0U) {
		reg |= QSPI_CONFIG_CPOL;
	}

	qspi_write(dev, QSPI_CONFIG, reg);

	data->ctx.config = config;

	LOG_DBG("%s: SCLK %u Hz (reference clock %u Hz divided by %u)", dev->name,
		reference / divider, reference, divider);

	return 0;
}

static int spi_xlnx_zynq_qspi_transceive(const struct device *dev, const struct spi_config *config,
					 const struct spi_buf_set *tx_bufs,
					 const struct spi_buf_set *rx_bufs)
{
	struct spi_xlnx_zynq_qspi_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	int err;

	spi_context_lock(ctx, false, NULL, NULL, config);

	err = qspi_configure(dev, config);
	if (err != 0) {
		goto out;
	}

	spi_context_buffers_setup(ctx, tx_bufs, rx_bufs, 1);

	qspi_rx_drain(dev);
	qspi_cs_control(dev, config->peripheral, true);
	spi_context_cs_control(ctx, true);

	while (spi_context_tx_on(ctx) || spi_context_rx_on(ctx)) {
		size_t chunk;

		if (ctx->tx_len != 0U && ctx->rx_len != 0U) {
			chunk = MIN(ctx->tx_len, ctx->rx_len);
		} else {
			chunk = MAX(ctx->tx_len, ctx->rx_len);
		}

		err = qspi_xfer(dev, spi_context_tx_buf_on(ctx) ? ctx->tx_buf : NULL,
				spi_context_rx_buf_on(ctx) ? ctx->rx_buf : NULL, chunk);
		if (err != 0) {
			break;
		}

		spi_context_update_tx(ctx, 1, chunk);
		spi_context_update_rx(ctx, 1, chunk);
	}

	spi_context_cs_control(ctx, false);

	if ((config->operation & SPI_HOLD_ON_CS) == 0U) {
		qspi_cs_control(dev, config->peripheral, false);
	}

out:
	spi_context_release(ctx, err);

	return err;
}

#ifdef CONFIG_SPI_ASYNC
static int spi_xlnx_zynq_qspi_transceive_async(const struct device *dev,
					       const struct spi_config *config,
					       const struct spi_buf_set *tx_bufs,
					       const struct spi_buf_set *rx_bufs, spi_callback_t cb,
					       void *userdata)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(config);
	ARG_UNUSED(tx_bufs);
	ARG_UNUSED(rx_bufs);
	ARG_UNUSED(cb);
	ARG_UNUSED(userdata);

	return -ENOTSUP;
}
#endif /* CONFIG_SPI_ASYNC */

static int spi_xlnx_zynq_qspi_release(const struct device *dev, const struct spi_config *config)
{
	struct spi_xlnx_zynq_qspi_data *data = dev->data;

	spi_context_cs_control(&data->ctx, false);
	qspi_cs_control(dev, config->peripheral, false);
	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

/**
 * @brief Bring the controller into a known I/O mode state
 *
 * The controller is taken out of the linear address mode a boot loader may have
 * left it in, the chip select line is de-asserted and the FIFOs are emptied.
 *
 * The flash memory interface mode is turned off, which leaves the controller in
 * the legacy SPI mode it has to be in to serve as an ordinary SPI controller:
 * the first byte of a transfer is then no longer taken for a flash instruction
 * whose encoding reconfigures the width of the data lines, and the lines beyond
 * the first are held as inputs (UG585 v1.13, appendix B.26, Config_reg).
 *
 * Manual chip select is turned on, which makes the PCS bit drive the chip select
 * line directly. This is what allows a transfer to span several FIFO loads: with
 * an automatic chip select the controller would end the transfer as soon as the
 * TX FIFO ran empty (UG585 v1.13, sections 12.2.2 and 12.2.4). Manual start is
 * left off, so words are shifted out as they are written.
 *
 * @param dev QSPI controller device
 * @retval 0 on success
 * @retval -errno as reported by the configuration of a GPIO chip select line
 */
static int spi_xlnx_zynq_qspi_init(const struct device *dev)
{
	struct spi_xlnx_zynq_qspi_data *data = dev->data;
	uint32_t reg;
	int err;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	qspi_write(dev, QSPI_ENABLE, 0U);
	qspi_write(dev, QSPI_INT_DISABLE, QSPI_IXR_ALL);

	/* Leave the linear address mode the boot loader reads the flash in */
	qspi_write(dev, QSPI_LINEAR_CFG, 0U);

	qspi_rx_drain(dev);

	/*
	 * Bit 17 of the configuration register is reserved and documented as not
	 * to be modified, so the register is read back rather than written from
	 * scratch. Everything else is driven to a known state, including the
	 * endianness the TX and RX data registers are accessed in and the
	 * reserved reference clock bit that has to stay zero.
	 */
	reg = qspi_read(dev, QSPI_CONFIG) & QSPI_CONFIG_KEEP;

	/*
	 * Driving the HOLD and write protect lines rather than leaving them to
	 * external pull-ups is recommended for every mode of operation
	 * (UG585 v1.13, appendix B.26, Config_reg, Holdb_dr).
	 */
	reg |= QSPI_CONFIG_HOLDB_DR | QSPI_CONFIG_SSFORCE | QSPI_CONFIG_PCS |
	       QSPI_CONFIG_FWIDTH_32 | QSPI_CONFIG_MSTREN;

	qspi_write(dev, QSPI_CONFIG, reg);

	/*
	 * One word, so that the RX FIFO status bit reads as set while the FIFO
	 * holds anything at all and the TX one while it holds nothing.
	 */
	qspi_write(dev, QSPI_TX_THRESH, 1U);
	qspi_write(dev, QSPI_RX_THRESH, 1U);

	qspi_write(dev, QSPI_ENABLE, QSPI_ENABLE_ENABLE);

	err = spi_context_cs_configure_all(&data->ctx);
	if (err != 0) {
		return err;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, spi_xlnx_zynq_qspi_api) = {
	.transceive = spi_xlnx_zynq_qspi_transceive,
#ifdef CONFIG_SPI_ASYNC
	.transceive_async = spi_xlnx_zynq_qspi_transceive_async,
#endif /* CONFIG_SPI_ASYNC */
#ifdef CONFIG_SPI_RTIO
	.iodev_submit = spi_rtio_iodev_default_submit,
#endif /* CONFIG_SPI_RTIO */
	.release = spi_xlnx_zynq_qspi_release,
};

#define SPI_XLNX_ZYNQ_QSPI_INIT(n)                                                                 \
	static const struct spi_xlnx_zynq_qspi_config spi_xlnx_zynq_qspi_config_##n = {            \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                              \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)(uintptr_t)DT_INST_CLOCKS_CELL(n, id),     \
	};                                                                                         \
                                                                                                   \
	static struct spi_xlnx_zynq_qspi_data spi_xlnx_zynq_qspi_data_##n = {                      \
		SPI_CONTEXT_INIT_LOCK(spi_xlnx_zynq_qspi_data_##n, ctx),                           \
		SPI_CONTEXT_INIT_SYNC(spi_xlnx_zynq_qspi_data_##n, ctx),                           \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)};                             \
                                                                                                   \
	SPI_DEVICE_DT_INST_DEFINE(n, spi_xlnx_zynq_qspi_init, NULL, &spi_xlnx_zynq_qspi_data_##n,  \
				  &spi_xlnx_zynq_qspi_config_##n, POST_KERNEL,                     \
				  CONFIG_SPI_INIT_PRIORITY, &spi_xlnx_zynq_qspi_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_XLNX_ZYNQ_QSPI_INIT)
