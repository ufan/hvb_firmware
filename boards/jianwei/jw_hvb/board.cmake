board_runner_args(jlink "--device=STM32F429BI" "--iface=swd" "--speed=4000")

# jlink is the default flash/debug runner; openocd (CMSIS-DAP probes, e.g.
# Raspberry Pi Debug Probe) is available as an alternative via -r openocd.
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
