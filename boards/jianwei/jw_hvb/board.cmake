board_runner_args(jlink "--device=STM32F429BI" "--iface=swd" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
