/*
 * Copyright (c) 2026 Radu Pirea
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT arm_pl310_cache

#include <zephyr/cache.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <cmsis_core.h>

/* Register offsets (ARM CoreLink Level 2 Cache Controller L2C-310 TRM) */
#define PL310_CACHE_ID      0x000
#define PL310_CACHE_TYPE    0x004
#define PL310_CONTROL       0x100
#define PL310_AUX_CONTROL   0x104
#define PL310_CACHE_SYNC    0x730
#define PL310_INV_PA        0x770
#define PL310_INV_WAY       0x77c
#define PL310_CLEAN_PA      0x7b0
#define PL310_CLEAN_WAY     0x7bc
#define PL310_CLEAN_INV_PA  0x7f0
#define PL310_CLEAN_INV_WAY 0x7fc

#define PL310_CONTROL_ENABLE BIT(0)

/* Number of ways the controller reports, from its auxiliary control register */
#define PL310_AUX_ASSOCIATIVITY_16 BIT(16)

/*
 * The controller's line length is fixed at eight words, and a maintenance
 * operation started by writing a register completes once that register reads
 * back with its low bit clear.
 */
#define PL310_LINE_SIZE 32U
#define PL310_OP_BUSY   BIT(0)

#define PL310_BASE DT_INST_REG_ADDR(0)

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "Exactly one L2C-310 cache controller is supported");

static inline uint32_t pl310_read(uint16_t offset)
{
	return sys_read32(PL310_BASE + offset);
}

static inline void pl310_write(uint16_t offset, uint32_t val)
{
	sys_write32(val, PL310_BASE + offset);
}

/**
 * @brief Wait for a maintenance operation to finish
 *
 * The controller performs an operation in the background and reports it done by
 * clearing the low bit of the register that started it.
 *
 * @param offset Offset of the register the operation was started through
 */
static inline void pl310_wait(uint16_t offset)
{
	while ((pl310_read(offset) & PL310_OP_BUSY) != 0U) {
	}
}

/**
 * @brief Drain the write buffers of the controller
 */
static inline void pl310_sync(void)
{
	pl310_write(PL310_CACHE_SYNC, 0U);
	pl310_wait(PL310_CACHE_SYNC);
}

/**
 * @brief Run a maintenance operation over a range of addresses
 *
 * The controller addresses its lines physically. Every mapping this driver can
 * be reached through is flat, so the address handed in is used as it is.
 *
 * @param offset Offset of the register performing the operation
 * @param addr   First address of the range
 * @param size   Length of the range in bytes
 */
static void pl310_range(uint16_t offset, void *addr, size_t size)
{
	uintptr_t line = ROUND_DOWN((uintptr_t)addr, PL310_LINE_SIZE);
	uintptr_t end = (uintptr_t)addr + size;

	for (; line < end; line += PL310_LINE_SIZE) {
		pl310_write(offset, (uint32_t)line);
		pl310_wait(offset);
	}

	pl310_sync();
}

/**
 * @brief Run a maintenance operation over every way of the cache
 *
 * @param offset Offset of the register performing the operation
 */
static void pl310_all(uint16_t offset)
{
	uint32_t ways = ((pl310_read(PL310_AUX_CONTROL) & PL310_AUX_ASSOCIATIVITY_16) != 0U)
				? BIT_MASK(16)
				: BIT_MASK(8);

	pl310_write(offset, ways);
	while ((pl310_read(offset) & ways) != 0U) {
	}

	pl310_sync();
}

/**
 * @brief Run the level one maintenance operation of a range
 *
 * @param addr First address of the range
 * @param size Length of the range in bytes
 * @param op   Operation to perform on each line
 */
static void l1_range(void *addr, size_t size, void (*op)(void *va))
{
	uintptr_t line = ROUND_DOWN((uintptr_t)addr, PL310_LINE_SIZE);
	uintptr_t end = (uintptr_t)addr + size;

	for (; line < end; line += PL310_LINE_SIZE) {
		op((void *)line);
	}

	__DSB();
}

void cache_data_enable(void)
{
	if ((pl310_read(PL310_CONTROL) & PL310_CONTROL_ENABLE) != 0U) {
		return;
	}

	pl310_all(PL310_INV_WAY);
	pl310_write(PL310_CONTROL, PL310_CONTROL_ENABLE);

	L1C_EnableCaches();
}

void cache_data_disable(void)
{
	if ((pl310_read(PL310_CONTROL) & PL310_CONTROL_ENABLE) == 0U) {
		return;
	}

	L1C_CleanInvalidateDCacheAll();
	__DSB();

	pl310_all(PL310_CLEAN_INV_WAY);
	pl310_write(PL310_CONTROL, 0U);
	pl310_sync();
}

/*
 * The level one cache sits above the level two one, so cleaning walks down and
 * invalidating walks up: cleaning pushes a line out of level one before level
 * two is told to push it to memory, while invalidating drops the level two copy
 * first, so that nothing can refill level one from it in between.
 */
int cache_data_flush_range(void *addr, size_t size)
{
	l1_range(addr, size, L1C_CleanDCacheMVA);
	pl310_range(PL310_CLEAN_PA, addr, size);

	return 0;
}

int cache_data_invd_range(void *addr, size_t size)
{
	pl310_range(PL310_INV_PA, addr, size);
	l1_range(addr, size, L1C_InvalidateDCacheMVA);

	return 0;
}

int cache_data_flush_and_invd_range(void *addr, size_t size)
{
	l1_range(addr, size, L1C_CleanInvalidateDCacheMVA);
	pl310_range(PL310_CLEAN_INV_PA, addr, size);

	return 0;
}

int cache_data_flush_all(void)
{
	L1C_CleanDCacheAll();
	__DSB();
	pl310_all(PL310_CLEAN_WAY);

	return 0;
}

int cache_data_invd_all(void)
{
	pl310_all(PL310_INV_WAY);
	L1C_InvalidateDCacheAll();
	__DSB();

	return 0;
}

int cache_data_flush_and_invd_all(void)
{
	L1C_CleanInvalidateDCacheAll();
	__DSB();
	pl310_all(PL310_CLEAN_INV_WAY);

	return 0;
}

/* The controller is unified, so the instruction cache it backs is the level one one */
void cache_instr_enable(void)
{
	L1C_EnableCaches();
}

void cache_instr_disable(void)
{
	L1C_DisableCaches();
}

int cache_instr_flush_all(void)
{
	return -ENOTSUP;
}

int cache_instr_invd_all(void)
{
	L1C_InvalidateICacheAll();

	return 0;
}

int cache_instr_flush_and_invd_all(void)
{
	return -ENOTSUP;
}

int cache_instr_flush_range(void *addr, size_t size)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(size);

	return -ENOTSUP;
}

int cache_instr_invd_range(void *addr, size_t size)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(size);

	return -ENOTSUP;
}

int cache_instr_flush_and_invd_range(void *addr, size_t size)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(size);

	return -ENOTSUP;
}

#ifdef CONFIG_DCACHE_LINE_SIZE_DETECT
size_t cache_data_line_size_get(void)
{
	return PL310_LINE_SIZE;
}
#endif /* CONFIG_DCACHE_LINE_SIZE_DETECT */

#ifdef CONFIG_ICACHE_LINE_SIZE_DETECT
size_t cache_instr_line_size_get(void)
{
	return PL310_LINE_SIZE;
}
#endif /* CONFIG_ICACHE_LINE_SIZE_DETECT */
