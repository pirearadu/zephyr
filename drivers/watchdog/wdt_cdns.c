/*
 * Copyright (c) 2026 Radu Pirea
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT cdns_wdt_r1p2

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wdt_cdns, CONFIG_WDT_LOG_LEVEL);

/* Register offsets (Zynq-7000 TRM UG585 v1.13, appendix B.31 swdt) */
#define WDT_MODE    0x00
#define WDT_CONTROL 0x04
#define WDT_RESTART 0x08
#define WDT_STATUS  0x0c

/*
 * Zero mode register. Writes only take effect while the access key is present
 * in the same write, and the key itself reads back as zero. Bits 6 to 4 are
 * reserved with a non-zero reset value and bits 8 to 7 hold the length of the
 * interrupt pulse, so a write preserves whatever those hold.
 */
#define WDT_MODE_ZKEY       (0xabcU << 12)
#define WDT_MODE_PRESERVE   GENMASK(8, 4)
#define WDT_MODE_IRQEN      BIT(2)
#define WDT_MODE_RSTEN      BIT(1)
#define WDT_MODE_WDEN       BIT(0)
#define WDT_MODE_WRITE_MASK GENMASK(2, 0)

/* Counter control register */
#define WDT_CONTROL_CKEY        (0x248U << 14)
#define WDT_CONTROL_CRV_MASK    GENMASK(13, 2)
#define WDT_CONTROL_CRV_SHIFT   2
#define WDT_CONTROL_CLKSEL_MASK GENMASK(1, 0)

/* Restart register: writing the key reloads the prescaler and the counter */
#define WDT_RESTART_KEY 0x1999U

/*
 * The counter is restarted with the value 0xNNNFFF, where NNN is the 12 bit
 * counter restart value of the control register, so the counter always ends in
 * twelve set bits and only its top twelve bits are programmable.
 */
#define WDT_COUNTER_FIXED_BITS 12
#define WDT_COUNTER_FIXED_MASK BIT_MASK(WDT_COUNTER_FIXED_BITS)
#define WDT_CRV_MAX            BIT_MASK(12)

/* The prescaler divides by 8, 64, 512 or 4096 */
#define WDT_CLKSEL_MAX     3U
#define WDT_PRESCALER(sel) (8U << (3U * (sel)))

struct wdt_cdns_config {
	DEVICE_MMIO_ROM;
	uint32_t clock_frequency;
	void (*irq_config)(void);
};

struct wdt_cdns_data {
	DEVICE_MMIO_RAM;
	struct k_spinlock lock;
	wdt_callback_t callback;
	uint32_t control;
	uint8_t flags;
	bool timeout_installed;
	bool started;
};

static inline uint32_t wdt_read(const struct device *dev, uint16_t offset)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + offset);
}

static inline void wdt_write(const struct device *dev, uint16_t offset, uint32_t val)
{
	sys_write32(val, DEVICE_MMIO_GET(dev) + offset);
}

/**
 * @brief Write the zero mode register
 *
 * @param dev  Watchdog device
 * @param bits The enable bits to write; every other field is preserved
 */
static void wdt_cdns_write_mode(const struct device *dev, uint32_t bits)
{
	uint32_t reg = wdt_read(dev, WDT_MODE) & WDT_MODE_PRESERVE;

	wdt_write(dev, WDT_MODE, WDT_MODE_ZKEY | reg | (bits & WDT_MODE_WRITE_MASK));
}

/**
 * @brief Turn a timeout in milliseconds into a control register value
 *
 * The counter counts the input clock down through a prescaler, so a timeout is
 * expressible as a prescaler division ratio and a counter restart value. The
 * smallest pair that covers the requested timeout is picked, which rounds the
 * timeout up: a watchdog that fires early is worse than one that fires late.
 *
 * @param dev     Watchdog device
 * @param timeout Requested timeout in milliseconds
 * @param control Where to store the counter restart value and the prescaler
 *                division ratio, in their register positions
 * @retval 0 on success
 * @retval -EINVAL The timeout is longer than the counter can express
 */
static int wdt_cdns_control_for_timeout(const struct device *dev, uint32_t timeout,
					uint32_t *control)
{
	const struct wdt_cdns_config *config = dev->config;
	uint64_t cycles;
	uint32_t clksel;

	cycles = ((uint64_t)timeout * (uint64_t)config->clock_frequency) / 1000U;

	for (clksel = 0U; clksel <= WDT_CLKSEL_MAX; clksel++) {
		uint64_t counts = DIV_ROUND_UP(cycles, WDT_PRESCALER(clksel));
		uint64_t crv = counts >> WDT_COUNTER_FIXED_BITS;

		if (crv <= WDT_CRV_MAX) {
			*control = ((uint32_t)crv << WDT_CONTROL_CRV_SHIFT) | clksel;
			LOG_DBG("%s: %u ms is %u counts of the clock divided by %u", dev->name,
				timeout,
				(uint32_t)(((uint32_t)crv << WDT_COUNTER_FIXED_BITS) |
					   WDT_COUNTER_FIXED_MASK),
				(uint32_t)WDT_PRESCALER(clksel));
			return 0;
		}
	}

	LOG_ERR("%s: timeout of %u ms is out of range", dev->name, timeout);

	return -EINVAL;
}

static int wdt_cdns_install_timeout(const struct device *dev, const struct wdt_timeout_cfg *cfg)
{
	struct wdt_cdns_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t control;
	int err;

	if (cfg->window.min != 0U) {
		LOG_ERR("%s: windowed timeouts are not supported", dev->name);
		return -EINVAL;
	}

	/*
	 * The reset output goes to the reset subsystem of the whole processing
	 * system, so it cannot be narrowed down to the CPU core.
	 */
	if ((cfg->flags & WDT_FLAG_RESET_MASK) == WDT_FLAG_RESET_CPU_CORE) {
		LOG_ERR("%s: resetting only the CPU core is not supported", dev->name);
		return -ENOTSUP;
	}

	if (cfg->callback == NULL && (cfg->flags & WDT_FLAG_RESET_MASK) == WDT_FLAG_RESET_NONE) {
		LOG_ERR("%s: a timeout without a reset needs a callback", dev->name);
		return -EINVAL;
	}

	err = wdt_cdns_control_for_timeout(dev, cfg->window.max, &control);
	if (err != 0) {
		return err;
	}

	key = k_spin_lock(&data->lock);

	if (data->timeout_installed) {
		err = -ENOMEM;
		goto out;
	}

	if (data->started) {
		err = -EBUSY;
		goto out;
	}

	data->control = control;
	data->callback = cfg->callback;
	data->flags = cfg->flags;
	data->timeout_installed = true;
	err = 0;

out:
	k_spin_unlock(&data->lock, key);

	return err;
}

static int wdt_cdns_setup(const struct device *dev, uint8_t options)
{
	struct wdt_cdns_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t mode;
	int err;

	/*
	 * The counter halts on its own while the CPU is halted by a debugger,
	 * which is what the matching option asks for, but it cannot be told to
	 * keep running, nor to stop while the CPU sleeps.
	 */
	if ((options & WDT_OPT_PAUSE_IN_SLEEP) != 0U) {
		LOG_ERR("%s: pausing while the CPU sleeps is not supported", dev->name);
		return -ENOTSUP;
	}

	key = k_spin_lock(&data->lock);

	if (!data->timeout_installed) {
		err = -EINVAL;
		goto out;
	}

	if (data->started) {
		err = -EBUSY;
		goto out;
	}

	mode = WDT_MODE_WDEN;
	if ((data->flags & WDT_FLAG_RESET_MASK) != WDT_FLAG_RESET_NONE) {
		mode |= WDT_MODE_RSTEN;
	}
	if (data->callback != NULL) {
		mode |= WDT_MODE_IRQEN;
	}

	/* UG585 v1.13, section 8.4.5: reload values first, then enable */
	wdt_write(dev, WDT_CONTROL, WDT_CONTROL_CKEY | data->control);
	wdt_write(dev, WDT_RESTART, WDT_RESTART_KEY);
	wdt_cdns_write_mode(dev, mode);

	data->started = true;
	err = 0;

out:
	k_spin_unlock(&data->lock, key);

	return err;
}

static int wdt_cdns_disable(const struct device *dev)
{
	struct wdt_cdns_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	int err;

	if (!data->started) {
		err = -EFAULT;
		goto out;
	}

	wdt_cdns_write_mode(dev, 0U);
	data->started = false;
	data->timeout_installed = false;
	data->callback = NULL;
	data->flags = 0U;
	err = 0;

out:
	k_spin_unlock(&data->lock, key);

	return err;
}

static int wdt_cdns_feed(const struct device *dev, int channel_id)
{
	struct wdt_cdns_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	int err;

	if (channel_id != 0 || !data->started) {
		err = -EINVAL;
		goto out;
	}

	wdt_write(dev, WDT_RESTART, WDT_RESTART_KEY);
	err = 0;

out:
	k_spin_unlock(&data->lock, key);

	return err;
}

/*
 * The reset output is asserted about one CPU_1x cycle after the counter reaches
 * zero, so a callback runs on borrowed time and is not expected to return.
 */
static void wdt_cdns_isr(const struct device *dev)
{
	struct wdt_cdns_data *data = dev->data;

	if (data->callback != NULL) {
		data->callback(dev, 0);
	}
}

static int wdt_cdns_init(const struct device *dev)
{
	const struct wdt_cdns_config *config = dev->config;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	if (IS_ENABLED(CONFIG_WDT_DISABLE_AT_BOOT)) {
		wdt_cdns_write_mode(dev, 0U);
	}

	config->irq_config();

	return 0;
}

static DEVICE_API(wdt, wdt_cdns_api) = {
	.setup = wdt_cdns_setup,
	.disable = wdt_cdns_disable,
	.install_timeout = wdt_cdns_install_timeout,
	.feed = wdt_cdns_feed,
};

#define WDT_CDNS_INIT(n)                                                                           \
	static void wdt_cdns_irq_config_##n(void)                                                  \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), wdt_cdns_isr,               \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
                                                                                                   \
	static const struct wdt_cdns_config wdt_cdns_config_##n = {                                \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                              \
		.clock_frequency = DT_INST_PROP(n, clock_frequency),                               \
		.irq_config = wdt_cdns_irq_config_##n,                                             \
	};                                                                                         \
                                                                                                   \
	static struct wdt_cdns_data wdt_cdns_data_##n;                                             \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, wdt_cdns_init, NULL, &wdt_cdns_data_##n, &wdt_cdns_config_##n,    \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &wdt_cdns_api);

DT_INST_FOREACH_STATUS_OKAY(WDT_CDNS_INIT)
