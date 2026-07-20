# Flashing and Debug Guide

How to get firmware onto `jw_hvb`/`jw_lvb` boards and attach a debugger, via
either of the two supported SWD probes:

| Runner | Probe | Default? | Notes |
|---|---|---|---|
| `jlink` | SEGGER J-Link (any model) | Yes | Also required for `jw_lvb`'s RTT shell/log console — see [Known limitation](#known-limitation-no-rtt-console-over-cmsis-dap) |
| `openocd` | Any CMSIS-DAP adapter (e.g. Raspberry Pi Debug Probe) | No, opt-in via `-r openocd` | Flash + gdb debug only; no RTT console |

Both runners are wired up per-board in `boards/jianwei/jw_hvb/board.cmake` and
`boards/jianwei/jw_lvb/board.cmake`. Nothing below changes which one runs by
default — plain `west flash` / `west debug` still use J-Link.

## J-Link (default)

No extra setup beyond a working SEGGER J-Link install and USB access to the
probe (SEGGER's installer sets up its own udev rules, `/etc/udev/rules.d/99-jlink.rules`).

```bash
west flash -d <build-dir>              # -r jlink is the default, can be omitted
west debug -d <build-dir>
```

`board.cmake` passes `--device=STM32F429BI` (jw_hvb) / `--device=STM32F103ZE`
(jw_lvb) and `--speed=4000` automatically.

## CMSIS-DAP / OpenOCD (alternative)

Use this when a J-Link isn't available on the bench. Verified against real
`jw_lvb` hardware using a Raspberry Pi Debug Probe; any CMSIS-DAP-compliant
adapter should work the same way.

### Host setup

The probe needs to be accessible without root. On most Linux distros, the
`openocd` package already installs a udev rule (commonly
`/etc/udev/rules.d/60-openocd.rules`) with a generic
`ATTRS{product}=="*CMSIS-DAP*"` match that covers CMSIS-DAP probes, tagging
the device `uaccess` and adding it to the `plugdev` group. Combined with your
user being in `plugdev`, that's normally all that's needed — no
project-specific udev rule required.

Verify it's actually applied after plugging the probe in:

```bash
# Find the probe's bus/device numbers
lsusb | grep -i "cmsis-dap\|2e8a"
# e.g. "Bus 003 Device 020: ID 2e8a:000c Raspberry Pi Debug Probe (CMSIS-DAP)"

# Confirm uaccess is tagged on it (look for "uaccess" in TAGS/CURRENT_TAGS)
udevadm info -q property -p /sys/bus/usb/devices/<bus>-<port> | grep -E "TAGS|ID_VENDOR|ID_MODEL"
```

If `uaccess`/`plugdev` access isn't present, install your `openocd`
package's udev rules (or reinstall/repair the package) and re-plug the
probe — don't hand-roll a new rule; the generic CMSIS-DAP one already covers
this device.

**A CMSIS-DAP probe enumerates as more than one thing.** For example, the
Raspberry Pi Debug Probe presents three USB interfaces: a CMSIS-DAP v2 bulk
interface (what OpenOCD uses for SWD), a CDC-ACM UART bridge (its own
`/dev/ttyACM*`, unrelated to SWD), and a vendor interface. **Don't assume the
first `/dev/ttyACM*` you see is the probe** — on a bench with multiple USB
serial adapters this is easy to get wrong. To find the probe's own UART
node specifically:

```bash
udevadm info -q property -p /sys/bus/usb/devices/<bus>-<port>:1.1 | grep DEVNAME
```

And to find out which `/dev/ttyACM*`/`/dev/ttyUSB*` port a given *board*'s
Modbus interface is actually on (as opposed to some other board sharing the
bench), don't guess from port numbering — query each candidate port and
compare the reported variant ID against the board's Kconfig default
(`VC_VARIANT_ID` in `boards/jianwei/<board>/Kconfig.defconfig`: `1` for
jw_hvb, `2` for jw_lvb):

```bash
tools/board_test/board_test.sh --port /dev/ttyACM0 --read-only | grep variant
```

### Flashing

```bash
west flash -d <build-dir> -r openocd
```

Uses `boards/jianwei/<board>/support/openocd.cfg`, which sources
`interface/cmsis-dap.cfg` and the matching target config
(`target/stm32f4x.cfg` for jw_hvb, `target/stm32f1x.cfg` for jw_lvb) at
4000 kHz SWD. `factory_bringup.sh` also accepts this runner:

```bash
tools/board_test/factory_bringup.sh --runner openocd --build-dir <build-dir> --port /dev/ttyACM0
```

### Debugging

```bash
west debug -d <build-dir> -r openocd
```

This starts OpenOCD as a gdbserver and attaches `arm-zephyr-eabi-gdb` from
the Zephyr SDK, same as the J-Link flow. For scripting or CI use, the two
halves can be driven separately — start OpenOCD as a standalone gdbserver:

```bash
openocd -f boards/jianwei/jw_lvb/support/openocd.cfg -c "gdb_port 3333; telnet_port disabled; tcl_port disabled"
```

then drive it non-interactively with gdb, e.g. to halt at `main`, inspect
state, and resume:

```bash
arm-zephyr-eabi-gdb -batch \
  -ex "target extended-remote :3333" \
  -ex "monitor reset halt" \
  -ex "break main" \
  -ex "continue" \
  -ex "bt" \
  -ex "monitor resume" \
  -ex "detach" \
  <build-dir>/zephyr/zephyr.elf
```

For a quick one-shot connectivity/identity check without gdb at all:

```bash
openocd -f boards/jianwei/jw_lvb/support/openocd.cfg -c "init; reset halt; exit"
```

### Known limitation: no RTT console over CMSIS-DAP

`jw_lvb` has no UART shell/log console — it routes both over Segger RTT
(`applications/psb_controller/boards/jw_lvb.conf`:
`CONFIG_UART_CONSOLE=n`, `CONFIG_USE_SEGGER_RTT=y`,
`CONFIG_RTT_CONSOLE=y`, `CONFIG_SHELL_BACKEND_RTT=y`). SEGGER's RTT
tooling (`JLinkRTTViewer`/`JLinkRTTLogger`) only talks to J-Link probes, so
a CMSIS-DAP probe on `jw_lvb` gives you flashing and gdb debugging, but not
shell/log access — use J-Link for that, or fall back to Modbus-level
inspection via `tools/board_test/board_test.sh` / `psb_demo_cli`.
`jw_hvb` isn't affected by this — nothing here changes its console.

## Troubleshooting

**`Error: unable to find CMSIS-DAP device`** — probe not detected. Check
`lsusb` shows it, and re-check the udev/`uaccess` verification above.

**`Error: init mode failed (unable to connect to the target)`** — check SWD
wiring (SWDIO/SWCLK/GND at minimum) and that nothing else (another OpenOCD
instance, a stuck gdb session) is already holding the probe open.

**Flash/debug seems to hit the wrong board** — see the port-identification
technique above; on a bench with multiple boards, `/dev/ttyACM*` numbering
is not a reliable way to tell them apart.

## Related docs

- [`test-tools.md`](test-tools.md) — `board_test.sh`/`factory_bringup.sh`, used above for board/port identification and post-flash verification
- [`build-tools.md`](build-tools.md) — building the host-side tools referenced here (`psb_demo_cli`, etc.)
