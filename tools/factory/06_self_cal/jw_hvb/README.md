# jw_hvb Self-Calibration (No External Instruments)

`jw_hvb_selfcal.py` is a **fallback** calibration procedure for jw_hvb,
for use when a real DMM/reference load isn't available. **Official
calibration always uses professional instruments** against the raw ADC
codes — see `docs/guide/calibration-guide.md` §5-6. Use this tool only
as an alternative when that isn't possible.

**This drives real high voltage** — up to ~90% of the board's rated max
(1800V at default settings) on the channel's HV output terminals. Ensure
the output is safely terminated/isolated and no one can contact it before
running this.

## What it calibrates, and what it deliberately leaves alone

- **Voltage measurement axis (k/k_exp/b)**: fitted via least-squares across
  5 test points (default 10/30/50/70/90% of max voltage), using the
  channel's own *existing* output calibration to convert each test voltage
  into a DAC code — this is what makes it self-referenced: no DMM, the
  DAC's own presumed-accurate voltage mapping stands in as ground truth.
  Reports the fit's residual; near-zero means the measurement path is
  genuinely linear, a large one flags something a simpler 2-point fit
  could never reveal.
- **Output axis (DAC calibration)**: never touched.
- **Current measurement gain (k/k_exp)**: never touched — this board's
  current sensing is a physical shunt + precision ADC, already
  circuit-characterized and accurate; only the **offset (b)** is derived,
  from the lowest (safest, most heavily sampled) test point, but only after
  every point has been checked for signs of a real connected load — see
  below. The tool also reports the per-point implied offset across all 5
  points, since HV leakage current can genuinely scale with voltage — a
  single constant `b` can't fully correct for that regardless of which
  point you pick, so seeing the spread explicitly matters here.

## Install

```bash
pip install -r tools/jw_hvb_selfcal/requirements.txt
```

## Run

Always preview first:

```bash
python3 tools/jw_hvb_selfcal/jw_hvb_selfcal.py --port /dev/ttyUSB0 --dry-run
```

Then, for real (asks a typed "yes" confirmation per channel before
commanding any voltage on it):

```bash
python3 tools/jw_hvb_selfcal/jw_hvb_selfcal.py --port /dev/ttyUSB0
```

Options:

```text
--port PATH             Serial port (default: /dev/ttyUSB0)
--baud RATE             Baud rate (default: 115200)
--slave ID              Modbus slave ID (default: 1)
--timeout SECONDS       Modbus timeout (default: 3.0 — Cal Sample Command
                        is synchronous, blocking until a real ADC
                        conversion completes, and confirmed on real
                        hardware to occasionally need more than 1s)
--channels LIST         Comma-separated channels (default: all reported)
--fractions LIST        Comma-separated fractions of max voltage to test
                        (default: 0.10,0.30,0.50,0.70,0.90)
--samples-per-point LIST
                        Comma-separated sample count per point, same order
                        as --fractions (default: 8,5,4,3,2 — more dwell
                        time/statistics at the low end, fewer at the top)
--max-voltage VOLTS     Board's rated max output voltage (default: 2000)
--settle-timeout-s SECONDS
                        Max time to wait for the voltage reading to actually
                        stabilize after each DAC step before giving up and
                        sampling anyway (default: 8.0) — a large setpoint
                        jump's physical settling time isn't a known
                        constant, so this polls for real stabilization
                        rather than sleeping a fixed guess
--settle-poll-s SECONDS Interval between stabilization polls (default: 0.3)
--settle-tolerance-frac FRACTION
                        Max fractional change (of the reading's own
                        magnitude) between consecutive polls to call it
                        settled (default: 0.001, i.e. 0.1%) — raw ADC
                        voltage readings are on the order of millions of
                        counts, so a fixed absolute tolerance doesn't scale
--settle-tolerance-floor COUNTS
                        Absolute raw ADC count floor for the tolerance,
                        used when the reading itself is near zero
                        (default: 200)
--settle-stable-count N Consecutive polls within tolerance required before
                        considering it settled (default: 3)
--inter-sample-s SECONDS
                        Delay between samples at the same point (default: 0.3)
--load-suspect-fraction FRACTION
                        Fraction of the int16 register range above which a
                        current reading (or the spread across test points)
                        is treated as a real connected load rather than
                        leakage (default: 0.05)
--max-residual-v VOLTS  Max acceptable voltage-fit residual before refusing
                        to commit it (default: 20.0)
--dry-run               Print the plan (voltages, DAC codes) for every
                        channel, command nothing
-y, --yes               Skip the per-channel typed HV confirmation
```

## Safety notes

- Steps up through the test points in order (never jumps straight to the
  top), and steps back down through them (not straight to 0) when done
  with a channel or on error.
- **Real-load guard, checked at every test point, not just one**: if any
  point's current looks too large to be leakage, *or* the spread across
  points is too large for flat leakage, the whole channel is aborted (both
  axes — a real load invalidates the voltage self-cal too, not just the
  current offset) with a clear message, rather than fitting against it.
  This exists because of a real failure found on actual hardware: a
  channel with a detector attached read near-zero current at the lowest
  test voltage and an enormous, wildly non-monotonic value by the highest
  — checking only the lowest point (an earlier version of this guard)
  missed it entirely.
- **Voltage-fit residual gate**: the least-squares fit is only committed if
  its max residual is under `--max-residual-v`. A 2-point fit can never
  fail this check (it always passes through both points exactly), which is
  exactly why it's not a substitute for one — 5+ points catching a bad fit
  is the point.
- Calibration Mode is always exited, even if a channel errors out partway
  through, so the board isn't left stranded with every channel force-disabled.
- Firmware enforces the documented safety rules regardless of this tool:
  only one channel's calibration output active at a time, hardware
  interlock still blocks non-zero output even in Calibration Mode, and the
  inactivity watchdog (default 300s) will exit Calibration Mode on its own
  if the session stalls.
- **This tool cannot produce a valid calibration on a channel with anything
  connected to its HV output** (a detector, a test load, etc.) — the whole
  premise is "no load connected." Disconnect first, or don't run it on that
  channel.

## Dependencies

`minimalmodbus` (see `requirements.txt`) — the only third-party dependency.
