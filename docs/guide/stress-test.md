# Modbus Stress Test — Jianwei Voltage-Control Board

Tools for measuring Modbus communication deadtime, blocking time, and throughput
against real HVB hardware. Two distinct purposes are supported via the same script.

## Purposes

### CI Pipeline Integration Test (`--mode ci`)

Runs in a CI pipeline on every commit. Single channel, minimal register set, fast
execution (~10s), exit-code driven:

- **Exit 0** — all checks passed
- **Exit 1** — one or more failures (blocks merge)

```sh
# CI pipeline invocation
python3 tools/stress_test/stress_test.py --mode ci \
    --port /dev/ttyUSB0 \
    --rounds 200 \
    --burst-duration 5 \
    --report reports/ci_$(git rev-parse --short HEAD).md
```

CI checks:

| Test | Registers | Rounds | Purpose |
|------|-----------|--------|---------|
| Connectivity | PROTOCOL_MAJOR, SUPPORTED_CH | 1 | Board present, protocol OK |
| Single read | SYS_STATUS (addr=13, FC04) | 200 | Modbus read integrity |
| Block read | sys input block (addr=0-14, FC04) | 100 | Multi-register read integrity |
| Config write | CFG_TARGET_VOLTAGE (addr=43, FC06) | 3 values | Write + readback verify |
| Cmd write | OUTPUT_ACTION, FAULT_CMD, PARAM_ACTION | 1 each | WO self-clear behavior |
| Sustained burst | SYS_STATUS | 5 s | Throughput stability, no drift |

### Production QA / Human Technician (`--mode qa`)

Exhaustive test covering all channels present on the board and every register.
Run by a human technician on the production line or during quality assurance.
Generates a detailed register-level pass/fail report for inspection:

```sh
# Production line QA invocation
python3 tools/stress_test/stress_test.py --mode qa \
    --port /dev/ttyUSB0 \
    --burst-duration 15 \
    --report reports/qa_$(date -u +%Y%m%d_%H%M%S).md
```

QA checks cover:

| Category | Scope |
|----------|-------|
| System input (FC04) | All 15 defined registers (addr 0-14), reserved range |
| System holding (FC03/FC06) | All 5 defined offsets, reserved read/write rejection |
| Extension holding | CAL_UNLOCK, CAL_EXIT read/write, reserved rejection |
| Per-channel input (FC04) | Block + individual reads offsets 0-11, cal-only exclusion |
| Per-channel holding (FC03/FC06) | All 23 RW offsets, all 5 WO offsets, self-clear verify |
| Per-channel config write | CFG_TARGET_VOLTAGE, RECOVERY_POLICY, CURRENT_SAFE_BAND cycle |
| Per-channel NVS | PARAM_ACTION Save/Load |
| Sustained burst | 10–30s continuous polling, latency drift check |

Total: ~117 register-level checks per channel. Exit 0 on all-pass, exit 1 otherwise.

## Tool Inventory

| Script | Engine | Connection | Use |
|--------|--------|------------|-----|
| `stress_test.py --mode ci` | `minimalmodbus` (Python) | Persistent | CI pipeline |
| `stress_test.py --mode qa` | `minimalmodbus` (Python) | Persistent | Production QA |
| `stress_test_native.sh` | `hvb_demo_cli` (C++ / libmodbus) | Per-invocation | Functional smoke test |

The Python script uses a persistent connection for accurate latency measurement. The
native CLI wrapper uses the project's own `hvb_demo_cli` binary but adds ~50ms
process-spawn overhead per transaction — use it for smoke/functional validation,
not latency profiling.

**Register source of truth:** all scripts derive their register addresses from
`include/reg_store/reg_map.h` and `include/reg_store/modbus_view.def`.

## Usage

### CI mode

```sh
python3 tools/stress_test/stress_test.py \
    --mode ci \
    --port /dev/ttyUSB0 \
    --baud 115200 \
    --slave 1 \
    --rounds 200 \
    --burst-duration 5 \
    --report reports/ci_report.md
```

| Option | Default | Description |
|--------|---------|-------------|
| `--mode` | `ci` | `ci` or `qa` |
| `--port` | `/dev/ttyUSB0` | Serial port |
| `--baud` | `115200` | Baud rate |
| `--slave` | `1` | Modbus slave ID |
| `--timeout` | `0.5` | Modbus timeout (seconds) |
| `--rounds` | `2000` | Iterations for read tests |
| `--burst-duration` | `30` | Sustained burst duration (seconds) |
| `--report` | `reports/stress_test_ci_<ts>.md` | Output report path |

### QA mode

```sh
python3 tools/stress_test/stress_test.py \
    --mode qa \
    --port /dev/ttyUSB0 \
    --burst-duration 15 \
    --report reports/qa_report.md
```

Same options as CI mode. `--rounds` is ignored in QA mode (checks are exhaustive).

### Native CLI (functional smoke test)

```sh
bash tools/stress_test/stress_test_native.sh \
    --port /dev/ttyUSB0 \
    --rounds 500 \
    --burst 10 \
    --report reports/smoke_$(date -u +%Y%m%d).md
```

| Option | Default | Description |
|--------|---------|-------------|
| `--port` | `/dev/ttyUSB0` | Serial port |
| `--baud` | `115200` | Baud rate |
| `--slave` | `1` | Modbus slave ID |
| `--rounds` | `500` | Iterations for read tests |
| `--burst` | `10` | Sustained burst duration (seconds) |
| `--cli` | `tools/bin/hvb_demo_cli` | Path to CLI binary |
| `--report` | `reports/stress_test_native_<ts>.md` | Output report path |

## Report Output

Both CI and QA modes generate a Markdown report:

- Test metadata (timestamp, port, baud, firmware version, channel count)
- Source-of-truth references (`reg_map.h`, `modbus_view.def`)
- Per-test pass/fail table (CI mode: 6 rows; QA mode: ~117 rows per channel)
- Overall PASS/FAIL verdict

CI report example:

```
# HVB CI Stress Test — PASS

| Test | Result | Detail |
|------|--------|--------|
| connectivity | PASS | v3.0 ch=1 |
| single_read | PASS | 200 reads, avg=6685us, 0 err |
| block_read | PASS | 100 reads, avg=9655us, 0 err |
| config_write | PASS | 0 errors |
| cmd_write | PASS | 0 errors |
| burst | PASS | 149 Hz, 0 errors |

**Overall: PASS**
```

QA report includes every check:

```
# HVB QA Stress Test — PASS

## Summary: 117/117 passed, 0 failed

| Check | Result | Detail |
|-------|--------|--------|
| SYS_IN block read (15 regs) | PASS | 9581us |
|   PROTOCOL_MAJOR == 3 | PASS | |
| ...
| ch0 holding off=3 (addr=43) read | PASS | val=0 6579us |
| ch0 CFG_TARGET_VOLTAGE write 1000 | PASS | |
| Sustained burst | PASS | 149.8 Hz, drift -0.4%, 0 errors |

**Overall: PASS**
```

## Engine Comparison

| Aspect | Python (`stress_test.py`) | Native CLI (`stress_test_native.sh`) |
|--------|---------------------------|--------------------------------------|
| Modbus library | `minimalmodbus` v2.x | `libmodbus` (via ModbusLib) |
| Connection model | Single persistent `serial.Serial` | Open/close per invocation |
| Process overhead | ~0 us | ~50 ms |
| Typical single-reg latency | ~6.7 ms | ~60 ms |
| Max sustained poll rate | ~150 Hz | ~16 Hz |

## Deadtime Reference

Results from a 1-channel HVB board at 115200 baud over USB-serial:

| Operation | Avg blocking | Throughput |
|-----------|-------------|------------|
| Single reg read (FC04) | 6,611 us | 151 Hz |
| Block read 15 reg (sys input) | 9,593 us | 104 Hz |
| Config write (FC06 + verify) | 6,791 us | — |
| Cmd write (PARAM_ACTION, NVS) | 7,005 us | — |
| Sustained burst 15 s | 6,662 us | 150 Hz (drift -0.5%) |

| Recommendation | Value |
|----------------|-------|
| Read deadtime (host) | ≥ 7.1 ms (p99) + margin |
| Poll period | ~9 ms (p99 + 2 ms) |
| Safe max poll rate (1 ch) | ~110 Hz |
| Write timeout | 500 ms |

## Prerequisites

- Python 3.10+ with `minimalmodbus` and `pyserial`
- HVB board connected via USB-serial at `/dev/ttyUSB0`
- Board flashed with `applications/hvb_controller` firmware
- For native CLI: pre-built `hvb_demo_cli` binary in `tools/bin/`
