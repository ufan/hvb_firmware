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
The script requires `gnuplot` with `pngcairo` support.

## Behavior

- Discovers the board's supported channel count and capability flags.
- Processes every channel sequentially.
- Skips channels without raw DAC output capability.
- Sweeps DAC codes `0, 5000, 10000, 15000, 20000, 25000, 30000, 35000, 40000,
  45000, 50000, 55000, 60000`.
- Waits five seconds after every DAC write.
- Requests a fresh calibration sample before reading supported measurements.
- Records raw ADC voltage/current codes only, each with a decimal and a
  0x-prefixed hex column; DAC codes get a hex column too.
- Reports unsupported measurement fields as `N/A`.

After each channel sweep, the report fits each supported raw ADC axis against
DAC code using ordinary least squares. The fit table includes slope, intercept,
R², maximum absolute residual, residual as a percentage of the fitted response
span, and point count. For a
constant raw ADC series, R² and residual percentage are reported as `N/A`.

One PNG is generated beside the report for every swept channel using the report
stem plus `_ch<channel>.png`. Each supported raw ADC axis gets an independent
panel containing the collected points and its OLS fit line; panel titles show
the equation and R². Constant series render as horizontal lines. DAC-only
channels receive a placeholder image stating that raw ADC measurement is not
available. The Markdown report embeds each image with a relative link.

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
