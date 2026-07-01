# ADS1232 Fresh Synchronous Read Experiment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Require a fresh DRDY transition for synchronous ADS1232 reads and repeat the channel-0 DAC sweep.

**Architecture:** Add a two-phase bounded wait inside `ads1232_read_one()`: wait for inactive/high, then active/low. The asynchronous VC path remains unchanged, allowing the sweep to determine whether this synchronous-only reference behavior affects results.

**Tech Stack:** Zephyr ADC/GPIO APIs, GPIO emulator, ztest, `jw_hvb` hardware shell.

---

### Task 1: Specify fresh synchronous sampling

**Files:**
- Modify: `tests/drivers/ads1232/src/main.c`

- [ ] Add a GPIO-emulator test that starts DRDY low, drives it high then low,
      and asserts no SCLK occurs before the complete transition.
- [ ] Run `west build -b native_sim -d /tmp/build-ads1232-test tests/drivers/ads1232 -p always && west build -d /tmp/build-ads1232-test -t run`.
- [ ] Confirm the new test fails because current synchronous code clocks the
      already-low sample immediately.

### Task 2: Implement bounded high-to-low wait

**Files:**
- Modify: `drivers/sensor/ads1232/ads1232.c`

- [ ] Add a bounded inactive/high wait before the existing active/low wait in
      `ads1232_read_one()`.
- [ ] Return `-ETIMEDOUT` from either phase without issuing SCLK.
- [ ] Re-run the ADS1232 native test and confirm every case passes.

### Task 3: Repeat board sweep

**Files:**
- Create: `/tmp/ads1232_dac_sweep_fresh_sync.csv`

- [ ] Build and flash the channel-0-only `jw_hvb` image.
- [ ] Run `vc cal 0` with output enabled at DAC codes 0, 10000, 20000, 30000,
      40000, 50000, and 60000, waiting one second before each sample.
- [ ] Record raw voltage, raw current, and command latency.
- [ ] In cleanup, set DAC to zero, disable output, exit calibration, and restore
      the standard firmware image.
- [ ] Compare the new CSV with `/tmp/ads1232_dac_sweep.csv`.

