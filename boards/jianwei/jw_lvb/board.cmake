board_runner_args(jlink "--device=STM32F103ZE" "--iface=swd" "--speed=4000")

# jlink is the default flash/debug runner; openocd (CMSIS-DAP probes, e.g.
# Raspberry Pi Debug Probe) is available as an alternative via -r openocd.
# Note: this board's shell/log console runs over Segger RTT (see
# applications/psb_controller/boards/jw_lvb.conf), which is J-Link-only —
# the openocd runner here covers flash/debug (gdb), not RTT console access.
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
