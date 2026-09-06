.. zephyr:board:: pynq_z1

Overview
********

The `Digilent PYNQ-Z1`_ is a development board built around the Xilinx Zynq-7000 XC7Z020 All
Programmable System-on-Chip (AP SoC), which tightly integrates a dual-core ARM Cortex-A9 processing
system (PS) with Xilinx 7-series programmable logic (PL). The board was designed for the PYNQ
open-source framework, but the processing system can equally well run a bare-metal RTOS such as
Zephyr.

Hardware
********

- Xilinx Zynq-7000 XC7Z020-1CLG400C AP SoC (dual-core ARM Cortex-A9 @ 650 MHz)
- 512 MiB DDR3 with a 16-bit bus
- 16 MiB Quad-SPI flash on QSPI (``MIO1``..``MIO6``, feedback clock on ``MIO8``)
- microSD slot
- USB-UART bridge on UART0 (``MIO14``/``MIO15``)
- Gigabit Ethernet (GEM0, RGMII on ``MIO16``..``MIO27``, MDIO on ``MIO52``/``MIO53``,
  Realtek RTL8211E PHY at MDIO address 1)
- USB 2.0 OTG (USB0, ``MIO28``..``MIO39``)
- Two Pmod ports, an Arduino/chipKIT shield connector, HDMI in/out and audio, all attached to the
  programmable logic

Supported Features
==================

.. zephyr:board-supported-hw::

The system watchdog is enabled and clocked by CPU_1x. It resets the whole processing
system on expiry, and its counter halts on its own while a debugger holds the CPU.

The QSPI controller is enabled, but no flash node is defined for it: the fitted 16 MiB device
differs between board revisions. Add a node for the device actually fitted as a child of
``&qspi`` in an application overlay. The driver operates the controller in single line I/O
mode, so the quad read and write commands of the flash are not used.

GEM0 is enabled by default. The GEM driver has no pinctrl support, so the RGMII and MDIO
multiplexing is left to the PS initialization performed by the boot loader. GEM1 exists in
the SoC but is not routed to anything on this board: its pins carry USB0 instead.

The LEDs, push buttons and slide switches of the PYNQ-Z1 are wired to the programmable logic, not to
the processing system MIO pins. They are therefore not available to Zephyr unless a matching
bitstream is loaded into the PL, and no ``led0``/``sw0`` aliases are defined for this board.

Programming and Debugging
*************************

The Zynq-7000 series SoC needs to be initialized prior to running a Zephyr application. This can be
achieved in a number of ways (e.g. using the Xilinx First Stage Boot Loader (FSBL), the Xilinx
Vivado generated ``ps_init.tcl`` JTAG script, Das U-Boot Secondary Program Loader (SPL), ...).

The instructions here use the U-Boot SPL. For further details and instructions for using Das U-Boot
with Xilinx Zynq-7000 series SoCs, see the following documentation:

- `Das U-Boot Website`_
- `Using Distro Boot With Xilinx U-Boot`_

Building Das U-Boot
===================

Clone and build Das U-Boot for the PYNQ-Z1. Set ``DEVICE_TREE`` to the PYNQ-Z1 device tree shipped
with your U-Boot version:

.. code-block:: console

   git clone https://source.denx.de/u-boot/u-boot.git
   cd u-boot
   make distclean
   make xilinx_zynq_virt_defconfig
   export PATH=/path/to/zephyr-sdk/arm-zephyr-eabi/bin/:$PATH
   export CROSS_COMPILE=arm-zephyr-eabi-
   export DEVICE_TREE="zynq-pynqz1"
   make

If your U-Boot version does not ship a PYNQ-Z1 device tree, use the Xilinx FSBL generated from the
PYNQ base design instead of the U-Boot SPL to initialize the SoC.

Flashing
========

Here is an example for running the :zephyr:code-sample:`hello_world` application via JTAG.

Ensure the board is configured for JTAG boot (``JP4`` set to ``JTAG``), open a serial terminal on
the USB-UART bridge, turn on/reset the board, and initialize the Zynq-7000 series SoC by uploading
and running the U-Boot SPL via JTAG.

Next, upload and run the Zephyr application:

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: pynq_z1
   :goals: flash

You should see the following message in the terminal:

.. code-block:: console

   *** Booting Zephyr OS vx.xx.x-xxx-gxxxxxxxxxxxx ***
   Hello World! pynq_z1

Another option is to load and run the :zephyr:code-sample:`hello_world` application via U-Boot. Copy
``u-boot/spl/boot.bin``, ``u-boot/u-boot.img``, and ``zephyr/zephyr.bin`` to a FAT32 formatted
microSD card, insert the card in the microSD slot on the PYNQ-Z1 board, ensure the board is
configured for ``SD`` boot (``JP4`` set to ``SD``), and turn on the board.

Once U-Boot is done initializing, load and run the Zephyr application:

.. code-block:: console

   Zynq> fatload mmc 0 0x0 zephyr.bin
   817120 bytes read in 56 ms (13.9 MiB/s)
   Zynq> go 0x0
   ## Starting application at 0x00000000 ...
   *** Booting Zephyr OS vx.xx.x-xxx-gxxxxxxxxxxxx ***
   Hello World! pynq_z1

Debugging
=========

Here is an example for the :zephyr:code-sample:`hello_world` application.

Ensure the board is configured for JTAG boot, open a serial terminal, turn on/reset the board, and
initialize the Zynq-7000 series SoC by uploading and running the U-Boot SPL via JTAG.

Next, upload and debug the Zephyr application:

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: pynq_z1
   :goals: debug

Step through the application in your debugger, and you should see the following message in the
terminal:

.. code-block:: console

   *** Booting Zephyr OS vx.x.x-xxx-gxxxxxxxxxxxx ***
   Hello World! pynq_z1

.. _Digilent PYNQ-Z1:
   https://digilent.com/reference/programmable-logic/pynq-z1/start

.. _Das U-Boot Website:
   https://www.denx.de/wiki/U-Boot

.. _Using Distro Boot With Xilinx U-Boot:
   https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/749142017/Using+Distro+Boot+With+Xilinx+U-Boot
