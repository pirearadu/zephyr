# Copyright (c) 2026 Radu Pirea
# SPDX-License-Identifier: Apache-2.0

board_runner_args(openocd "--file-type=elf" "--cmd-reset-halt" "halt")
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
