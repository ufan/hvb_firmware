#!/usr/bin/env python3
"""
jw_lvb current zero-offset calibration.

Automates docs/guide/calibration-guide.md §7 for the jw_lvb board only.
jw_hvb calibration needs external reference instruments (DMM, precision
load) and a DAC sweep — not automatable the same way, so this tool refuses
to run against anything but a jw_lvb board (checked via System Input
VARIANT_ID, offset 2).

Why this measures with the channel ON, not via Calibration Mode:
Calibration Mode force-disables every channel's output board-wide (existing
firmware behavior — jw_lvb has no DAC, so there's no way to re-energize a
channel while inside Calibration Mode). An earlier version of this
procedure treated that forced-off state as the zero-current reference
point. Measurement on real hardware showed that's wrong: the ACS712's true
zero-current bias measurably differs between the relay open (forced off)
and relay closed (real operating condition — self-heating, EMI, live 12V
rail) states. So this tool measures the live, already-calibrated Measured
Current register while each channel is genuinely enabled and unloaded, and
only enters Calibration Mode afterward, briefly, to persist the correction.

Source of truth for register addresses: docs/guide/modbus-reference.md.
"""

import argparse
import statistics
import sys
import time

import minimalmodbus

# ---------------------------------------------------------------------------
# Register map (absolute Modbus addresses, 0-based). Same conventions as
# tools/factory/04_stress_test/stress_test.py.
# ---------------------------------------------------------------------------
SYS_VARIANT_ID = 2
SYS_SUPPORTED_CH = 4
SYS_OPERATING_MODE = 0  # holding

CH_BASE = lambda c: 40 + c * 40
CH_STATUS = lambda c: CH_BASE(c) + 0        # input
CH_CAPABILITIES = lambda c: CH_BASE(c) + 9  # input
CH_MEASURED_CURRENT = lambda c: CH_BASE(c) + 11  # input
CH_CAL_I_B = lambda c: CH_BASE(c) + 25      # holding
CH_CAL_COMMIT_CMD = lambda c: CH_BASE(c) + 33  # holding

EXT_CAL_UNLOCK = 680
EXT_CAL_EXIT = 681

JW_LVB_VARIANT_ID = 2

STATUS_OUTPUT_ENABLED = 0x0002
STATUS_MEASUREMENT_STALE = 0x0040

CH_CAP_CURRENT_MEASUREMENT = 0x0008

CAL_UNLOCK_STEP1 = 0xCA1B
CAL_UNLOCK_STEP2 = 0xA11B
OPERATING_MODE_CALIBRATION = 2


RETRIES = 4
RETRY_BACKOFF_S = 0.1


def connect(args):
    instr = minimalmodbus.Instrument(args.port, args.slave)
    instr.serial.baudrate = args.baud
    instr.serial.timeout = args.timeout
    instr.mode = minimalmodbus.MODE_RTU
    instr.clear_buffers_before_each_transaction = True
    instr.close_port_after_each_call = False
    return instr


def _with_retries(fn):
    last_exc = None
    for attempt in range(RETRIES):
        try:
            return fn()
        except (minimalmodbus.ModbusException, OSError) as exc:
            last_exc = exc
            time.sleep(RETRY_BACKOFF_S)
    raise last_exc


def read_input(instr, addr, signed=False):
    return _with_retries(lambda: instr.read_register(addr, 0, functioncode=4, signed=signed))


def read_holding(instr, addr, signed=False):
    return _with_retries(lambda: instr.read_register(addr, 0, functioncode=3, signed=signed))


def write_holding(instr, addr, value, signed=False):
    _with_retries(lambda: instr.write_register(
        addr, value, number_of_decimals=0, functioncode=6, signed=signed))


def check_board_is_jw_lvb(instr):
    variant = read_input(instr, SYS_VARIANT_ID)
    if variant != JW_LVB_VARIANT_ID:
        print(f"ERROR: connected board reports VARIANT_ID={variant}, "
              f"not {JW_LVB_VARIANT_ID} (jw_lvb). This tool only supports "
              f"jw_lvb — jw_hvb calibration needs external reference "
              f"instruments and isn't automatable the same way. Aborting.")
        sys.exit(1)


def measure_channel(instr, ch, samples, interval):
    """Average `samples` live Measured Current readings for one channel,
    discarding any sample taken while the measurement-stale bit is set."""
    values = []
    for i in range(samples):
        status = read_input(instr, CH_STATUS(ch))
        if status is not None and not (status & STATUS_MEASUREMENT_STALE):
            imeas = read_input(instr, CH_MEASURED_CURRENT(ch), signed=True)
            values.append(imeas)
        if i < samples - 1:
            time.sleep(interval)
    return values


# A channel with a real load draws a stable current: large mean, small
# noise relative to that mean. Zero-load channels are the opposite — mean
# near zero, noise comparable to or larger than the mean. Caught a real
# case: a loaded channel read a rock-steady ~300mA (stdev ~15mA) while
# every genuinely unloaded channel scattered noisily around 0. Calibrating
# a loaded channel's "offset" against real load current silently bakes in
# a permanent under-reporting bias, so this is a hard stop, not just a
# warning — override with --force-loaded only if you're certain the
# reading reflects noise, not a load.
LOAD_SUSPECT_ABS_MEAN_MA = 150
LOAD_SUSPECT_MAX_REL_NOISE = 0.2


def looks_like_real_load(mean, stdev):
    if abs(mean) < LOAD_SUSPECT_ABS_MEAN_MA:
        return False
    rel_noise = (stdev / abs(mean)) if mean != 0 else 0.0
    return rel_noise < LOAD_SUSPECT_MAX_REL_NOISE


def plan_channel(instr, ch, samples, interval, force_loaded):
    caps = read_input(instr, CH_CAPABILITIES(ch))
    if caps is None or not (caps & CH_CAP_CURRENT_MEASUREMENT):
        return {"ch": ch, "skip": "no CURRENT_MEASUREMENT capability"}

    status = read_input(instr, CH_STATUS(ch))
    if status is None or not (status & STATUS_OUTPUT_ENABLED):
        return {"ch": ch, "skip": "channel not enabled (must be ON to "
                "calibrate under real operating condition)"}

    values = measure_channel(instr, ch, samples, interval)
    if not values:
        return {"ch": ch, "skip": "no valid (non-stale) samples collected"}

    mean = statistics.mean(values)
    stdev = statistics.stdev(values) if len(values) > 1 else 0.0

    if looks_like_real_load(mean, stdev) and not force_loaded:
        return {"ch": ch, "skip": f"looks like a real load, not no-load "
                f"noise (mean={mean:.1f}mA, stdev={stdev:.1f}) — remove "
                f"the load or pass --force-loaded if this is wrong"}

    b_old = read_holding(instr, CH_CAL_I_B(ch), signed=True)
    b_new = round(b_old - mean)

    if not (-32768 <= b_new <= 32767):
        return {"ch": ch, "skip": f"computed B={b_new} out of int16 range"}

    return {
        "ch": ch, "skip": None, "n": len(values), "mean": mean,
        "stdev": stdev, "b_old": b_old, "b_new": b_new,
    }


def print_plan(plans):
    print(f"\n{'Ch':>3} {'n':>3} {'Imeas avg':>12} {'stdev':>8} "
          f"{'B old':>8} {'B new':>8}  Notes")
    for p in plans:
        if p["skip"]:
            print(f"{p['ch']:>3} {'-':>3} {'-':>12} {'-':>8} "
                  f"{'-':>8} {'-':>8}  SKIP: {p['skip']}")
        else:
            print(f"{p['ch']:>3} {p['n']:>3} {p['mean']:>9.1f} mA "
                  f"{p['stdev']:>7.1f} {p['b_old']:>8} {p['b_new']:>8}")


def commit_plans(instr, plans):
    print("\n--- Unlocking Calibration Mode ---")
    write_holding(instr, EXT_CAL_UNLOCK, CAL_UNLOCK_STEP1)
    write_holding(instr, EXT_CAL_UNLOCK, CAL_UNLOCK_STEP2)
    write_holding(instr, SYS_OPERATING_MODE, OPERATING_MODE_CALIBRATION)

    mode = read_input(instr, 12)
    if mode != OPERATING_MODE_CALIBRATION:
        print(f"ERROR: expected Calibration mode, board reports {mode}. Aborting.")
        sys.exit(1)
    print("Calibration Mode confirmed active.")

    for p in plans:
        if p["skip"]:
            continue
        ch = p["ch"]
        write_holding(instr, CH_CAL_I_B(ch), p["b_new"], signed=True)
        readback = read_holding(instr, CH_CAL_I_B(ch), signed=True)
        write_holding(instr, CH_CAL_COMMIT_CMD(ch), 1)
        status = "OK" if readback == p["b_new"] else f"MISMATCH (got {readback})"
        print(f"  CH{ch}: B committed = {readback}  [{status}]")

    print("\n--- Exiting Calibration Mode ---")
    write_holding(instr, EXT_CAL_EXIT, 1)


def verify_after(instr, plans):
    print("\n--- Post-calibration verification ---")
    mode = read_input(instr, 12)
    print(f"Operating mode restored to: {mode}")
    for p in plans:
        ch = p["ch"]
        status = read_input(instr, CH_STATUS(ch))
        was_on = not p["skip"]
        now_on = bool(status is not None and (status & STATUS_OUTPUT_ENABLED))
        flag = "" if (not was_on) or now_on else "  <-- NOT RE-ENABLED, CHECK BOARD"
        note = "(skipped)" if p["skip"] else ("ON" if now_on else "off")
        print(f"  CH{ch}: status=0x{status:04x} {note}{flag}")


def main():
    parser = argparse.ArgumentParser(
        description="jw_lvb current zero-offset calibration (live, "
                    "channel-on measurement + brief Calibration Mode commit)")
    parser.add_argument("--port", default="/dev/ttyUSB0", help="Serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--slave", type=int, default=1, help="Slave ID")
    parser.add_argument("--timeout", type=float, default=0.5, help="Modbus timeout (s)")
    parser.add_argument("--channels", default=None,
                         help="Comma-separated channel list to calibrate "
                              "(default: all channels the board reports)")
    parser.add_argument("--samples", type=int, default=12,
                         help="Live Measured Current samples averaged per channel")
    parser.add_argument("--interval", type=float, default=0.6,
                         help="Seconds between samples (must exceed the "
                              "board's oversampled report period, ~0.5s)")
    parser.add_argument("--dry-run", action="store_true",
                         help="Compute and print the plan, write nothing")
    parser.add_argument("-y", "--yes", action="store_true",
                         help="Skip the confirmation prompt before committing")
    parser.add_argument("--force-loaded", action="store_true",
                         help="Calibrate channels even if their reading looks "
                              "like a real load rather than no-load noise "
                              "(see looks_like_real_load) — only use this if "
                              "you're certain the channel is truly unloaded")
    args = parser.parse_args()

    instr = connect(args)
    check_board_is_jw_lvb(instr)

    if args.channels:
        channels = [int(c) for c in args.channels.split(",")]
    else:
        n = read_input(instr, SYS_SUPPORTED_CH) or 0
        channels = list(range(n))

    print(f"jw_lvb zero-offset calibration — {args.port}, channels {channels}")
    print(f"Measuring {args.samples} live samples/channel, "
          f"{args.interval}s apart (channels must stay ON throughout)...")

    plans = [plan_channel(instr, ch, args.samples, args.interval, args.force_loaded)
             for ch in channels]
    print_plan(plans)

    n_calibrating = sum(1 for p in plans if not p["skip"])
    if n_calibrating == 0:
        print("\nNo channels eligible to calibrate. Exiting.")
        instr.serial.close()
        sys.exit(1)

    if args.dry_run:
        print(f"\nDry run — {n_calibrating} channel(s) would be committed. "
              "No changes made.")
        instr.serial.close()
        return

    if not args.yes:
        resp = input(f"\nCommit new B for {n_calibrating} channel(s) to NVS? [y/N] ")
        if resp.strip().lower() != "y":
            print("Aborted, no changes made.")
            instr.serial.close()
            sys.exit(1)

    commit_plans(instr, plans)
    verify_after(instr, plans)

    instr.serial.close()
    print("\nDone.")


if __name__ == "__main__":
    main()
