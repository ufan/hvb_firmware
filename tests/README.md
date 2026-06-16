# Test Documentation

## Test Layers

This project uses two complementary test layers:

| Layer | Framework | Target | Speed | CI |
|---|---|---|---|---|
| **Domain unit tests** | Zephyr ztest | `native_posix` | < 1s | Yes |
| **Modbus integration tests** | mbpoll (shell) | `jw_hvb` board | ~30s | No (requires HW) |
| **CLI integration tests** | bash | `hvbctrl` (host) | ~60s | No (requires HW) |

## Running Tests

### Domain Unit Tests (native_posix, no hardware)

```sh
west build -b native_posix -t run -d build/test-domain tests/voltage_control/domain
```

### Modbus Integration Tests (requires real board)

```sh
cd tests/integration/modbus

# Run all
PORT=/dev/ttyUSB0 ./run_all.sh

# Run individual sections
./smoke.sh
./simulation.sh
./protection.sh
./validation.sh
```

The `PORT` environment variable defaults to `/dev/ttyUSB0`. Override it if your USB-to-RS485 adapter is on a different port:

```sh
PORT=/dev/ttyACM0 ./run_all.sh
```

### CLI Integration Tests (requires real board)

These test the `hvbctrl` CLI tool end-to-end and live alongside the tool's own unit tests:

```sh
cd tools/modbus_debug_tool/tests

# Regression guard: hvbctrl vs mbpoll differential (partial-frame read bug)
PORT=/dev/ttyUSB0 ./cli_crosscheck.sh

# Full CLI suite mirroring run_all.sh (smoke/simulation/protection/validation)
PORT=/dev/ttyUSB0 ./cli_run_all.sh
```

### Adding a New Test

1. **Domain logic**: add a `ZTEST` in `tests/voltage_control/domain/src/main.c`
2. **Modbus integration**: add a new `.sh` script in `tests/integration/modbus/` and add it to `run_all.sh`

## TDD Flow

```
1. Write ztest case → red
2. Implement domain logic → green
3. Write mbpoll script → red (if new Modbus-visible behavior)
4. Implement adapter or wire it up → green
5. Run ./run_all.sh → all green
6. Commit
```

## Test Plan

The integration test scripts under `tests/integration/modbus/` form the executable test plan. See each script's header comment for what is tested and how.
