# DAC Sweep Test

`dac_sweep_test.sh` characterizes every DAC-capable channel through the Modbus
interface using `tools/bin/hvb_demo_cli`.

## Run

```bash
tools/dac_sweep_test/dac_sweep_test.sh --port /dev/ttyUSB0
```

Options:

```text
--port PATH       Serial port (default: /dev/ttyUSB0)
--baud RATE       Baud rate (default: 115200)
--slave ID        Modbus slave ID (default: 1)
--timeout MS      CLI timeout in milliseconds (default: 3000)
--cli PATH        hvb_demo_cli executable
--report PATH     Markdown report path
```

Without `--report`, reports are written to `tools/dac_sweep_test/reports/`.

## Behavior

- Discovers the board's supported channel count and capability flags.
- Processes every channel sequentially.
- Skips channels without raw DAC output capability.
- Sweeps DAC codes `0, 10000, 20000, 30000, 40000, 50000, 60000`.
- Waits five seconds after every DAC write.
- Requests a fresh calibration sample before reading supported measurements.
- Records raw ADC voltage/current, measured raw voltage/current, volts, and nA.
- Reports unsupported measurement fields as `N/A`.

The script enters volatile Calibration Mode but does not write calibration
coefficients, commit calibration, or save configuration to NVS.

## Cleanup

On success, failure, SIGINT, or SIGTERM, the script best-effort:

1. Writes DAC zero to every discovered DAC-capable channel.
2. Disables every calibration output.
3. Exits Calibration Mode.

The cleanup and overall results are included in the Markdown report.

## Tests

The regression test uses a mock CLI and does not access hardware:

```bash
bash tools/dac_sweep_test/tests/test_dac_sweep_test.sh
```
