#!/usr/bin/env python3
"""
jw_hvb self-calibration — NO EXTERNAL INSTRUMENTS.

*** THIS IS A FALLBACK, NOT THE OFFICIAL CALIBRATION PROCEDURE. ***
Official calibration always uses a real DMM/reference load against the
raw ADC codes, per docs/guide/calibration-guide.md §5-6. Use THIS tool
only when professional instruments genuinely aren't available.

*** THIS COMMANDS REAL HIGH VOLTAGE (up to ~90% of the board's rated
max, ~1800V at default settings) ON THE CHANNEL'S HV OUTPUT TERMINALS.
Ensure the output is safely terminated/isolated and no one can contact
it before running this. ***

What it does, per channel:
  1. Reads the channel's CURRENT output calibration (k/k_exp/b) — left
     completely untouched — and uses it to convert each test voltage
     into the raw DAC code that calibration would already produce for
     a normal Vset command of that voltage. This is what makes the
     calibration "self-referenced": there's no DMM, so the DAC's own
     existing (assumed-accurate, factory or previously-calibrated)
     voltage mapping stands in for ground truth.
  2. Steps through 5 voltages (default 10/30/50/70/90% of the board's
     rated max) via Calibration Mode's cal-enable/cal-dac/cal-sample
     path (the same mechanism the manual procedure uses), sampling
     raw ADC voltage and current at each. More samples at the lower
     (safer) voltages, fewer at the higher ones — deliberately
     minimizes total time spent at the top of the range.
  3. Voltage axis: least-squares fit of (raw_adc, target_voltage)
     across all 5 points, keeping k_exp unless the fit falls outside
     uint16 range (same accommodation as the manual worked example in
     calibration-guide.md §6). Reports the fit's residual — near-zero
     means the measurement path is genuinely linear across the range;
     a large one means something is off that a 2-point fit could never
     have revealed.
  4. Current axis: k/k_exp are NOT touched (left at their
     circuit-derived factory default, per design intent — this board's
     current sensing is a physical shunt + precision ADC, not something
     that needs a field gain correction). Only the offset b is derived,
     from the lowest (safest, most-sampled) voltage point. Also reports
     the per-point implied offset across all 5 points — HV leakage
     current can genuinely scale with voltage, which a single constant
     b can never fully correct for regardless of which point you pick,
     so seeing that spread explicitly matters more here than for a
     low-voltage board.

Source of truth for register addresses: docs/guide/modbus-reference.md.
"""

import argparse
import statistics
import sys
import time

import minimalmodbus

# ---------------------------------------------------------------------------
# Register map (absolute Modbus addresses, 0-based).
# ---------------------------------------------------------------------------
SYS_VARIANT_ID = 2
SYS_SUPPORTED_CH = 4
SYS_CURRENT_UNIT_EXP = 15
SYS_OPERATING_MODE = 0  # holding

CH_BASE = lambda c: 40 + c * 40
CH_STATUS = lambda c: CH_BASE(c) + 0             # input
CH_CAPABILITIES = lambda c: CH_BASE(c) + 9       # input
CH_RAW_ADC_VOLTAGE_HI = lambda c: CH_BASE(c) + 12  # input, INT32 (hi,lo)
CH_RAW_ADC_CURRENT_HI = lambda c: CH_BASE(c) + 14  # input, INT32 (hi,lo)

CH_OUTPUT_CAL_K = lambda c: CH_BASE(c) + 20      # holding
CH_OUTPUT_CAL_B = lambda c: CH_BASE(c) + 21      # holding
CH_MEASURED_V_CAL_K = lambda c: CH_BASE(c) + 22  # holding
CH_MEASURED_V_CAL_B = lambda c: CH_BASE(c) + 23  # holding
CH_MEASURED_I_CAL_K = lambda c: CH_BASE(c) + 24  # holding
CH_MEASURED_I_CAL_B = lambda c: CH_BASE(c) + 25  # holding
CH_OUTPUT_CAL_K_EXP = lambda c: CH_BASE(c) + 26  # holding
CH_MEASURED_V_CAL_K_EXP = lambda c: CH_BASE(c) + 27  # holding
CH_MEASURED_I_CAL_K_EXP = lambda c: CH_BASE(c) + 28  # holding

CH_CAL_OUTPUT_ENABLE = lambda c: CH_BASE(c) + 30  # holding
CH_CAL_DAC_CODE = lambda c: CH_BASE(c) + 31       # holding
CH_CAL_SAMPLE_CMD = lambda c: CH_BASE(c) + 32     # holding
CH_CAL_COMMIT_CMD = lambda c: CH_BASE(c) + 33     # holding

EXT_CAL_UNLOCK = 680
EXT_CAL_EXIT = 681

JW_HVB_VARIANT_ID = 1

STATUS_MEASUREMENT_STALE = 0x0040

CH_CAP_RAW_OUTPUT_DRIVE = 0x0002
CH_CAP_VOLTAGE_MEASUREMENT = 0x0004
CH_CAP_CURRENT_MEASUREMENT = 0x0008

CAL_UNLOCK_STEP1 = 0xCA1B
CAL_UNLOCK_STEP2 = 0xA11B
OPERATING_MODE_CALIBRATION = 2

# Board's rated max output voltage (raw ×0.1V units) — not a Modbus register
# (VC_MAX_TARGET_VOLTAGE is compile-time only), so this is the confirmed
# hardware spec (ref/jw_hvb/board_design.md: Vadj 0-5V -> 0-2000V).
JW_HVB_MAX_TARGET_VOLTAGE_RAW = 20000  # 2000.0 V

# Test points as a fraction of max, and how many samples to average at each —
# deliberately front-loaded: more dwell time/statistics at the low (safer)
# end, fewer quick samples at the high end.
DEFAULT_FRACTIONS = [0.10, 0.30, 0.50, 0.70, 0.90]
DEFAULT_SAMPLES_PER_POINT = [8, 5, 4, 3, 2]

# A "looks like a real load/detector" guard, mirroring tools/factory/06_self_cal/jw_lvb's
# real-load guard but checking every test point (not just one) — see
# calibrate_channel(). Default fraction of the int16 register range above
# which a current reading is "too large to be leakage."
DEFAULT_LOAD_SUSPECT_FRACTION = 0.05

# Default max acceptable voltage-fit residual (volts) before refusing to
# commit the fit — a 2-point fit always passes through both points exactly
# and so can never reveal this; with 5+ points, a residual this large means
# the 5 points don't actually lie on one line, so the fit isn't trustworthy
# regardless of how it was computed.
DEFAULT_MAX_RESIDUAL_V = 20.0

RETRIES = 3
RETRY_BACKOFF_S = 0.3


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
    for _ in range(RETRIES):
        try:
            return fn()
        except (minimalmodbus.ModbusException, OSError) as exc:
            last_exc = exc
            time.sleep(RETRY_BACKOFF_S)
    raise last_exc


def read_input(instr, addr, signed=False):
    return _with_retries(lambda: instr.read_register(addr, 0, functioncode=4, signed=signed))


def read_input32(instr, addr_hi):
    hi, lo = _with_retries(lambda: instr.read_registers(addr_hi, 2, functioncode=4))
    val = (hi << 16) | lo
    return val - 0x100000000 if val >= 0x80000000 else val


def read_holding(instr, addr, signed=False):
    return _with_retries(lambda: instr.read_register(addr, 0, functioncode=3, signed=signed))


def write_holding(instr, addr, value, signed=False):
    _with_retries(lambda: instr.write_register(
        addr, value, number_of_decimals=0, functioncode=6, signed=signed))


def check_board_is_jw_hvb(instr):
    variant = read_input(instr, SYS_VARIANT_ID)
    if variant != JW_HVB_VARIANT_ID:
        print(f"ERROR: connected board reports VARIANT_ID={variant}, "
              f"not {JW_HVB_VARIANT_ID} (jw_hvb). This tool is jw_hvb-only. "
              f"Aborting.")
        sys.exit(1)


def apply_gain(raw, k, k_exp):
    return raw * k * (10.0 ** k_exp)


def dac_code_for_voltage_raw(target_v_raw, out_k, out_k_exp, out_b):
    code = round(apply_gain(target_v_raw, out_k, out_k_exp) + out_b)
    return max(0, min(65535, code))


def least_squares(xs, ys):
    n = len(xs)
    sum_x = sum(xs)
    sum_y = sum(ys)
    sum_xy = sum(x * y for x, y in zip(xs, ys))
    sum_xx = sum(x * x for x in xs)
    denom = n * sum_xx - sum_x * sum_x
    if denom == 0:
        raise ValueError("degenerate fit (all raw ADC samples identical)")
    slope = (n * sum_xy - sum_x * sum_y) / denom
    intercept = (sum_y - slope * sum_x) / n
    residuals = [y - (slope * x + intercept) for x, y in zip(xs, ys)]
    return slope, intercept, max(abs(r) for r in residuals)


def k_kexp_from_gain(gain, k_exp_hint):
    """Pick k (uint16) + k_exp (int16, -9..4) reproducing `gain` as closely as
    possible, preferring to keep k_exp_hint (the channel's current exponent)
    if k lands in uint16 range there — same accommodation as the manual
    worked example in calibration-guide.md §6."""
    k_exp = k_exp_hint
    for _ in range(20):
        d = 10.0 ** (-k_exp)
        k = round(gain * d)
        if 1 <= k <= 65535:
            return k, k_exp
        if k > 65535:
            k_exp -= 1
        else:
            k_exp += 1
        if k_exp < -9 or k_exp > 4:
            break
    k_exp = max(-9, min(4, k_exp))
    d = 10.0 ** (-k_exp)
    k = max(1, min(65535, round(gain * d)))
    return k, k_exp


def unlock_calibration(instr):
    write_holding(instr, EXT_CAL_UNLOCK, CAL_UNLOCK_STEP1)
    write_holding(instr, EXT_CAL_UNLOCK, CAL_UNLOCK_STEP2)
    write_holding(instr, SYS_OPERATING_MODE, OPERATING_MODE_CALIBRATION)
    mode = read_input(instr, 12)
    if mode != OPERATING_MODE_CALIBRATION:
        print(f"ERROR: expected Calibration mode, board reports {mode}. Aborting.")
        sys.exit(1)


def exit_calibration(instr):
    write_holding(instr, EXT_CAL_EXIT, 1)


def wait_for_stable_voltage(instr, ch, timeout_s, poll_s, tolerance_frac, tolerance_floor,
                             stable_needed, verbose=False):
    """Poll raw ADC voltage after a DAC step until it stops moving, instead
    of sleeping a fixed guessed duration.

    Diagnosed on real hardware: a direct DAC jump to a test voltage settles
    cleanly within ~1.5s (confirmed by logging raw_v continuously for 25s —
    flat from the very first sample). The real bug was an earlier version
    of this function comparing consecutive raw_v samples (which sit in the
    millions, e.g. ~5,930,000 raw counts at 1400V) against a tiny *absolute*
    tolerance (50 raw counts) — meaninglessly tight at that scale, so it
    reported "never stabilized" on literally every point even though the
    reading was already flat. Tolerance is now a fraction of the reading's
    own magnitude (with an absolute floor for when it's near zero).
    """
    t0 = time.monotonic()
    deadline = t0 + timeout_s
    prev = None
    stable = 0
    last = None
    while time.monotonic() < deadline:
        write_holding(instr, CH_CAL_SAMPLE_CMD(ch), 1)
        status = read_input(instr, CH_STATUS(ch))
        stale = status is not None and (status & STATUS_MEASUREMENT_STALE)
        if stale:
            if verbose:
                print(f"      [poll t={time.monotonic()-t0:5.2f}s] STALE, status={status}")
            time.sleep(poll_s)
            continue
        v = read_input32(instr, CH_RAW_ADC_VOLTAGE_HI(ch))
        last = v
        tolerance = max(tolerance_floor, abs(v) * tolerance_frac)
        if prev is not None and abs(v - prev) <= tolerance:
            stable += 1
            if verbose:
                print(f"      [poll t={time.monotonic()-t0:5.2f}s] v={v} "
                      f"tol={tolerance:.0f} stable={stable}/{stable_needed}")
            if stable >= stable_needed:
                return v, True
        else:
            if verbose:
                print(f"      [poll t={time.monotonic()-t0:5.2f}s] v={v} "
                      f"tol={tolerance:.0f} prev={prev} -> reset (stable=0)")
            stable = 0
        prev = v
        time.sleep(poll_s)
    return last, False


def sample_point(instr, ch, n_samples, inter_sample_s):
    """Trigger and average n_samples raw ADC voltage+current readings."""
    vs, ins = [], []
    for i in range(n_samples):
        write_holding(instr, CH_CAL_SAMPLE_CMD(ch), 1)
        status = read_input(instr, CH_STATUS(ch))
        if status is None or not (status & STATUS_MEASUREMENT_STALE):
            vs.append(read_input32(instr, CH_RAW_ADC_VOLTAGE_HI(ch)))
            ins.append(read_input32(instr, CH_RAW_ADC_CURRENT_HI(ch)))
        if i < n_samples - 1:
            time.sleep(inter_sample_s)
    return vs, ins


def calibrate_channel(instr, ch, args, dry_run):
    caps = read_input(instr, CH_CAPABILITIES(ch))
    if caps is None or not (caps & CH_CAP_RAW_OUTPUT_DRIVE):
        print(f"  CH{ch}: SKIP — no RAW_OUTPUT_DRIVE capability")
        return None
    if not (caps & CH_CAP_VOLTAGE_MEASUREMENT) or not (caps & CH_CAP_CURRENT_MEASUREMENT):
        print(f"  CH{ch}: SKIP — missing voltage/current measurement capability")
        return None

    out_k = read_holding(instr, CH_OUTPUT_CAL_K(ch))
    out_k_exp = read_holding(instr, CH_OUTPUT_CAL_K_EXP(ch), signed=True)
    out_b = read_holding(instr, CH_OUTPUT_CAL_B(ch), signed=True)
    v_k_old = read_holding(instr, CH_MEASURED_V_CAL_K(ch))
    v_kexp_old = read_holding(instr, CH_MEASURED_V_CAL_K_EXP(ch), signed=True)
    v_b_old = read_holding(instr, CH_MEASURED_V_CAL_B(ch), signed=True)
    i_k = read_holding(instr, CH_MEASURED_I_CAL_K(ch))
    i_kexp = read_holding(instr, CH_MEASURED_I_CAL_K_EXP(ch), signed=True)
    i_b_old = read_holding(instr, CH_MEASURED_I_CAL_B(ch), signed=True)

    points_v_raw = [round(f * args.max_voltage_raw) for f in args.fractions]
    dac_codes = [dac_code_for_voltage_raw(v, out_k, out_k_exp, out_b) for v in points_v_raw]

    print(f"\n  CH{ch} plan (output cal k={out_k} k_exp={out_k_exp} b={out_b} — unchanged):")
    for v_raw, code, n in zip(points_v_raw, dac_codes, args.samples_per_point):
        print(f"    {v_raw/10:7.1f} V -> DAC code {code:5d}  ({n} samples)")

    if dry_run:
        print("  Dry run — no output commanded, nothing written.")
        return None

    print(f"\n  *** About to drive CH{ch} up to {max(points_v_raw)/10:.1f} V. ***")
    if not args.yes:
        resp = input("  Type 'yes' to proceed with this channel, anything else skips it: ")
        if resp.strip().lower() != "yes":
            print("  Skipped.")
            return None

    write_holding(instr, CH_CAL_OUTPUT_ENABLE(ch), 1)

    raw_v_avg, raw_i_avg = [], []
    try:
        for v_raw, code, n in zip(points_v_raw, dac_codes, args.samples_per_point):
            write_holding(instr, CH_CAL_DAC_CODE(ch), code)
            print(f"    -> {v_raw/10:.1f} V (code {code}), waiting for settle "
                  f"(up to {args.settle_timeout_s:.0f}s)...")
            settled_v, settled = wait_for_stable_voltage(
                instr, ch, args.settle_timeout_s, args.settle_poll_s,
                args.settle_tolerance_frac, args.settle_tolerance_floor,
                args.settle_stable_count)
            if not settled:
                print(f"    WARNING: voltage never stabilized within "
                      f"{args.settle_timeout_s:.0f}s (last raw_v={settled_v}) "
                      f"— sampling anyway, expect a bad residual.")
            vs, ins = sample_point(instr, ch, n, args.inter_sample_s)
            if not vs:
                print(f"    WARNING: no valid samples at {v_raw/10:.1f}V, skipping channel")
                raw_v_avg, raw_i_avg = [], []
                break
            raw_v_avg.append(statistics.mean(vs))
            raw_i_avg.append(statistics.mean(ins))
            print(f"       raw_v={raw_v_avg[-1]:.1f}  raw_i={raw_i_avg[-1]:.1f}  (n={len(vs)})")
    finally:
        write_holding(instr, CH_CAL_DAC_CODE(ch), 0)
        write_holding(instr, CH_CAL_OUTPUT_ENABLE(ch), 0)

    if not raw_v_avg:
        return None

    # ---- Real-load guard: check EVERY point, not just the lowest ----
    # A real load (e.g. an actually-connected detector) can draw negligible
    # current at low bias and only become significant well into the range —
    # checking only the first/lowest point (as an earlier version of this
    # tool did) missed exactly that case on real hardware: a connected
    # detector read near-zero at 200V and exploded to a huge, wildly
    # non-monotonic value by 1800V, and a lowest-point-only guard let it
    # through. A real load invalidates BOTH axes' "no load connected"
    # assumption (the voltage self-cal also assumes a clean, lightly-loaded
    # output — a real load can pull the achieved voltage away from what the
    # DAC-code mapping alone would predict), so this aborts the whole
    # channel, not just the current axis.
    int16_span = 65535
    load_threshold = args.load_suspect_fraction * int16_span
    per_point_calibrated_i = [apply_gain(raw_i, i_k, i_kexp) + i_b_old for raw_i in raw_i_avg]
    offending = [(v_raw / 10, ci) for v_raw, ci in zip(points_v_raw, per_point_calibrated_i)
                 if abs(ci) > load_threshold]
    # A real load's current can grow with voltage rather than being uniformly
    # large — confirmed on real hardware: an attached detector read small at
    # the lowest test point (well under any sane absolute threshold) and
    # enormous by the highest, and only the highest point alone would have
    # tripped a pure per-point check. Flat, small leakage shouldn't show a
    # large SPREAD across points either, so check that independently.
    i_span = max(per_point_calibrated_i) - min(per_point_calibrated_i)
    spread_suspect = i_span > load_threshold
    if offending or spread_suspect:
        if offending:
            print(f"  CH{ch}: current reads too large to be leakage at "
                  f"{[f'{v:.0f}V={ci:.0f}' for v, ci in offending]} (raw "
                  f"register units, under the OLD calibration).")
        if spread_suspect:
            print(f"  CH{ch}: current spread across test points ({i_span:.0f}) "
                  f"is too large for flat leakage — "
                  f"{[f'{v_raw/10:.0f}V={ci:.0f}' for v_raw, ci in zip(points_v_raw, per_point_calibrated_i)]}.")
        print(f"  CH{ch}: looks like a real load is connected. ABORTING "
              f"calibration for this channel entirely (both axes) rather "
              f"than fitting against it. Disconnect any load/detector and "
              f"rerun.")
        return None

    # ---- Voltage axis: least-squares fit, gated on fit quality ----
    slope, intercept, max_resid = least_squares(raw_v_avg, points_v_raw)
    v_k_new, v_kexp_new = k_kexp_from_gain(slope, v_kexp_old)
    v_b_new = round(intercept)
    max_resid_v = max_resid / 10
    print(f"\n  CH{ch} voltage fit: k={v_k_new} k_exp={v_kexp_new} b={v_b_new}  "
          f"(was k={v_k_old} k_exp={v_kexp_old} b={v_b_old})  "
          f"max residual = {max_resid_v:.2f} V")
    if not (-32768 <= v_b_new <= 32767):
        print(f"  CH{ch}: voltage fit B={v_b_new} out of int16 range — SKIPPING voltage axis")
        v_k_new = None
    elif max_resid_v > args.max_residual_v:
        print(f"  CH{ch}: residual {max_resid_v:.2f}V exceeds --max-residual-v "
              f"({args.max_residual_v}V) — the 5 points don't fit a line well "
              f"enough to trust. SKIPPING voltage axis for this channel "
              f"(not committing a bad fit).")
        v_k_new = None

    # ---- Current axis: offset only, from the lowest (most-sampled) point ----
    # (Already confirmed above that every point looks like leakage, not a
    # real load, so it's safe to use any of them — the lowest gets the most
    # samples and is closest to typical operating conditions.)
    implied_b_per_point = [-apply_gain(raw_i, i_k, i_kexp) for raw_i in raw_i_avg]
    i_b_new = round(implied_b_per_point[0])
    i_spread = max(implied_b_per_point) - min(implied_b_per_point)

    print(f"    Current: b={i_b_new}  (was b={i_b_old}, k/k_exp unchanged: "
          f"k={i_k} k_exp={i_kexp})")
    print(f"    Per-point implied current offset: "
          f"{[round(b) for b in implied_b_per_point]}"
          f"  (spread={i_spread:.0f} — large spread suggests voltage-dependent "
          f"leakage, which a single offset can't fully correct)")

    if v_k_new is None and i_b_new is None:
        print(f"  CH{ch}: nothing passed quality gates — not committing anything.")
        return None

    return {
        "ch": ch, "v_k": v_k_new, "v_kexp": v_kexp_new, "v_b": v_b_new,
        "i_b": i_b_new, "i_k": i_k, "i_kexp": i_kexp,
    }


def commit_channel(instr, plan):
    ch = plan["ch"]
    if plan["v_k"] is not None:
        write_holding(instr, CH_MEASURED_V_CAL_K(ch), plan["v_k"])
        write_holding(instr, CH_MEASURED_V_CAL_K_EXP(ch), plan["v_kexp"], signed=True)
        write_holding(instr, CH_MEASURED_V_CAL_B(ch), plan["v_b"], signed=True)
    if plan["i_b"] is not None:
        write_holding(instr, CH_MEASURED_I_CAL_B(ch), plan["i_b"], signed=True)
    write_holding(instr, CH_CAL_COMMIT_CMD(ch), 1)
    print(f"  CH{ch}: committed.")


def main():
    parser = argparse.ArgumentParser(
        description="jw_hvb self-calibration (no external instruments) — "
                     "FALLBACK ONLY, drives real high voltage.")
    parser.add_argument("--port", default="/dev/ttyUSB0", help="Serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--slave", type=int, default=1, help="Slave ID")
    parser.add_argument("--timeout", type=float, default=3.0,
                         help="Modbus timeout (s) — Cal Sample Command is "
                              "synchronous (blocks until a real ADC "
                              "conversion completes) and can occasionally "
                              "take longer than a plain register read/write, "
                              "confirmed on real hardware; 1.0s was too "
                              "short and caused spurious failures")
    parser.add_argument("--channels", default=None,
                         help="Comma-separated channels to calibrate (default: all)")
    parser.add_argument("--fractions", default=",".join(str(f) for f in DEFAULT_FRACTIONS),
                         help="Comma-separated fractions of max voltage to test")
    parser.add_argument("--samples-per-point",
                         default=",".join(str(n) for n in DEFAULT_SAMPLES_PER_POINT),
                         help="Comma-separated sample count per test point")
    parser.add_argument("--max-voltage", type=float, default=JW_HVB_MAX_TARGET_VOLTAGE_RAW / 10,
                         help="Board's rated max output voltage in volts (default 2000)")
    parser.add_argument("--settle-timeout-s", type=float, default=8.0,
                         help="Max time to wait for the voltage reading to "
                              "stabilize after each DAC step before giving "
                              "up and sampling anyway (default 8.0)")
    parser.add_argument("--settle-poll-s", type=float, default=0.3,
                         help="Interval between stabilization polls while "
                              "waiting to settle (default 0.3)")
    parser.add_argument("--settle-tolerance-frac", type=float, default=0.001,
                         help="Max fractional change (of the reading's own "
                              "magnitude) between consecutive polls to "
                              "consider the voltage settled (default 0.001, "
                              "i.e. 0.1%%) — raw ADC voltage readings are on "
                              "the order of millions of counts, so a fixed "
                              "absolute tolerance doesn't scale")
    parser.add_argument("--settle-tolerance-floor", type=int, default=200,
                         help="Absolute raw ADC count floor for the settle "
                              "tolerance, used when the reading itself is "
                              "near zero (default 200)")
    parser.add_argument("--settle-stable-count", type=int, default=3,
                         help="Consecutive polls within tolerance required "
                              "before considering the voltage settled "
                              "(default 3)")
    parser.add_argument("--inter-sample-s", type=float, default=0.3,
                         help="Delay between samples at the same point")
    parser.add_argument("--load-suspect-fraction", type=float,
                         default=DEFAULT_LOAD_SUSPECT_FRACTION,
                         help="Fraction of the int16 register range above "
                              "which a current reading (or the spread across "
                              "test points) is treated as a real connected "
                              "load rather than leakage, aborting that "
                              "channel entirely (default 0.05)")
    parser.add_argument("--max-residual-v", type=float,
                         default=DEFAULT_MAX_RESIDUAL_V,
                         help="Max acceptable voltage-fit residual in volts "
                              "before refusing to commit it (default 20.0)")
    parser.add_argument("--dry-run", action="store_true",
                         help="Print the plan (DAC codes, voltages) for every "
                              "channel, command nothing")
    parser.add_argument("-y", "--yes", action="store_true",
                         help="Skip the per-channel typed HV confirmation")
    args = parser.parse_args()

    fractions = [float(f) for f in args.fractions.split(",")]
    samples = [int(n) for n in args.samples_per_point.split(",")]
    if len(fractions) != len(samples):
        print("ERROR: --fractions and --samples-per-point must have the same length")
        sys.exit(1)
    # calibrate_channel assumes ascending order (index 0 = lowest/safest/most-
    # sampled point, used as the current-axis offset source) — sort here so
    # that holds regardless of the order --fractions was given in.
    fractions, samples = zip(*sorted(zip(fractions, samples)))
    args.fractions = list(fractions)
    args.samples_per_point = list(samples)
    args.max_voltage_raw = args.max_voltage * 10

    instr = connect(args)
    check_board_is_jw_hvb(instr)

    if args.channels:
        channels = [int(c) for c in args.channels.split(",")]
    else:
        n = read_input(instr, SYS_SUPPORTED_CH) or 0
        channels = list(range(n))

    print(f"jw_hvb self-calibration — {args.port}, channels {channels}")
    print(f"Test points: {[f'{f*100:.0f}%' for f in args.fractions]} of "
          f"{args.max_voltage:.0f}V = "
          f"{[round(f*args.max_voltage) for f in args.fractions]} V")
    print("*** This is a fallback for when professional instruments are not "
          "available. Official calibration always uses a real DMM/reference "
          "load (see docs/guide/calibration-guide.md §5-6). ***")

    plans = []
    if not args.dry_run:
        unlock_calibration(instr)
        print("Calibration Mode active.")
        try:
            for ch in channels:
                plan = calibrate_channel(instr, ch, args, args.dry_run)
                if plan is not None:
                    plans.append(plan)
            for plan in plans:
                commit_channel(instr, plan)
        finally:
            # Always try to leave Calibration Mode, even if a channel raised
            # partway through — don't strand the board in Cal Mode (which
            # force-disables every channel's output) on an error.
            exit_calibration(instr)
            print("\nExited Calibration Mode.")
    else:
        for ch in channels:
            calibrate_channel(instr, ch, args, args.dry_run)

    instr.serial.close()
    print("\nDone." if not args.dry_run else "\nDry run complete — nothing written.")


if __name__ == "__main__":
    main()
