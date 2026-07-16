# Board Test

`board_test.sh` is an end-to-end test of `tools/bin/psb_demo_cli` itself —
every subcommand exercised against a live board over its Modbus RTU
interface, the same way an operator would use the tool. It is
variant-agnostic: channel count and every per-channel `CH_CAP_*` flag are
discovered at runtime, so the same script covers `jw_hvb`, `jw_lvb`, and any
future variant without modification.

This complements `tools/stress_test/` (Modbus-protocol load/QA testing) and
`tools/dac_sweep_test/` (DAC linearity characterization) rather than
replacing them — this one is specifically about validating the CLI tool's
own command surface and capability-gating logic against real hardware.

## Run

```bash
tools/board_test/board_test.sh --port /dev/ttyUSB0
```

Options:

```text
--port PATH           Serial port (default: /dev/ttyUSB0)
--baud RATE           Baud rate (default: 115200)
--slave ID            Modbus slave ID (default: 1)
--timeout MS          CLI timeout in milliseconds (default: 2000)
--cli PATH            psb_demo_cli executable
--report PATH         Markdown report path
--channel N           Test only channel N (default: all supported channels)
--read-only            Skip every write test; only exercise read/describe commands
--exercise-outputs      Also toggle ENABLE/DISABLE-GRACEFUL on switchable channels
--assert-fresh          Also assert every channel matches documented factory
                        defaults, not just that reads/writes round-trip
                        self-consistently. Only meaningful right after a clean
                        erase+flash — see "Clean bring-up" below.
--expect-disabled LIST  Comma-separated channel numbers whose factory default
                        is CFG_OUTPUT_ENABLED=0 instead of the normal 1 (i.e.
                        channels with a default-output-disabled DTS override,
                        e.g. jw_lvb ch5). Only affects --assert-fresh.
```

Without `--report`, reports are written to `tools/board_test/reports/`.

## Clean bring-up

`west flash` alone only programs the application image — every board
variant's DTS defines a separate NVS `storage_partition` for persisted
config/calibration, deliberately outside that image region so it survives
ordinary firmware updates. That means a plain reflash during bring-up/testing
can leave a channel's config at whatever it was left at during some earlier
test session, indistinguishable from a genuine factory-default boot unless
you know to mass-erase first. `board_test.sh`'s ordinary write checks
wouldn't have caught this either — they're same-value round trips (read
current value, write it back, verify unchanged), which pass regardless of
whether "current value" happens to be the correct default.

```bash
tools/board_test/factory_bringup.sh --build-dir build_psb_lvb --port /dev/ttyUSB0
```

This mass-erases the chip (wiping the NVS partition too), reflashes, and runs
`board_test.sh --assert-fresh` to confirm every channel actually came up at
its Kconfig-documented default — not just that it's internally consistent.
Use this whenever you need to know the board's *true* out-of-box state (e.g.
after changing a Kconfig default), not a plain `west flash`.

`jw_lvb` ch5 has a `default-output-disabled` DTS override (see
`docs/guide/vc-runtime-execution.md` §4, Step 3b), so its true factory
default is `0`, not the normal `1`. Pass that through to `board_test.sh`:

```bash
tools/board_test/factory_bringup.sh --build-dir build_psb_lvb --port /dev/ttyUSB0 -- --expect-disabled 5
```

This is a bring-up/dev-loop tool for one bench unit with direct SWD access,
not a manufacturing-line tool. See
`docs/superpowers/plans/2026-07-16-board-lifecycle-state-management.md` for
the broader board-lifecycle-state roadmap this is the first step of.

## Behavior

**Phase 0 — connectivity.** Reads the system input block via `raw fc04` as
ground truth (protocol version, variant ID, channel count).

**Phase 1 — system-level commands.** `info`, `status`, `system config`,
`list ports`, `list regs`, `describe`, `monitor` (one rendered frame), and a
CLI11 argument-validation check (`channel 99` must be rejected).

**Phase 2 — per-channel, for every supported channel.** `channel <n> info`,
`channel <n> config` (with a regression check that Voltage/Ramp/I-Protection/
Derate fields are only shown when the channel's capability flags actually
support them), `channel <n> cal`, and capability-gated write round trips for
voltage, enable-cfg, ramp-up/down, recovery policy, safe-band, current
protection, derate step, and fault-history clear. Finishes with the
always-on invariant: on a channel without `CH_CAP_OUTPUT_ENABLE`, attempts
`output DISABLE-GRACEFUL` and asserts it is rejected with the live
`OUTPUT_ENABLE_ACTIVE` status bit unchanged.

## Safety model

All per-channel CONFIG-field writes (ramp, recovery, safe-band, prot-i,
derate, voltage target, enable-cfg) are **same-value round trips**: read the
current value, write it straight back, verify it reads back unchanged. This
exercises the full write path without ever changing configured behavior.
`fault CLEAR-HISTORY` is a harmless one-way command and runs unconditionally.

The one command that is genuinely, instantly live is
`channel <n> output ENABLE|DISABLE-GRACEFUL`. On a locked always-on channel
this script always attempts `DISABLE-GRACEFUL` and asserts it is *rejected*
— safe by construction, and the core hardware-safety invariant this test
exists to catch regressions in. On a switchable channel, actually toggling
it off and back on is skipped by default (a real connected load could be
affected) and only runs with `--exercise-outputs`.

## Notes

Each CLI invocation is a fresh process that reopens the serial port from
scratch. On USB-serial adapters (e.g. CH340) back-to-back reopens with no
gap can intermittently fail to re-establish before the first Modbus
transaction; the script adds a short settle delay and retries transient
failures before reporting them.

**Before assuming a bad run means hardware flakiness or a regression, check
for a process already holding the port** (`fuser /dev/ttyUSB0` or
`lsof /dev/ttyUSB0`) — `psb_demo_tui`/`psb_demo_cli monitor` left running in
another terminal will race every `board_test.sh` invocation for the same
serial port, producing exactly the same symptoms as a degraded USB link
(intermittent "Connection error", partial multi-register reads failing more
than single-register ones) but with no hardware cause at all.
