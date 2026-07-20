# jw_lvb Current Zero-Offset Calibration

`jw_lvb_calibrate.py` automates the jw_lvb-specific current calibration
procedure documented in `docs/guide/calibration-guide.md` §7. **jw_lvb
only** — jw_hvb calibration needs external reference instruments (DMM,
precision load) and a DAC sweep, so it isn't automatable the same way; this
tool refuses to run against anything but a jw_lvb board (checked via the
VARIANT_ID register).

Every channel must be genuinely unloaded (no external load connected) and
enabled (ON) while this runs — the zero-offset is derived from live,
already-calibrated `Measured Current` readings taken under real operating
conditions, not from Calibration Mode's forced-off state. See the module
docstring and calibration-guide.md §7 for the rationale.

## Install

```bash
pip install -r tools/factory/06_self_cal/jw_lvb/requirements.txt
```

## Run

```bash
python3 tools/factory/06_self_cal/jw_lvb/jw_lvb_calibrate.py --port /dev/ttyUSB0
```

Recommended: preview first with `--dry-run` (read-only, no writes).

Options:

```text
--port PATH        Serial port (default: /dev/ttyUSB0)
--baud RATE        Baud rate (default: 115200)
--slave ID         Modbus slave ID (default: 1)
--timeout SECONDS  Modbus timeout (default: 0.5)
--channels LIST     Comma-separated channels to calibrate (default: all reported)
--samples N        Live samples averaged per channel (default: 12)
--interval SECONDS Seconds between samples (default: 0.6)
--dry-run          Compute and print the plan, write nothing
-y, --yes          Skip the confirmation prompt before committing
--force-loaded     Override the real-load safety guard (see below) — only
                    if you're certain a flagged channel is truly unloaded
```

## Safety guard: real-load detection

A channel with a real load draws a stable current (large mean, low noise);
a genuinely unloaded channel scatters noisily around a small mean. Any
channel matching the "stable and large" signature is skipped by default —
calibrating a loaded channel's "offset" against real load current would
silently bake in a permanent under-reporting bias. Remove the load and
rerun, or pass `--force-loaded` only if you're certain the flag is wrong.

## Dependencies

`minimalmodbus` (see `requirements.txt`; also used by
`tools/factory/04_stress_test/stress_test.py`) — the only third-party dependency,
so the script itself is otherwise self-contained (stdlib only).
