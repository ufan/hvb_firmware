# Modbus DAC Sweep Test Design

## Goal

Add a standalone host-side test under `tools/dac_sweep_test/` that uses
`tools/bin/hvb_demo_cli` raw Modbus commands to sweep every supported
DAC-capable channel and report output and measurement telemetry.

## Interface

The entry point is `tools/dac_sweep_test/dac_sweep_test.sh`.

Connection options:

- `--port`, default `/dev/ttyUSB0`
- `--baud`, default `115200`
- `--slave`, default `1`
- `--timeout`, default `3000` ms
- `--cli`, default `tools/bin/hvb_demo_cli`
- `--report`, default a timestamped Markdown file under
  `tools/dac_sweep_test/reports/`

The sweep is fixed to DAC codes 0 through 60000 inclusive, in steps of
10000. Each point settles for five seconds. The script does not request
interactive confirmation.

## Discovery and capability gating

The script reads the system input block to validate protocol v3, calibration
support, and `supportedChannels`. It examines each channel's capability flags.

- Channels without `CH_CAP_RAW_OUTPUT_DRIVE` are skipped and reported.
- Raw and measured voltage are recorded only with
  `CH_CAP_VOLTAGE_MEASUREMENT`.
- Raw and measured current are recorded only with
  `CH_CAP_CURRENT_MEASUREMENT`.

Channels are swept sequentially. Only one calibration output may be enabled at
a time.

## Sweep sequence

The script enters Calibration Mode using the v3 unlock sequence and operating
mode register. For each DAC-capable channel it:

1. Enables calibration output.
2. Writes each DAC code.
3. Waits five seconds.
4. Requests a fresh calibration sample when measurement capability exists.
5. Reads channel input offsets 10 through 15 as applicable.
6. Records signed raw ADC values, signed measured values, and scaled physical
   measurements.
7. Writes DAC zero and disables calibration output before moving to the next
   channel.

Voltage uses 0.1 V per measured-value LSB. Current uses 1 nA per
measured-value LSB. Raw ADC voltage/current are signed 32-bit values assembled
from high and low Modbus registers.

## Safety and failure handling

A shell trap runs on normal exit, command failure, SIGINT, and SIGTERM. It
best-effort writes DAC zero and disables calibration output on every discovered
channel, then exits Calibration Mode. Cleanup errors are reported without
hiding the original failure.

The script never commits calibration coefficients or saves configuration to
NVS.

## Report

The Markdown report contains:

- timestamp and connection parameters;
- protocol, firmware, variant, and channel count;
- channel capability flags and skipped-channel reasons;
- one table per swept channel with DAC code, raw ADC V/I, measured raw V/I,
  measured volts, and measured nA;
- per-channel and overall pass/fail status;
- cleanup result.

Unavailable capability-dependent values are shown as `N/A`.

### Raw ADC linearity fit

After each channel sweep, the script performs an ordinary least-squares fit of
each supported raw ADC series against DAC code. Voltage and current axes are
fitted independently. Calibrated measured V/I values are not fitted.

Each fit reports:

- `raw_adc = slope × DAC + intercept`;
- coefficient of determination (R²);
- maximum absolute residual in raw ADC counts;
- maximum absolute residual as a percentage of the fitted full-scale span;
- number of sweep points.

The calculation uses `awk` and adds no non-standard runtime dependency. A
constant raw ADC series has zero response span and reports R² and residual
percentage as `N/A`; its slope, intercept, absolute residual, and point count
are still reported.

## Verification

Tests use a mock CLI executable and no hardware. They cover:

- channel discovery and capability-based skipping;
- seven-point sweep generation;
- signed 16-bit and signed 32-bit decoding;
- physical-unit conversion;
- positive-slope and negative-slope raw ADC fits;
- constant-series fit handling;
- Markdown report content;
- DAC-zero/output-disable/Calibration-Mode-exit cleanup after success and an
  injected failure.

A final hardware smoke test may run against `/dev/ttyUSB0` only when explicitly
requested, because it drives physical high-voltage outputs.
