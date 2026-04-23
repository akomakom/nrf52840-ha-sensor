# SPDX-License-Identifier: Apache-2.0
# nice!nano — Adafruit UF2 bootloader, no JLink required

board_runner_args(uf2 "--board-id=NICENANO")
include(${ZEPHYR_BASE}/boards/common/uf2.board.cmake)
