# Modbus DAC Sweep Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone Bash test that drives a fixed DAC sweep on every supported DAC-capable channel through `hvb_demo_cli` and emits a capability-aware Markdown report.

**Architecture:** `dac_sweep_test.sh` owns CLI parsing, raw Modbus transactions, signed decoding, sequential channel execution, report generation, and trap-based cleanup. A mock CLI provides deterministic register responses and records writes so shell tests verify normal and failed runs without hardware.

**Tech Stack:** Bash 4+, `hvb_demo_cli` raw FC03/FC04/FC06 commands, standard Unix tools.

---

## File structure

- Create `tools/dac_sweep_test/dac_sweep_test.sh`: production sweep runner.
- Create `tools/dac_sweep_test/tests/mock_hvb_demo_cli.sh`: deterministic CLI replacement.
- Create `tools/dac_sweep_test/tests/test_dac_sweep_test.sh`: end-to-end regression tests.
- Create `tools/dac_sweep_test/README.md`: usage, report, and safety behavior.

### Task 1: Mocked sweep contract

**Files:**
- Create: `tools/dac_sweep_test/tests/mock_hvb_demo_cli.sh`
- Create: `tools/dac_sweep_test/tests/test_dac_sweep_test.sh`

- [ ] **Step 1: Write a deterministic mock CLI**

The mock ignores connection options, logs the raw command, and returns protocol
3.0 with calibration capability and three channels: CH0 has DAC/V/I, CH1 has
DAC only, and CH2 has V measurement only. CH0 telemetry is derived from the
most recently logged DAC write. `MOCK_FAIL_DAC` injects a write failure.

- [ ] **Step 2: Write failing end-to-end tests**

Run the production script with a fake `sleep` first in `PATH`. Assert seven DAC
writes per swept channel, signed/scaled CH0 report values, CH1 `N/A` values,
CH2 skip reporting, and cleanup writes. Run again with failure at DAC 30000 and
assert nonzero exit, overall FAIL, DAC zero, output disable, and mode exit.

- [ ] **Step 3: Verify the tests fail before implementation**

Run `bash tools/dac_sweep_test/tests/test_dac_sweep_test.sh`.

Expected: FAIL because `tools/dac_sweep_test/dac_sweep_test.sh` is absent.

### Task 2: Sweep runner

**Files:**
- Create: `tools/dac_sweep_test/dac_sweep_test.sh`

- [ ] **Step 1: Add option parsing and CLI wrappers**

Support `--port`, `--baud`, `--slave`, `--timeout`, `--cli`, and `--report`.
Every transaction invokes the CLI with connection options followed by a raw
FC03, FC04, or FC06 command. Validate the executable and report directory.

- [ ] **Step 2: Add register decoding helpers**

Parse CLI hexadecimal words. Decode INT16 by subtracting `0x10000` when bit 15
is set and INT32 by combining high/low words then subtracting `0x100000000`
when bit 31 is set. Format voltage as `raw * 0.1` V and current as raw nA.

- [ ] **Step 3: Add discovery and Calibration Mode entry**

Read FC04 address 0 count 15. Require protocol major 3, calibration capability
bit `0x0004`, and channel count 1–16. Read capability flags at
`40 + channel*40 + 9`. Enter Calibration Mode with FC06 writes `680=0xCA1B`,
`680=0xA11B`, and `0=2`.

- [ ] **Step 4: Implement sequential seven-point sweeps**

For every channel with raw-drive bit `0x0002`, enable `base+30`, sweep DAC
`0 10000 20000 30000 40000 50000 60000` through `base+31`, sleep five seconds,
trigger `base+32=1` when measurement capability exists, wait 100 ms, and read
the supported subset of input offsets 10–15. Append capability-aware Markdown
rows. Zero `base+31` and disable `base+30` before the next channel.

- [ ] **Step 5: Implement unconditional cleanup**

An EXIT trap best-effort zeros and disables every discovered DAC channel, then
writes `681=1` to exit Calibration Mode. Preserve the original exit status and
append cleanup and overall PASS/FAIL to the report.

- [ ] **Step 6: Run regression tests**

Run `bash tools/dac_sweep_test/tests/test_dac_sweep_test.sh`.

Expected: success and injected-failure scenarios both pass their assertions.

### Task 3: Documentation and final verification

**Files:**
- Create: `tools/dac_sweep_test/README.md`

- [ ] **Step 1: Document usage and output**

Document `tools/dac_sweep_test/dac_sweep_test.sh --port /dev/ttyUSB0`, the fixed
sweep and timing, sequential capability-gated behavior, report path, absence of
NVS writes, and automatic cleanup.

- [ ] **Step 2: Run final verification**

Run:

```bash
bash -n tools/dac_sweep_test/dac_sweep_test.sh
bash -n tools/dac_sweep_test/tests/mock_hvb_demo_cli.sh
bash -n tools/dac_sweep_test/tests/test_dac_sweep_test.sh
bash tools/dac_sweep_test/tests/test_dac_sweep_test.sh
git diff --check -- tools/dac_sweep_test
```

Expected: all commands exit 0.

- [ ] **Step 3: Commit the implementation**

```bash
git add tools/dac_sweep_test docs/superpowers/plans/2026-07-02-modbus-dac-sweep-test.md
git commit -m "feat(tools): add Modbus DAC sweep test"
```

### Task 4: Raw ADC linearity fit

**Files:**
- Modify: `tools/dac_sweep_test/dac_sweep_test.sh`
- Modify: `tools/dac_sweep_test/tests/test_dac_sweep_test.sh`
- Modify: `tools/dac_sweep_test/README.md`

- [ ] **Step 1: Add failing report assertions**

Extend the mock success test to require independent Raw ADC V and Raw ADC I
fits for CH0. Its deterministic data must produce slopes `10.000000` and
`-10.000000`, intercepts `0.000`, R² `1.000000`, zero maximum residual, zero
residual percentage, and seven points. Assert CH1 has no fit section because it
has no measurement capability.

- [ ] **Step 2: Run the test and verify RED**

Run `bash tools/dac_sweep_test/tests/test_dac_sweep_test.sh`.

Expected: FAIL because the report has no `Linearity Fit` section.

- [ ] **Step 3: Collect per-axis point files and calculate OLS fits**

Create temporary per-channel point files containing `DAC raw_adc`. After each
channel sweep, use one `awk` calculation per supported axis to compute slope,
intercept, R², maximum absolute residual, fitted response span, residual percent,
and point count. For a zero response span, print `N/A` for R² and residual
percent without dividing by zero.

- [ ] **Step 4: Append the fit table to each channel report**

Add a `### Linearity Fit` table with columns Axis, Equation, R², Max residual,
Max residual (% span), and Points. Include only supported raw ADC axes.

- [ ] **Step 5: Extend documentation**

Document that fits use raw ADC V/I versus DAC code, ordinary least squares, and
report slope/intercept/R²/residual metrics. State constant-series behavior.

- [ ] **Step 6: Run final verification**

Run syntax checks, mock regression tests, ShellCheck, and `git diff --check`.
Expected: all commands exit 0.

- [ ] **Step 7: Commit the fit enhancement**

```bash
git add tools/dac_sweep_test docs/superpowers/plans/2026-07-02-modbus-dac-sweep-test.md
git commit -m "feat(tools): report DAC sweep linearity fits"
```

### Task 5: Per-channel PNG plots

**Files:**
- Modify: `tools/dac_sweep_test/dac_sweep_test.sh`
- Modify: `tools/dac_sweep_test/tests/test_dac_sweep_test.sh`
- Modify: `tools/dac_sweep_test/README.md`

- [ ] **Step 1: Add failing PNG assertions**

Extend the mock success test to require `<report-stem>_ch0.png` and
`<report-stem>_ch1.png`, verify each starts with the PNG signature, and require
relative Markdown image links. CH0 has two plotted panels; CH1 produces a
placeholder image explaining that it has no raw ADC measurement capability.

- [ ] **Step 2: Run the regression test and verify RED**

Run `bash tools/dac_sweep_test/tests/test_dac_sweep_test.sh`.

Expected: FAIL because PNG files are absent.

- [ ] **Step 3: Add plotting preflight and fit-value reuse**

Require `gnuplot` before entering Calibration Mode. Refactor the OLS calculation
so slope, intercept, and R² used in the Markdown table are also available to the
plot generator without fitting the data a second way.

- [ ] **Step 4: Generate one PNG per channel after safe-off**

Write a temporary gnuplot program using `pngcairo`. For each supported raw ADC
axis, plot the point file and `slope*x+intercept` on its own panel with equation
and R² in the title. Use independent Y axes. For a DAC-only channel, render a
single placeholder panel. Generate plots only after DAC zero and output disable.

- [ ] **Step 5: Embed relative image links**

Append `![CHn DAC sweep plot](<report-stem>_chn.png)` to each channel section.
Treat a gnuplot error or absent/empty output file as a test failure so the EXIT
cleanup path executes.

- [ ] **Step 6: Extend documentation**

Document the `gnuplot` dependency, per-channel filenames, panel behavior, fit
line, point markers, constant-series handling, and report embedding.

- [ ] **Step 7: Run final verification**

Run syntax checks, mock regression tests, ShellCheck, PNG signature checks, and
`git diff --check`. Expected: all commands exit 0.

- [ ] **Step 8: Commit the plotting enhancement**

```bash
git add tools/dac_sweep_test docs/superpowers/plans/2026-07-02-modbus-dac-sweep-test.md
git commit -m "feat(tools): plot DAC sweep linearity"
```
