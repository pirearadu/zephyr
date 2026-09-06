/*
 * Copyright (c) 2026 Radu Pirea
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_CLOCK_XLNX_PS7_CLKC_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_CLOCK_XLNX_PS7_CLKC_H_

/**
 * @name Xilinx Zynq-7000 clock identifiers
 *
 * Identifiers of the clocks the PS clock subsystem generates, in the order the
 * bindings of this controller have always listed them.
 *
 * @{
 */
#define XLNX_PS7_CLK_ARMPLL     0
#define XLNX_PS7_CLK_DDRPLL     1
#define XLNX_PS7_CLK_IOPLL      2
#define XLNX_PS7_CLK_CPU_6OR4X  3
#define XLNX_PS7_CLK_CPU_3OR2X  4
#define XLNX_PS7_CLK_CPU_2X     5
#define XLNX_PS7_CLK_CPU_1X     6
#define XLNX_PS7_CLK_DDR2X      7
#define XLNX_PS7_CLK_DDR3X      8
#define XLNX_PS7_CLK_DCI        9
#define XLNX_PS7_CLK_LQSPI      10
#define XLNX_PS7_CLK_SMC        11
#define XLNX_PS7_CLK_PCAP       12
#define XLNX_PS7_CLK_GEM0       13
#define XLNX_PS7_CLK_GEM1       14
#define XLNX_PS7_CLK_FCLK0      15
#define XLNX_PS7_CLK_FCLK1      16
#define XLNX_PS7_CLK_FCLK2      17
#define XLNX_PS7_CLK_FCLK3      18
#define XLNX_PS7_CLK_CAN0       19
#define XLNX_PS7_CLK_CAN1       20
#define XLNX_PS7_CLK_SDIO0      21
#define XLNX_PS7_CLK_SDIO1      22
#define XLNX_PS7_CLK_UART0      23
#define XLNX_PS7_CLK_UART1      24
#define XLNX_PS7_CLK_SPI0       25
#define XLNX_PS7_CLK_SPI1       26
#define XLNX_PS7_CLK_DMA        27
#define XLNX_PS7_CLK_USB0_APER  28
#define XLNX_PS7_CLK_USB1_APER  29
#define XLNX_PS7_CLK_GEM0_APER  30
#define XLNX_PS7_CLK_GEM1_APER  31
#define XLNX_PS7_CLK_SDIO0_APER 32
#define XLNX_PS7_CLK_SDIO1_APER 33
#define XLNX_PS7_CLK_SPI0_APER  34
#define XLNX_PS7_CLK_SPI1_APER  35
#define XLNX_PS7_CLK_CAN0_APER  36
#define XLNX_PS7_CLK_CAN1_APER  37
#define XLNX_PS7_CLK_I2C0_APER  38
#define XLNX_PS7_CLK_I2C1_APER  39
#define XLNX_PS7_CLK_UART0_APER 40
#define XLNX_PS7_CLK_UART1_APER 41
#define XLNX_PS7_CLK_GPIO_APER  42
#define XLNX_PS7_CLK_LQSPI_APER 43
#define XLNX_PS7_CLK_SMC_APER   44
#define XLNX_PS7_CLK_SWDT       45
#define XLNX_PS7_CLK_DBG_TRC    46
#define XLNX_PS7_CLK_DBG_APB    47
/** @} */

#define XLNX_PS7_CLK_NUM 48

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_CLOCK_XLNX_PS7_CLKC_H_ */
