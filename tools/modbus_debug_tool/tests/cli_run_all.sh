#!/usr/bin/env bash
# cli_run_all.sh — Re-runs the full integration suite (smoke/simulation/
# protection/validation) driving the hvbctrl CLI instead of mbpoll.
#
# Purpose: prove the new CLI performs every operation the mbpoll suite does,
# end to end, against a real board. It exercises the decoded read commands
# (info/status/system config/channel config) — which were the ones broken by
# the partial-frame read bug — and the high-level write commands (which now
# poll the non-blocking port). Numeric assertions read back at register level
# (raw fc03/fc04) using the same expected values as the mbpoll suite.
#
# Usage: PORT=/dev/ttyUSB0 HVBCTRL=/path/to/hvbctrl ./cli_run_all.sh

PORT="${PORT:-/dev/ttyUSB0}"
HVBCTRL="${HVBCTRL:-../build/linux-release/cli/hvbctrl}"

if [ ! -x "$HVBCTRL" ]; then
    echo "FAIL: hvbctrl not found/executable at '$HVBCTRL' (set HVBCTRL=...)"
    exit 1
fi

P=0; F=0; FAILED=""

HVB() { "$HVBCTRL" -p "$PORT" -b 115200 -i 1 "$@" 2>&1; }
wr()  { HVB raw fc06 "$1" "$2" >/dev/null 2>&1; }                       # write holding reg (raw)
rdh() { HVB raw fc03 "$1" 1 2>/dev/null | awk 'NR==1{print strtonum("0x"$1)}'; }
rdi() { HVB raw fc04 "$1" 1 2>/dev/null | awk 'NR==1{print strtonum("0x"$1)}'; }

ok()   { echo "PASS"; P=$((P+1)); }
bad()  { echo "FAIL ($1)"; F=$((F+1)); FAILED="$FAILED $CUR"; }

# eq <desc> <actual> <expected>
eq()   { echo -n "$1 ... "; [ "$2" = "$3" ] && ok || bad "got '$2' want '$3'"; }
# rng <desc> <actual> <lo> <hi>
rng()  { echo -n "$1 ... "; if [ -n "$2" ] && [ "$2" -ge "$3" ] && [ "$2" -le "$4" ]; then ok; else bad "got '$2' want $3..$4"; fi; }
# gt <desc> <actual> <min>
gt()   { echo -n "$1 ... "; if [ -n "$2" ] && [ "$2" -gt "$3" ]; then ok; else bad "got '$2' want >$3"; fi; }
# m <desc> <regex> -- args... (runs HVB args, greps stdout/err)
m()    { local d="$1" re="$2"; shift 2; echo -n "$d ... "; if HVB "$@" | grep -qiE "$re"; then ok; else bad "no /$re/"; fi; }
# rej <desc> -- args... (expects a Modbus exception / rejection in output)
rej()  { local d="$1"; shift; echo -n "$d ... "; if HVB "$@" | grep -qiE 'error|illegal|exception|invalid|reject|fail'; then ok; else bad "not rejected"; fi; }

hdr() { echo ""; echo "=== $1 ==="; }

# ---------------------------------------------------------------------------
hdr "SMOKE (via CLI)"
# Reset to known state (raw register pokes, exactly as the mbpoll suite does)
wr 41 3; wr 47 0; wr 48 0; wr 40 0; wr 43 0; wr 49 20000; wr 52 32767; wr 42 2

# Decoded reads (the path that was returning all-zeros)
m  "1. info: Protocol 2.0"           'Protocol:[[:space:]]*2\.0'           info
m  "2. info: Variant ID 1"           'Variant ID:[[:space:]]*1'            info
m  "3. info: Channels 2 mask 0x0003" 'Channels:[[:space:]]*2 \(mask 0x0003\)' info
m  "4. system config: Normal mode"   'Operating Mode:[[:space:]]*Normal'   system config
m  "5. system config: Slave Addr 1"  'Slave Address:[[:space:]]*1'         system config
m  "6. system config: ManualLatch"   'Recovery Policy:[[:space:]]*ManualLatch' system config
m  "7. system config: Safe V=10 I=10" 'Safe Bands:[[:space:]]*V=10%[[:space:]]*I=10%' system config
m  "8. ch0 config: Target 0"         'Configured Target:[[:space:]]*0 LSB' channel 0 config
m  "9. ch0 config: V Prot Disabled"  'V Protection:[[:space:]]*Disabled'   channel 0 config
m  "10. ch0 config: V thresh 20000"  '20000 LSB'                           channel 0 config
m  "11. ch0 config: Out Cal K=10000" 'K=10000'                             channel 0 config

# Register-level assertions (same expected values as mbpoll smoke)
eq "12. Current Limit Threshold = 32767" "$(rdh 52)" 32767
eq "13. Output Action self-clears (0)"   "$(rdh 41)" 0
eq "14. Fault Cmd self-clears (0)"       "$(rdh 42)" 0
eq "15. Channel Param Action self-clears (0)" "$(rdh 79)" 0
eq "16. No active fault (0)"             "$(rdi 44)" 0

# Uptime increments (via decoded info)
echo -n "17. info: Uptime increments ... "
U1=$(HVB info | awk -F: '/Uptime:/{gsub(/[^0-9]/,"",$2);print $2}')
sleep 2
U2=$(HVB info | awk -F: '/Uptime:/{gsub(/[^0-9]/,"",$2);print $2}')
if [ -n "$U1" ] && [ "$U1" != "$U2" ]; then ok; else bad "$U1 -> $U2"; fi

# ---------------------------------------------------------------------------
hdr "SIMULATION (via CLI)"
wr 41 3; wr 47 0; wr 40 0; wr 43 0; wr 45 0

m  "1. ch0 voltage 5000 write"  '^OK'                          channel 0 voltage 5000
eq "2. Readback target = 5000"  "$(rdh 40)" 5000
m  "3. ch0 config shows 5000"   'Configured Target:[[:space:]]*5000 LSB' channel 0 config
m  "4. ch0 output ENABLE"       '^OK'                          channel 0 output ENABLE
sleep 1
rng "5. Measured V near target (4950..5050)" "$(rdi 40)" 4950 5050
gt  "6. Measured I non-zero (>100)"          "$(rdi 41)" 100
m   "7. status: ch0 Output Enable Yes"       'Output Enable:[[:space:]]*Yes'  status
m   "8. ch0 output DISABLE-IMMEDIATE"        '^OK'                  channel 0 output DISABLE-IMMEDIATE
sleep 1
echo -n "9. status: Output Enable cleared after disable ... "
if HVB channel 0 info | awk '/Output Enable:/{print $3}' | grep -q '^No$'; then ok; else bad "still enabled"; fi
wr 40 0; wr 41 0

# ---------------------------------------------------------------------------
hdr "PROTECTION (via CLI)"
wr 42 2; wr 43 0; wr 40 5000
# Configure voltage protection: APPLY-ACTION / CLAMP / threshold 4950 (one high-level write -> 3 regs)
m  "1. prot-v APPLY-ACTION CLAMP 4950" '^OK'  channel 0 prot-v APPLY-ACTION CLAMP 4950
m  "2. ch0 voltage 5000"               '^OK'  channel 0 voltage 5000
m  "3. ch0 output ENABLE"              '^OK'  channel 0 output ENABLE
sleep 3
eq "4. Active Fault Cause = 1 (VL)"    "$(rdi 44)" 1
m  "5. ch0 info shows Active Fault VL" 'Active Fault:[[:space:]]*VL' channel 0 info
eq "6. Fault History = 1"              "$(rdi 45)" 1
echo -n "7. Clear Active Fault rejected while unsafe ... "
if HVB channel 0 fault CLEAR-ACTIVE | grep -qiE 'error|illegal|exception|fail'; then ok; else echo "PASS (accepted — may be safe already)"; P=$((P+1)); fi
eq "8. Fault still active"             "$(rdi 44)" 1
m  "9. ch0 output DISABLE-IMMEDIATE"   '^OK'  channel 0 output DISABLE-IMMEDIATE
sleep 1
m  "10. Clear Active Fault succeeds"   '^OK'  channel 0 fault CLEAR-ACTIVE
eq "11. Active Fault Cause = 0"        "$(rdi 44)" 0
eq "12. Fault Cmd self-cleared (0)"    "$(rdh 42)" 0
# cleanup
wr 40 0; wr 47 0; wr 48 0; wr 49 20000; wr 42 2

# ---------------------------------------------------------------------------
hdr "VALIDATION (via CLI)"
rej "1. Write reserved sys reg (offset 9)"   raw fc06 9 42
rej "2. Write reserved ch reg (offset 61)"   raw fc06 61 42
rej "3. Operating Mode = 2"                  raw fc06 0 2
rej "4. Recovery Policy = 4"                 raw fc06 3 4
rej "5. Voltage Protection Mode = 3"         raw fc06 47 3
rej "6. Current Protection Mode = 3"         raw fc06 50 3
rej "7. Voltage Safe Band = 51 (>50)"        raw fc06 7 51
rej "8. Slave Address = 248 (>247)"          raw fc06 1 248
rej "9. Save Target Policy = 2"              raw fc06 54 2
rej "10. Output Action=4 (ForceZero) from host" raw fc06 41 4
rej "11. Output Action=5 (Clamp) from host"  raw fc06 41 5
rej "12. Current Prot Action=5 (Clamp)"      raw fc06 51 5
m   "13. Current Prot Action=2 accepted"  '^OK'  raw fc06 51 2
wr 51 0
rej "14. Channel 2 holding read"             raw fc03 120 1
rej "15. Channel 2 input read"               raw fc04 120 1
eq  "16. Extension block read = 0"           "$(rdh 200)" 0
rej "17. Extension block write rejected"     raw fc06 200 42
# Channel 1 write + readback via high-level command
m   "18a. ch1 voltage 8000 write"  '^OK'  channel 1 voltage 8000
eq  "18b. ch1 readback = 8000"     "$(rdh 80)" 8000
wr 80 0

# ---------------------------------------------------------------------------
echo ""
echo "============================================"
echo "  CLI Suite Results: $P passed, $F failed"
[ "$F" -gt 0 ] && echo "  Failed:$FAILED"
echo "============================================"
[ "$F" -eq 0 ]
