# ADS1232 Fresh Synchronous Read Experiment

## Goal

Test whether matching the reference drivers' DRDY high-to-low wait changes
ADS1232 measurements during the existing channel-0 DAC sweep.

## Scope

- Change only the ADS1232 synchronous read path.
- Before reading 24 data bits, observe DRDY inactive (physical high), then wait
  for DRDY active (physical low).
- Keep the asynchronous `adc_read_async()` path and `vc_channel_api` unchanged.
- Preserve the 25-clock framing and asynchronous timeout fixes.
- Test only board channel 0. Sweep DAC codes 0 through 60000 in steps of 10000,
  waiting one second at each point and recording raw voltage/current ADC codes.
- Always restore DAC zero and output disabled after the sweep.

## Verification

An emulated GPIO test must prove that a synchronous read does not consume an
already-low stale sample. Existing ADS1232 tests must remain green. The board
experiment must report the new sweep beside the previous sweep; the experiment
is useful even if the values do not change.

