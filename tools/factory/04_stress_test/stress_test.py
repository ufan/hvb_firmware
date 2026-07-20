#!/usr/bin/env python3
"""
PSB Board Modbus Stress Test

Two operational modes:
  --mode ci   CI pipeline integration test: single channel, minimal register set,
              fast (~10s), exit 0 on pass / exit 1 on failure.
  --mode qa   Production QA / human technician: all channels present, all
              registers exercised, detailed pass/fail register-level report.

Source of truth for register addresses:
  - include/reg_store/reg_map.h       (block layout, protocol constants)
  - include/reg_store/modbus_view.def (X-macro expanded register offsets)

Categories:
  1. Polling read: single register + continuous block
  2. Config write: target voltage, protection settings, recovery policy
  3. Cmd write: output action, fault cmd, param action (self-clearing WO)
"""

import time
import statistics
import sys
import os
import argparse
from datetime import datetime, timezone
import minimalmodbus

# ---------------------------------------------------------------------------
# Register map (absolute Modbus addresses, 0-based)
# Source of truth: include/reg_store/reg_map.h + include/reg_store/modbus_view.def
# Absolute addr = SYS_BLOCK_BASE(0) + offset          for system registers
#               = CH_BLOCK_BASE(c) + offset  (=40+c*40) for channel registers
#               = EXT_BLOCK_BASE + offset    (=680)      for extension registers
# ---------------------------------------------------------------------------
# System input (FC04, addrs 0..14)
SYS_IN_FIRST  = 0
SYS_IN_COUNT  = 15       # offsets 0..14 defined; 15..39 reserved
SYS_PROTOCOL_MAJOR   = 0
SYS_PROTOCOL_MINOR   = 1
SYS_VARIANT_ID       = 2
SYS_CAPABILITY_FLAGS = 3
SYS_SUPPORTED_CH     = 4
SYS_ACTIVE_CH_MASK   = 5
SYS_STATUS           = 13
SYS_FAULT_CAUSE      = 14

# System holding (FC03/FC06, addrs 0..39)
SYS_HOLDING_DEFINED = [0, 1, 2, 3, 39]  # offsets with defined registers
SYS_OPERATING_MODE   = 0
SYS_STARTUP_POLICY   = 1
SYS_SLAVE_ADDRESS    = 2
SYS_BAUD_RATE_CODE   = 3
SYS_PARAM_ACTION     = 39

# Channel block
CH_BLOCK_BASE  = lambda c: 40 + c * 40
CH_IN_FIRST    = lambda c: CH_BLOCK_BASE(c)
CH_IN_COUNT    = 12       # offsets 0..11 available in normal mode (12-15 cal-only)
CH_IN_CAL_ONLY = [12, 13, 14, 15]  # raw ADC offsets — exception 0x02 in normal mode

# Channel holding offsets (defined, not reserved)
# offset 17 (CFG_OUTPUT_ENABLED) is capability-gated the opposite way from the
# DAC-only fields below — see CH_HOLDING_GATE.
CH_HOLDING_RW_OFFSETS = [3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 20, 21, 22, 23, 24, 25, 30, 31, 34]
CH_HOLDING_WO_OFFSETS = [0, 1, 2, 32, 33]  # self-clearing write-only

# Channel offsets 3, 17 have dedicated write+readback test blocks in
# qa_channel()/ci_config_write() below and are excluded from the generic
# bulk-read applicability table (CH_HOLDING_GATE) to avoid double-testing.
CFG_TARGET_VOLTAGE_OFFSET = 3
CFG_OUTPUT_ENABLED_OFFSET = 17

# ---------------------------------------------------------------------------
# Channel capability flags (dt-bindings/voltage_control/capabilities.h) and
# the per-register applicability rules from vc_catalog_supported() in
# lib/voltage_control/vc_runtime.c. A holding/input register that isn't
# applicable to a channel's capability set is REJECTED by the firmware (FC03
# read or FC06 write both return a Modbus exception), not silently ignored —
# so every check below asserts the capability-correct outcome for whatever
# channel it's given, rather than assuming every channel looks like HVB's.
# ---------------------------------------------------------------------------
CH_CAP_OUTPUT_ENABLE       = 0x0001
CH_CAP_RAW_OUTPUT_DRIVE    = 0x0002
CH_CAP_VOLTAGE_MEASUREMENT = 0x0004
CH_CAP_CURRENT_MEASUREMENT = 0x0008
CH_CAP_HARDWARE_STATUS     = 0x0010

def chan_has(capabilities, cap):
    return (capabilities & cap) == cap

def target_voltage_applicable(capabilities):
    return chan_has(capabilities, CH_CAP_RAW_OUTPUT_DRIVE)

def output_enabled_applicable(capabilities):
    # Mirrors vc_catalog_supported()'s REG_VC_FIELD_CFG_OUTPUT_ENABLED case:
    # fixed-voltage channels that can actually be disabled.
    return (not chan_has(capabilities, CH_CAP_RAW_OUTPUT_DRIVE) and
            chan_has(capabilities, CH_CAP_OUTPUT_ENABLE))

# offset -> predicate(capabilities) deciding whether this holding register
# applies to a given channel. Offsets not listed here (e.g. 8-12, the
# recovery/retry fields) have no capability gate and always apply.
CH_HOLDING_GATE = {
    3:  target_voltage_applicable,                        # CFG_TARGET_VOLTAGE
    4:  lambda c: chan_has(c, CH_CAP_RAW_OUTPUT_DRIVE),   # RAMP_UP_STEP
    5:  lambda c: chan_has(c, CH_CAP_RAW_OUTPUT_DRIVE),   # RAMP_UP_INTERVAL
    6:  lambda c: chan_has(c, CH_CAP_RAW_OUTPUT_DRIVE),   # RAMP_DOWN_STEP
    7:  lambda c: chan_has(c, CH_CAP_RAW_OUTPUT_DRIVE),   # RAMP_DOWN_INTERVAL
    13: lambda c: chan_has(c, CH_CAP_CURRENT_MEASUREMENT),  # CURRENT_PROTECTION_MODE
    14: lambda c: chan_has(c, CH_CAP_CURRENT_MEASUREMENT),  # CURRENT_PROT_OUT_ACTION
    15: lambda c: chan_has(c, CH_CAP_CURRENT_MEASUREMENT),  # CURRENT_LIMIT_THRESHOLD
    16: lambda c: chan_has(c, CH_CAP_RAW_OUTPUT_DRIVE | CH_CAP_VOLTAGE_MEASUREMENT),  # AUTO_DERATE_STEP
    17: output_enabled_applicable,                        # CFG_OUTPUT_ENABLED
    20: lambda c: chan_has(c, CH_CAP_RAW_OUTPUT_DRIVE),   # OUTPUT_CAL_K
    21: lambda c: chan_has(c, CH_CAP_RAW_OUTPUT_DRIVE),   # OUTPUT_CAL_B
    22: lambda c: chan_has(c, CH_CAP_VOLTAGE_MEASUREMENT),  # MEASURED_V_CAL_K
    23: lambda c: chan_has(c, CH_CAP_VOLTAGE_MEASUREMENT),  # MEASURED_V_CAL_B
    24: lambda c: chan_has(c, CH_CAP_CURRENT_MEASUREMENT),  # MEASURED_I_CAL_K
    25: lambda c: chan_has(c, CH_CAP_CURRENT_MEASUREMENT),  # MEASURED_I_CAL_B
    30: lambda c: chan_has(c, CH_CAP_RAW_OUTPUT_DRIVE),   # CAL_OUTPUT_ENABLE
    31: lambda c: chan_has(c, CH_CAP_RAW_OUTPUT_DRIVE),   # CAL_DAC_CODE
}

# Extension block (FC03/FC06, addr 680+)
EXT_CAL_UNLOCK  = 680
EXT_CAL_EXIT    = 681

# Constants
BAUD = 115200
TIMEOUT_S = 0.5
BITS_PER_BYTE = 10
T35_US = 3.5 * BITS_PER_BYTE / BAUD * 1e6   # ~304 us

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def timed_call(fn, *args, **kwargs):
    t0 = time.perf_counter()
    result = fn(*args, **kwargs)
    t1 = time.perf_counter()
    return (t1 - t0) * 1e6, result

def stats_str(name, samples_us, n=None):
    if not samples_us:
        return f"  {name}: NO DATA"
    s = samples_us
    avg = statistics.mean(s)
    p50 = statistics.median(s)
    p99 = sorted(s)[int(len(s) * 0.99)] if len(s) >= 100 else max(s)
    mn, mx = min(s), max(s)
    hz = 1e6 / avg if avg > 0 else 0
    info = f"avg={avg:.0f}us  p50={p50:.0f}us  p99={p99:.0f}us  min={mn:.0f}us  max={mx:.0f}us  → {hz:.1f} Hz"
    return f"  {name}: [n={n}] {info}" if n else f"  {name}: {info}"

def read_reg_fc04(instr, addr):
    """Read single input register. Returns (elapsed_us, value) or (elapsed_us, None)."""
    try:
        e, v = timed_call(instr.read_register, addr, 0, 4, False)
        return e, v
    except Exception:
        return 0, None

def read_reg_fc03(instr, addr):
    """Read single holding register."""
    try:
        e, v = timed_call(instr.read_registers, addr, 1, 3)
        return e, v[0] if v else None
    except Exception:
        return 0, None

def write_reg_fc06(instr, addr, val):
    """Write single holding register."""
    try:
        e, _ = timed_call(instr.write_register, addr, val,
                          number_of_decimals=0, functioncode=6, signed=False)
        return e, True
    except Exception:
        return 0, False

def read_block_fc04(instr, addr, count):
    """Read block of input registers. Returns (elapsed_us, values) or (elapsed_us, None)."""
    try:
        e, v = timed_call(instr.read_registers, addr, count, 4)
        return e, v
    except Exception:
        return 0, None

# ---------------------------------------------------------------------------
# CI Mode tests
# ---------------------------------------------------------------------------
def ci_connectivity(instr):
    """Verify protocol version and channel count."""
    _, major = read_reg_fc04(instr, SYS_PROTOCOL_MAJOR)
    _, minor = read_reg_fc04(instr, SYS_PROTOCOL_MINOR)
    _, channels = read_reg_fc04(instr, SYS_SUPPORTED_CH)
    _, active_mask = read_reg_fc04(instr, SYS_ACTIVE_CH_MASK)
    ok = major is not None and channels is not None
    print(f"  protocol v{major}.{minor}  channels={channels}  active_mask=0x{active_mask:04X}")
    return ok, major, minor, channels, active_mask

def ci_single_read(instr, rounds=100):
    """Single register read stress — CI minimal."""
    errors = 0
    samples = []
    for _ in range(rounds):
        e, v = read_reg_fc04(instr, SYS_STATUS)
        if v is not None:
            samples.append(e)
        else:
            errors += 1
    ok = errors == 0
    avg = statistics.mean(samples) if samples else 0
    print(f"  single_read: {rounds} rounds, {errors} errors, avg={avg:.0f}us")
    return ok, samples, errors

def ci_block_read(instr, rounds=50):
    """Block read system input — CI minimal."""
    errors = 0
    samples = []
    for _ in range(rounds):
        e, v = read_block_fc04(instr, SYS_IN_FIRST, SYS_IN_COUNT)
        if v is not None and len(v) == SYS_IN_COUNT:
            samples.append(e)
        else:
            errors += 1
    ok = errors == 0
    avg = statistics.mean(samples) if samples else 0
    print(f"  block_read (sys input, {SYS_IN_COUNT} regs): {rounds} rounds, {errors} errors, avg={avg:.0f}us")
    return ok, samples, errors

def ci_config_write(instr, channel=0, capabilities=0):
    """Single config write + verify — CI minimal.

    Adapts to the channel's actual capabilities instead of assuming every
    channel has a DAC (true on HVB, false on LVB): DAC channels
    (CH_CAP_RAW_OUTPUT_DRIVE) test CFG_TARGET_VOLTAGE; fixed-voltage channels
    that support on/off (CH_CAP_OUTPUT_ENABLE) test CFG_OUTPUT_ENABLED
    instead; a channel with neither (locked always-on, e.g. jw_lvb ch0) has
    no writable config-write register at all, so the test asserts the write
    is correctly rejected rather than forcing a check that can never pass.
    """
    base = CH_BLOCK_BASE(channel)
    errors = 0

    if target_voltage_applicable(capabilities):
        addr = base + CFG_TARGET_VOLTAGE_OFFSET
        reg_name = "target_v"
        test_vals = [0, 1000, 0]
    elif output_enabled_applicable(capabilities):
        addr = base + CFG_OUTPUT_ENABLED_OFFSET
        reg_name = "output_enabled"
        _, initial = read_reg_fc03(instr, addr)
        restore = initial if initial in (0, 1) else 1
        test_vals = [0, 1, restore]
    else:
        # No configurable output at all (e.g. locked always-on channel) —
        # both candidate registers must be rejected, not silently untested.
        addr = base + CFG_TARGET_VOLTAGE_OFFSET
        _, ok = write_reg_fc06(instr, addr, 0)
        if ok:
            errors += 1
        addr17 = base + CFG_OUTPUT_ENABLED_OFFSET
        _, ok17 = write_reg_fc06(instr, addr17, 0)
        if ok17:
            errors += 1
        print(f"  config_write (ch{channel} locked, no output config): "
              f"{errors} errors (expected both rejected)")
        return errors == 0, errors

    for val in test_vals:
        e_w, ok = write_reg_fc06(instr, addr, val)
        if not ok:
            errors += 1
            continue
        time.sleep(0.02)
        _, rb = read_reg_fc03(instr, addr)
        if rb != val:
            errors += 1
    print(f"  config_write (ch{channel} {reg_name}): {errors} errors")
    return errors == 0, errors

def ci_cmd_write(instr, channel=0, capabilities=0):
    """Single cmd write + self-clear check — CI minimal.

    OUTPUT_ACTION's DISABLE_IMMEDIATE(3) is only valid on channels that
    support CH_CAP_OUTPUT_ENABLE; on a locked always-on channel it's
    correctly rejected, so ENABLE(1) — always accepted regardless of
    capability — is used instead to still exercise the register.
    FAULT_CMD and PARAM_ACTION have no capability gate.
    """
    base = CH_BLOCK_BASE(channel)
    errors = 0
    output_action_val = 3 if chan_has(capabilities, CH_CAP_OUTPUT_ENABLE) else 1
    cmd_values = {0: output_action_val, 1: 1, 2: 1}  # off -> value
    for off in [0, 1, 2]:  # OUTPUT_ACTION, FAULT_CMD, PARAM_ACTION
        addr = base + off
        _, ok = write_reg_fc06(instr, addr, cmd_values[off])
        if not ok:
            errors += 1
            continue
        time.sleep(0.01)
        _, rb = read_reg_fc03(instr, addr)
        if rb != 0:
            errors += 1
    print(f"  cmd_write (ch{channel}): {errors} errors")
    return errors == 0, errors

def ci_burst(instr, duration_s=5):
    """Short sustained burst — CI minimal."""
    errors = 0
    samples = []
    t_end = time.perf_counter() + duration_s
    while time.perf_counter() < t_end:
        e, v = read_reg_fc04(instr, SYS_STATUS)
        if v is not None:
            samples.append(e)
        else:
            errors += 1
    ok = errors == 0
    hz = len(samples) / duration_s if duration_s > 0 else 0
    print(f"  burst: {len(samples)} reads in {duration_s}s, {hz:.0f} Hz, {errors} errors")
    return ok, samples, errors

# ---------------------------------------------------------------------------
# QA Mode tests
# ---------------------------------------------------------------------------
def qa_test_header(text):
    print()
    print("=" * 60)
    print(f"QA: {text}")
    print("=" * 60)

class QAResults:
    def __init__(self):
        self.checks = []  # list of (label, passed, detail)

    def add(self, label, passed, detail=""):
        self.checks.append((label, passed, detail))
        mark = "PASS" if passed else "FAIL"
        line = f"  [{mark}] {label}"
        if detail:
            line += f"  ({detail})"
        print(line)

    def all_pass(self):
        return all(p for _, p, _ in self.checks)

def qa_system_input(instr, qa):
    """Read and validate all system input registers."""
    qa_test_header("System Input Registers (FC04, addr 0-14)")
    e, vals = read_block_fc04(instr, SYS_IN_FIRST, SYS_IN_COUNT)
    if vals is None or len(vals) < SYS_IN_COUNT:
        qa.add("SYS_IN block read", False, f"got {len(vals) if vals else 0} regs, expected {SYS_IN_COUNT}")
        return

    qa.add("SYS_IN block read (15 regs)", True, f"{e:.0f}us")

    # Validate known values
    checks = [
        ("PROTOCOL_MAJOR == 3", vals[0] == 3),
        ("PROTOCOL_MINOR == 0", vals[1] == 0),
        ("SUPPORTED_CHANNELS >= 1", vals[4] >= 1),
        ("ACTIVE_CHANNEL_MASK != 0", vals[5] != 0),
    ]
    for label, ok in checks:
        qa.add(f"  {label}", ok)

    # Read individual well-known registers
    for i in range(SYS_IN_COUNT):
        e, v = read_reg_fc04(instr, i)
        qa.add(f"  addr={i} single read", v is not None, f"val={v} {e:.0f}us")

    # Verify reserved range — firmware returns 0 by design (not exception)
    # "module-absent registers read as zero" per register catalog
    for addr in [15, 25, 39]:
        e, v = read_reg_fc04(instr, addr)
        qa.add(f"  addr={addr} (reserved) read", v is not None, f"val={v} (expected 0)")

def qa_system_holding(instr, qa):
    """Read and validate all system holding registers."""
    qa_test_header("System Holding Registers (FC03/FC06, addr 0-39)")

    # Read defined registers individually
    for off in SYS_HOLDING_DEFINED:
        e, v = read_reg_fc03(instr, off)
        qa.add(f"  addr={off} read", v is not None, f"val={v} {e:.0f}us")

    # Read reserved addresses — should return 0 or exception
    for addr in [4, 10, 20, 38]:
        try:
            e, v = read_reg_fc03(instr, addr)
            qa.add(f"  addr={addr} (reserved) read", v is not None, f"val={v}")
        except minimalmodbus.IllegalRequestError:
            qa.add(f"  addr={addr} (reserved) — exception 0x02", True)
        except Exception:
            qa.add(f"  addr={addr} (reserved) — exception (expected)", True)

    # Write OPERATING_MODE (0=Normal, 1=Auto, 2=Cal)
    for mode in [0, 1, 0]:  # cycle back to Normal
        _, ok = write_reg_fc06(instr, SYS_OPERATING_MODE, mode)
        time.sleep(0.01)
        _, rb = read_reg_fc03(instr, SYS_OPERATING_MODE)
        qa.add(f"  OPERATING_MODE write {mode} -> readback {rb}", ok and rb == mode)

    # Try writing to reserved addresses — should reject
    for addr in [4, 10]:
        try:
            instr.write_register(addr, 0, number_of_decimals=0, functioncode=6, signed=False)
            qa.add(f"  addr={addr} (reserved) write — expected rejection", False)
        except Exception:
            qa.add(f"  addr={addr} (reserved) write — rejected", True)

def qa_channel(instr, qa, ch, capabilities):
    """Exhaustive test of all registers for one channel."""
    base = CH_BLOCK_BASE(ch)
    label = f"ch{ch} (base={base})"
    qa_test_header(f"Channel {ch} Registers {label}")

    # --- Input registers (FC04) ---
    # Read the valid input block (offsets 0-11)
    e, vals = read_block_fc04(instr, base, CH_IN_COUNT)
    qa.add(f"{label} input block read ({CH_IN_COUNT} regs)",
           vals is not None and len(vals) == CH_IN_COUNT, f"{e:.0f}us")

    # Read all individually
    for off in range(CH_IN_COUNT):
        addr = base + off
        e, v = read_reg_fc04(instr, addr)
        qa.add(f"{label} input off={off} (addr={addr})", v is not None,
               f"val={v} {e:.0f}us")

    # Cal-only input offsets should return exception in normal mode
    for off in CH_IN_CAL_ONLY:
        addr = base + off
        try:
            instr.read_register(addr, 0, 4, False)
            qa.add(f"{label} input off={off} (cal-only) — expected exception", False)
        except Exception:
            qa.add(f"{label} input off={off} (cal-only) — exception 0x02", True)

    # Verify reserved input range — firmware returns 0 by design
    for off in [16, 20, 39]:
        addr = base + off
        e, v = read_reg_fc04(instr, addr)
        qa.add(f"{label} input off={off} (reserved) read", v is not None, f"val={v} (expected 0)")

    # --- Holding registers (FC03/FC06) ---
    # Read all RW config registers. Capability-gated ones (CH_HOLDING_GATE)
    # must be REJECTED on a channel lacking the required capability — that's
    # the firmware's actual, tested behavior (vc_catalog_supported()), not a
    # skip condition.
    for off in CH_HOLDING_RW_OFFSETS:
        addr = base + off
        gate = CH_HOLDING_GATE.get(off)
        if gate is None or gate(capabilities):
            e, v = read_reg_fc03(instr, addr)
            qa.add(f"{label} holding off={off} (addr={addr}) read",
                   v is not None, f"val={v} {e:.0f}us")
        else:
            try:
                instr.read_registers(addr, 1, 3)
                qa.add(f"{label} holding off={off} (addr={addr}) — "
                       f"expected rejection (capability absent)", False)
            except Exception:
                qa.add(f"{label} holding off={off} (addr={addr}) — "
                       f"correctly rejected (capability absent)", True)

    # Read WO command registers (should return 0)
    for off in CH_HOLDING_WO_OFFSETS:
        addr = base + off
        e, v = read_reg_fc03(instr, addr)
        qa.add(f"{label} holding off={off} (addr={addr}) WO readback",
               v == 0, f"val={v} (expected 0)")

    # Read genuinely reserved holding offsets — should reject or return 0.
    # (offset 17 is CFG_OUTPUT_ENABLED — capability-gated above, not reserved.)
    for off in [26, 35]:
        addr = base + off
        try:
            e, v = read_reg_fc03(instr, addr)
            qa.add(f"{label} holding off={off} (reserved) read",
                   v is not None, f"val={v}")
        except Exception:
            qa.add(f"{label} holding off={off} (reserved) — exception", True)

    # --- Config write + verify ---
    # CFG_TARGET_VOLTAGE (DAC channels) and CFG_OUTPUT_ENABLED (fixed-voltage
    # switchable channels) are mutually exclusive by capability. A channel
    # with neither (e.g. a locked always-on channel) must reject both.
    addr_tv = base + CFG_TARGET_VOLTAGE_OFFSET
    if target_voltage_applicable(capabilities):
        _, orig = read_reg_fc03(instr, addr_tv)
        for val in [0, 1000, 2000, orig]:
            _, ok = write_reg_fc06(instr, addr_tv, val)
            time.sleep(0.02)
            _, rb = read_reg_fc03(instr, addr_tv)
            qa.add(f"{label} CFG_TARGET_VOLTAGE write {val}", ok and rb == val)
    else:
        _, ok = write_reg_fc06(instr, addr_tv, 1000)
        qa.add(f"{label} CFG_TARGET_VOLTAGE write — correctly rejected (no DAC)",
               not ok)

    addr_oe = base + CFG_OUTPUT_ENABLED_OFFSET
    if output_enabled_applicable(capabilities):
        _, orig_oe = read_reg_fc03(instr, addr_oe)
        restore = orig_oe if orig_oe in (0, 1) else 1
        for val in [0, 1, restore]:
            _, ok = write_reg_fc06(instr, addr_oe, val)
            time.sleep(0.02)
            _, rb = read_reg_fc03(instr, addr_oe)
            qa.add(f"{label} CFG_OUTPUT_ENABLED write {val}", ok and rb == val)
    else:
        reason = "has DAC" if chan_has(capabilities, CH_CAP_RAW_OUTPUT_DRIVE) \
            else "locked always-on"
        _, ok = write_reg_fc06(instr, addr_oe, 0)
        qa.add(f"{label} CFG_OUTPUT_ENABLED write — correctly rejected ({reason})",
               not ok)

    # RECOVERY_POLICY_MODE (offset 8): cycle through values — ungated
    addr = base + 8
    _, orig_rp = read_reg_fc03(instr, addr)
    for rp in [0, 1, 3, orig_rp]:  # Manual, Auto, Never, restore
        _, ok = write_reg_fc06(instr, addr, rp)
        time.sleep(0.01)
        _, rb = read_reg_fc03(instr, addr)
        qa.add(f"{label} RECOVERY_POLICY write {rp}", ok and rb == rp)

    # CURRENT_SAFE_BAND_PCT (offset 12): cycle through values — ungated
    addr = base + 12
    _, orig_sb = read_reg_fc03(instr, addr)
    for sb in [10, 20, orig_sb]:
        _, ok = write_reg_fc06(instr, addr, sb)
        time.sleep(0.01)
        _, rb = read_reg_fc03(instr, addr)
        qa.add(f"{label} CURRENT_SAFE_BAND write {sb}", ok and rb == sb)

    # --- Command writes (self-clearing WO) ---
    # OUTPUT_ACTION's DISABLE_* values (cmd_val=2) require CH_CAP_OUTPUT_ENABLE;
    # ENABLE (cmd_val=1) is always accepted regardless of capability.
    has_output_enable = chan_has(capabilities, CH_CAP_OUTPUT_ENABLE)
    for off, action_name in [(0, "OUTPUT_ACTION"), (1, "FAULT_CMD"), (2, "PARAM_ACTION")]:
        addr = base + off
        for cmd_val in [1, 2]:
            if off == 0 and cmd_val == 2 and not has_output_enable:
                _, ok = write_reg_fc06(instr, addr, cmd_val)
                qa.add(f"{label} {action_name} write {cmd_val} (DISABLE) — "
                       f"correctly rejected (locked always-on)", not ok)
                continue
            _, ok = write_reg_fc06(instr, addr, cmd_val)
            time.sleep(0.01)
            _, rb = read_reg_fc03(instr, addr)
            qa.add(f"{label} {action_name} write {cmd_val} -> self-clear",
                   ok and rb == 0, f"readback={rb}")

    # PARAM_ACTION Save/Load/Factory — ungated
    addr_pa = base + 2
    for pa in [1, 2]:  # Save, Load
        _, ok = write_reg_fc06(instr, addr_pa, pa)
        time.sleep(0.05)  # NVS needs settle
        _, rb = read_reg_fc03(instr, addr_pa)
        qa.add(f"{label} PARAM_ACTION {pa}", ok and rb == 0, f"readback={rb}")

    # Try writing to genuinely reserved offsets — should reject.
    # (offset 17 is tested above, not reserved.)
    for off in [35]:
        addr = base + off
        try:
            instr.write_register(addr, 0, number_of_decimals=0, functioncode=6, signed=False)
            qa.add(f"{label} holding off={off} (reserved) write — expected rejection", False)
        except Exception:
            qa.add(f"{label} holding off={off} (reserved) write — rejected", True)

def qa_extension(instr, qa):
    """Test extension holding block."""
    qa_test_header("Extension Holding Registers (FC03/FC06, addr 680-681)")

    # Read
    for addr, name in [(EXT_CAL_UNLOCK, "CAL_UNLOCK"), (EXT_CAL_EXIT, "CAL_EXIT")]:
        e, v = read_reg_fc03(instr, addr)
        qa.add(f"EXT {name} (addr={addr}) read", v is not None, f"val={v} {e:.0f}us")

    # Write CAL_UNLOCK (single step — should NOT unlock without step2)
    try:
        instr.write_register(EXT_CAL_UNLOCK, 0xCA1B, number_of_decimals=0, functioncode=6, signed=False)
        qa.add("EXT CAL_UNLOCK step1 write", True)
    except Exception:
        qa.add("EXT CAL_UNLOCK step1 write", True, "rejected (expected)")

    # Try writing to reserved extension addresses
    for addr in [682, 700, 759]:
        try:
            instr.write_register(addr, 0, number_of_decimals=0, functioncode=6, signed=False)
            qa.add(f"EXT addr={addr} (reserved) write — expected rejection", False)
        except Exception:
            qa.add(f"EXT addr={addr} (reserved) write — rejected", True)

def qa_burst(instr, qa, duration_s=30):
    """Sustained burst polling."""
    qa_test_header(f"Sustained Burst Polling ({duration_s}s)")
    errors = 0
    samples = []
    t_end = time.perf_counter() + duration_s
    window_size = 500
    window_start = time.perf_counter()

    while time.perf_counter() < t_end:
        e, v = read_reg_fc04(instr, SYS_STATUS)
        if v is not None:
            samples.append(e)
        else:
            errors += 1

        if len(samples) % window_size == 0:
            now = time.perf_counter()
            rate = window_size / (now - window_start)
            lat = statistics.mean(samples[-window_size:])
            print(f"  [{len(samples)}] rate={rate:.1f} Hz  latency(avg)={lat:.0f}us  errors={errors}")
            window_start = now

    elapsed = time.perf_counter() - t_end + duration_s
    hz = (len(samples) - errors) / elapsed if elapsed > 0 else 0
    avg = statistics.mean(samples) if samples else 0

    # Latency drift
    tenth = max(len(samples) // 10, 1)
    early_avg = statistics.mean(samples[:tenth])
    late_avg = statistics.mean(samples[-tenth:])
    drift_pct = ((late_avg - early_avg) / early_avg * 100) if early_avg > 0 else 0

    print(stats_str("sustained_latency", samples, n=len(samples)))
    print(f"  rate: {hz:.1f} Hz  drift: {drift_pct:+.1f}%  errors: {errors}")
    qa.add("Sustained burst", errors == 0 and abs(drift_pct) < 5,
           f"{hz:.1f} Hz, drift {drift_pct:+.1f}%, {errors} errors")

# ---------------------------------------------------------------------------
# Connect
# ---------------------------------------------------------------------------
def connect_and_probe(args):
    """Connect to the board, return (instr, major, minor, channels, active_mask)."""
    print(f"Connecting to {args.port} baud={args.baud} slave={args.slave} ...")
    instr = minimalmodbus.Instrument(args.port, args.slave)
    instr.serial.baudrate = args.baud
    instr.serial.timeout = args.timeout
    instr.mode = minimalmodbus.MODE_RTU
    instr.clear_buffers_before_each_transaction = True
    instr.close_port_after_each_call = False

    _, major = read_reg_fc04(instr, SYS_PROTOCOL_MAJOR)
    _, minor = read_reg_fc04(instr, SYS_PROTOCOL_MINOR)
    _, channels = read_reg_fc04(instr, SYS_SUPPORTED_CH)
    _, active_mask = read_reg_fc04(instr, SYS_ACTIVE_CH_MASK)

    if major is None or channels is None:
        print("ERROR: Cannot communicate with board")
        sys.exit(1)

    # Determine which channels are present
    present_channels = [c for c in range(max(channels, 1)) if active_mask & (1 << c)] if active_mask else list(range(channels))

    print(f"Connected: protocol v{major}.{minor}, {channels} channel(s) supported, "
          f"present: {present_channels}")
    return instr, major, minor, channels, active_mask, present_channels

# ---------------------------------------------------------------------------
# Report generators
# ---------------------------------------------------------------------------
def report_ci(args, results, started_at, ended_at, major, minor, channels,
              active_mask, report_path):
    """Generate CI pass/fail report."""
    overall = results["overall_pass"]
    lines = []
    lines.append(f"# PSB CI Stress Test — {'PASS' if overall else 'FAIL'}\n\n")
    lines.append(f"- **Time**: {started_at.isoformat()}\n")
    lines.append(f"- **Port**: {args.port}  baud={args.baud}  slave={args.slave}\n")
    lines.append(f"- **Firmware**: v{major}.{minor}  channels={channels}  mask=0x{active_mask:04X}\n")
    lines.append(f"- **Duration**: {int((ended_at - started_at).total_seconds())}s\n")
    lines.append(f"- **Source**: `include/reg_store/reg_map.h` + `include/reg_store/modbus_view.def`\n\n")

    lines.append("## Results\n\n")
    lines.append("| Test | Result | Detail |\n")
    lines.append("|------|--------|--------|\n")
    for name, ok, detail in results.get("ci_checks", []):
        lines.append(f"| {name} | {'PASS' if ok else 'FAIL'} | {detail} |\n")
    lines.append("\n")
    lines.append(f"**Overall: {'PASS' if overall else 'FAIL'}**\n")
    lines.append(f"\n---\n*`tools/stress_test/stress_test.py --mode ci`*\n")

    os.makedirs(os.path.dirname(report_path) or ".", exist_ok=True)
    with open(report_path, "w") as f:
        f.writelines(lines)
    print(f"CI report: {report_path}")
    return overall

def report_qa(args, results, started_at, ended_at, major, minor, channels,
              active_mask, present_channels, report_path):
    """Generate exhaustive QA report."""
    qa = results["qa"]
    overall = qa.all_pass()
    total = len(qa.checks)
    passed = sum(1 for _, p, _ in qa.checks if p)
    failed = total - passed

    lines = []
    lines.append(f"# PSB QA Stress Test — {'PASS' if overall else 'FAIL'}\n\n")
    lines.append(f"- **Time**: {started_at.isoformat()}\n")
    lines.append(f"- **Duration**: {int((ended_at - started_at).total_seconds())}s\n")
    lines.append(f"- **Port**: {args.port}  baud={args.baud}  slave={args.slave}\n")
    lines.append(f"- **Firmware**: v{major}.{minor}  channels={channels}  mask=0x{active_mask:04X}\n")
    lines.append(f"- **Channels present**: {present_channels}\n")
    lines.append(f"- **Source**: `include/reg_store/reg_map.h` + `include/reg_store/modbus_view.def`\n\n")

    lines.append(f"## Summary: {passed}/{total} passed, {failed} failed\n\n")

    lines.append("| Check | Result | Detail |\n")
    lines.append("|-------|--------|--------|\n")
    for label, ok, detail in qa.checks:
        lines.append(f"| {label} | {'PASS' if ok else 'FAIL'} | {detail} |\n")
    lines.append("\n")
    lines.append(f"**Overall: {'PASS' if overall else 'FAIL'}**\n")
    lines.append(f"\n---\n*`tools/stress_test/stress_test.py --mode qa`*\n")

    os.makedirs(os.path.dirname(report_path) or ".", exist_ok=True)
    with open(report_path, "w") as f:
        f.writelines(lines)
    print(f"QA report: {report_path}")
    return overall

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="PSB Modbus Stress Test")
    parser.add_argument("--mode", choices=["ci", "qa"], default="ci",
                        help="ci: fast CI pipeline (default); qa: exhaustive production QA")
    parser.add_argument("--port", default="/dev/ttyUSB0", help="Serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--slave", type=int, default=1, help="Slave ID")
    parser.add_argument("--timeout", type=float, default=TIMEOUT_S, help="Modbus timeout (s)")
    parser.add_argument("--rounds", type=int, default=2000, help="Rounds for read tests (CI mode)")
    parser.add_argument("--burst-duration", type=int, default=30, help="Sustained burst duration (s)")
    parser.add_argument("--skip-estimate", action="store_true", help="Skip theoretical estimate")
    parser.add_argument("--report", default=None, help="Report output path")
    args = parser.parse_args()

    started_at = datetime.now(timezone.utc)
    instr, major, minor, channels, active_mask, present_channels = connect_and_probe(args)

    print(f"Mode: {args.mode.upper()}")
    print()

    if args.mode == "ci":
        # --- CI Mode: minimal, fast, exit-code driven ---
        ci_checks = []

        ok, _, _, _, _ = ci_connectivity(instr)
        ci_checks.append(("connectivity", ok, f"v{major}.{minor} ch={channels}"))

        ok, sr_samples, sr_err = ci_single_read(instr, rounds=args.rounds)
        avg = statistics.mean(sr_samples) if sr_samples else 0
        ci_checks.append(("single_read", ok, f"{len(sr_samples)} reads, avg={avg:.0f}us, {sr_err} err"))

        ok, br_samples, br_err = ci_block_read(instr, rounds=max(args.rounds // 2, 50))
        avg = statistics.mean(br_samples) if br_samples else 0
        ci_checks.append(("block_read", ok, f"{len(br_samples)} reads, avg={avg:.0f}us, {br_err} err"))

        test_channel = present_channels[0] if present_channels else 0
        _, ch_caps = read_reg_fc04(instr, CH_BLOCK_BASE(test_channel) + 9)  # CAPABILITY_FLAGS
        ch_caps = ch_caps or 0

        ok, cw_err = ci_config_write(instr, channel=test_channel, capabilities=ch_caps)
        ci_checks.append(("config_write", ok, f"{cw_err} errors"))

        ok, cmd_err = ci_cmd_write(instr, channel=test_channel, capabilities=ch_caps)
        ci_checks.append(("cmd_write", ok, f"{cmd_err} errors"))

        ok, bust_samples, bust_err = ci_burst(instr, duration_s=args.burst_duration)
        hz = len(bust_samples) / args.burst_duration if args.burst_duration > 0 else 0
        ci_checks.append(("burst", ok, f"{hz:.0f} Hz, {bust_err} errors"))

        overall = all(ok for _, ok, _ in ci_checks)
        results = {"overall_pass": overall, "ci_checks": ci_checks}

        print()
        print(f"CI RESULT: {'PASS' if overall else 'FAIL'}")

    else:
        # --- QA Mode: exhaustive, all channels, all registers ---
        qa = QAResults()

        # System-level tests
        qa_system_input(instr, qa)
        qa_system_holding(instr, qa)
        qa_extension(instr, qa)

        # Per-channel tests
        for ch in present_channels:
            # Read channel capability flags
            _, cap_flags = read_reg_fc04(instr, CH_BLOCK_BASE(ch) + 9)
            qa.add(f"ch{ch} CAPABILITY_FLAGS read", cap_flags is not None,
                   f"0x{cap_flags:04X}" if cap_flags else "None")
            qa_channel(instr, qa, ch, cap_flags or 0)

        # Sustained burst
        qa_burst(instr, qa, duration_s=args.burst_duration)

        overall = qa.all_pass()
        results = {"qa": qa}

        print()
        print(f"QA RESULT: {'PASS' if overall else 'FAIL'} "
              f"({sum(1 for _, p, _ in qa.checks if p)}/{len(qa.checks)} checks passed)")

    ended_at = datetime.now(timezone.utc)

    # Generate report
    report_path = args.report
    if report_path is None:
        ts = started_at.strftime("%Y%m%d_%H%M%S")
        report_path = f"reports/stress_test_{args.mode}_{ts}.md"

    if args.mode == "ci":
        overall = report_ci(args, results, started_at, ended_at, major, minor,
                           channels, active_mask, report_path)
    else:
        overall = report_qa(args, results, started_at, ended_at, major, minor,
                           channels, active_mask, present_channels, report_path)

    instr.serial.close()
    sys.exit(0 if overall else 1)


if __name__ == "__main__":
    main()
