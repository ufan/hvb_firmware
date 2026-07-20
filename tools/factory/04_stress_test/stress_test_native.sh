#!/usr/bin/env bash
# stress_test_native.sh — HVB Board Modbus Stress Test (native CLI)
#
# Uses the project's native psb_demo_cli binary.
# Each test measures per-invocation blocking time via date +%s%N.
#
# Source of truth for register addresses:
#   include/reg_store/reg_map.h       (block layout, protocol constants)
#   include/reg_store/modbus_view.def (X-macro expanded register offsets)
#
# Register map (absolute Modbus addresses, 0-based):
#   SYS_BLOCK_BASE     = 0
#   CH_BLOCK_BASE(c)   = 40 + c * 40
#   EXT_BLOCK_BASE     = 680
#
# Compared to stress_test.py (minimalmodbus, persistent connection):
#   This script spawns a process per transaction, adding ~50ms overhead.
#
# Usage: ./stress_test_native.sh [--port /dev/ttyUSB0] [--rounds 500] [--burst 10]
set -euo pipefail

PORT="${PORT:-/dev/ttyUSB0}"
BAUD="${BAUD:-115200}"
SLAVE="${SLAVE:-1}"
TIMEOUT_MS=500
ROUNDS="${ROUNDS:-500}"
BURST_DURATION="${BURST_DURATION:-10}"
CLI="${CLI:-$(cd "$(dirname "$0")/../../.." && pwd)/tools/bin/psb_demo_cli}"
REPORT_DIR="${REPORT_DIR:-reports}"
TIMESTAMP=$(date -u +%Y%m%d_%H%M%S)
REPORT="${REPORT:-${REPORT_DIR}/stress_test_native_${TIMESTAMP}.md}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)   PORT="$2"; shift 2 ;;
        --baud)   BAUD="$2"; shift 2 ;;
        --slave)  SLAVE="$2"; shift 2 ;;
        --rounds) ROUNDS="$2"; shift 2 ;;
        --burst)  BURST_DURATION="$2"; shift 2 ;;
        --cli)    CLI="$2"; shift 2 ;;
        --report) REPORT="$2"; shift 2 ;;
        -*) echo "Unknown: $1"; exit 1 ;;
        *)  shift ;;
    esac
done

TMPDIR="${TMPDIR:-/tmp}"
TEMP_FILES=()

new_temp() { local f=$(mktemp "$TMPDIR/stress_native.XXXXXX"); TEMP_FILES+=("$f"); echo "$f"; }
cleanup() { rm -f "${TEMP_FILES[@]}" 2>/dev/null; }
trap cleanup EXIT

# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------
now_us() { echo $(( $(date +%s%N) / 1000 )); }  # microseconds since epoch
now_s()  { date +%s; }

cli() { "$CLI" -p "$PORT" -b "$BAUD" -i "$SLAVE" -t "$TIMEOUT_MS" "$@"; }

# Run a CLI command, print elapsed us to stdout, return exit code
timed_cli() {
    local st et rc
    st=$(now_us)
    cli "$@" >/dev/null 2>&1; rc=$?
    et=$(now_us)
    echo $(( et - st ))
    return $rc
}

# Print stats for samples in a file (one us value per line)
print_stats() {
    local name="$1" file="$2" label="$3"
    local -a vals=()
    while IFS= read -r v; do [[ -n "$v" ]] && vals+=("$v"); done < "$file"
    local n=${#vals[@]}
    if [[ $n -eq 0 ]]; then echo "  ${name}: [${label}] NO DATA"; return; fi

    local sorted=($(printf '%s\n' "${vals[@]}" | sort -n))
    local sum=0 avg p50 p99 mn=${sorted[0]} mx=${sorted[-1]}
    for v in "${sorted[@]}"; do sum=$((sum + v)); done
    avg=$((sum / n))

    local mid=$((n / 2))
    p50=$(( (sorted[mid] + sorted[mid-1]) / 2 ))

    local p99_idx=$((n * 99 / 100))
    [[ $p99_idx -ge $n ]] && p99_idx=$((n - 1))
    p99=${sorted[$p99_idx]}

    local hz
    if [[ $avg -gt 0 ]]; then
        hz=$(awk "BEGIN {printf \"%.1f\", 1000000 / $avg}")
    else hz="0"; fi

    echo "  ${name}: [${label}] avg=${avg}us p50=${p50}us p99=${p99}us min=${mn}us max=${mx}us → ${hz} Hz" >&2
}

# Compute average from file
file_avg() {
    local file="$1" sum=0 count=0
    while read -r v; do sum=$((sum + v)); count=$((count + 1)); done < "$file"
    [[ $count -gt 0 ]] && echo $((sum / count)) || echo 0
}

# ------------------------------------------------------------------
# Theoretical
# ------------------------------------------------------------------
print_theory() {
    cat << 'EOF'
============================================================
THEORETICAL ESTIMATION (wire timing only, no firmware overhead)
============================================================
  Baud rate:          115200
  Byte time:          86.8 us
  t3.5 inter-frame:   303.8 us
  MB_CMD_TIMEOUT:     1.0 s (firmware worst-case write)

  Single reg read:    ~1910 us wire → max ~524 Hz
  Block read (20reg): ~5295 us wire → max ~189 Hz
  Single reg write:   ~1997 us wire (echo transaction)

  2ch single rd:      max ~262 Hz/ch (sequential)
  2ch block rd:       max ~94 Hz/ch (sequential)

EOF
}

# ------------------------------------------------------------------
# Connectivity
# ------------------------------------------------------------------
check_connect() {
    if ! cli info >/dev/null 2>&1; then
        echo "ERROR: Cannot communicate with board at $PORT" >&2
        exit 1
    fi
    local info
    info=$(cli info 2>/dev/null)
    PROTO_MAJOR=$(echo "$info" | sed -n 's/.*Protocol: *\([0-9]*\)\..*/\1/p')
    PROTO_MINOR=$(echo "$info" | sed -n 's/.*Protocol: *[0-9]*\.\([0-9]*\).*/\1/p')
    CHANNELS=$(echo "$info" | sed -n 's/.*Channels: *\([0-9]*\).*/\1/p')
    echo "Connected: protocol v${PROTO_MAJOR}.${PROTO_MINOR}, ${CHANNELS} channel(s)"
    echo "" >&2
}

# ------------------------------------------------------------------
# Test 1: Single register polling read
# ------------------------------------------------------------------
test_single_read() {
    local rounds=$1 outfile
    outfile=$(new_temp)
    local errors=0 t0
    t0=$(now_s)

    echo "TEST 1: POLLING READ — SINGLE REGISTER (FC04, addr=13)" >&2
    echo "------------------------------------------------------------" >&2

    for ((i=1; i<=rounds; i++)); do
        local elapsed rc
        elapsed=$(timed_cli raw fc04 13 1) || { errors=$((errors+1)); continue; }
        echo "$elapsed" >> "$outfile"

        if (( i % 200 == 0 )); then
            local dt=$(( $(now_s) - t0 ))
            local rate=0
            [[ $dt -gt 0 ]] && rate=$(awk "BEGIN {printf \"%.1f\", ($i - $errors) / $dt}")
            echo "  [$i/$rounds] rate=${rate} Hz  errors=$errors" >&2
        fi
    done

    local dt=$(( $(now_s) - t0 ))
    local hz=0
    [[ $dt -gt 0 ]] && hz=$(awk "BEGIN {printf \"%.1f\", ($rounds - $errors) / $dt}")

    print_stats "blocking_time" "$outfile" "n=$rounds"
    echo "  Throughput: ${hz} Hz (${dt}s for $((rounds-errors)) reads, $errors errors)" >&2
    echo "" >&2

    echo "$outfile|$hz|$errors|$rounds"
}

# ------------------------------------------------------------------
# Test 2: Block read
# ------------------------------------------------------------------
test_block_read() {
    local rounds=$1 start_addr=$2 reg_count=$3 label=$4 fc=${5:-fc04}
    local outfile
    outfile=$(new_temp)
    local errors=0 t0
    t0=$(now_s)

    echo "TEST 2: POLLING READ — BLOCK ($reg_count regs, $fc, $label)" >&2
    echo "------------------------------------------------------------" >&2

    for ((i=1; i<=rounds; i++)); do
        local elapsed rc
        elapsed=$(timed_cli raw "$fc" "$start_addr" "$reg_count") || { errors=$((errors+1)); continue; }
        echo "$elapsed" >> "$outfile"

        if (( i % 200 == 0 )); then
            local dt=$(( $(now_s) - t0 ))
            local rate=0
            [[ $dt -gt 0 ]] && rate=$(awk "BEGIN {printf \"%.1f\", ($i - $errors) / $dt}")
            echo "  [$i/$rounds] rate=${rate} Hz  errors=$errors" >&2
        fi
    done

    local dt=$(( $(now_s) - t0 ))
    local hz=0
    [[ $dt -gt 0 ]] && hz=$(awk "BEGIN {printf \"%.1f\", ($rounds - $errors) / $dt}")

    print_stats "blocking_time" "$outfile" "n=$rounds"
    echo "  Throughput: ${hz} Hz" >&2
    echo "" >&2

    echo "$outfile|$hz|$errors|$rounds"
}

# ------------------------------------------------------------------
# Test 3: Config write (target voltage)
# ------------------------------------------------------------------
test_config_write() {
    local outfile
    outfile=$(new_temp)
    local errors=0
    local test_values=(0 1000 2000 5000 10000 0)

    echo "TEST 3: CONFIG WRITE (FC06, CFG_TARGET_VOLTAGE addr=43)" >&2
    echo "------------------------------------------------------------" >&2

    # Read initial value
    local initial
    initial=$(cli channel 0 config 2>/dev/null | grep "Configured Target:" | grep -oP '\d+(?= LSB)' || echo "0")
    initial=$((initial))
    echo "  Initial value: ${initial} mV" >&2

    for val in "${test_values[@]}"; do
        local elapsed
        elapsed=$(timed_cli raw fc06 43 "$val")
        echo "$elapsed" >> "$outfile"

        sleep 0.02

        # Readback verify
        local rb
        rb=$(cli channel 0 config 2>/dev/null | grep "Configured Target:" | grep -oP '\d+(?= LSB)' || echo "0")
        rb=$((rb))
        if [[ "$rb" != "$val" ]]; then
            echo "  VERIFY FAIL: wrote $val, read $rb"
            errors=$((errors+1))
        fi
    done

    print_stats "write_time" "$outfile" "n=${#test_values[@]}"
    echo "  Verify errors: $errors" >&2
    echo "" >&2

    echo "$outfile|$errors"
}

# ------------------------------------------------------------------
# Test 4: Cmd write
# ------------------------------------------------------------------
test_cmd_write() {
    local oa_file fc_file pa_file
    oa_file=$(new_temp)
    fc_file=$(new_temp)
    pa_file=$(new_temp)

    echo "TEST 4: CMD WRITE (FC06, self-clearing WO registers)" >&2
    echo "------------------------------------------------------------" >&2

    # 4a: OUTPUT_ACTION
    echo "  4a: OUTPUT_ACTION (addr=40) — Enable(1)" >&2
    for ((i=0; i<20; i++)); do
        local elapsed
        elapsed=$(timed_cli raw fc06 40 1)
        echo "$elapsed" >> "$oa_file"
        sleep 0.01
    done
    print_stats "OUTPUT_ACTION" "$oa_file" "n=20"

    # 4b: FAULT_CMD
    echo "  4b: FAULT_CMD (addr=41) — Clear Active(1)" >&2
    for ((i=0; i<20; i++)); do
        local elapsed
        elapsed=$(timed_cli raw fc06 41 1)
        echo "$elapsed" >> "$fc_file"
        sleep 0.01
    done
    print_stats "FAULT_CMD" "$fc_file" "n=20"

    # 4c: PARAM_ACTION
    echo "  4c: PARAM_ACTION (addr=42) — Save/Load/Factory (NVS ops)" >&2
    local actions=(1 2 3 1 2 3 1 2 3 1 2 3 1 2 3)
    for act in "${actions[@]}"; do
        local elapsed
        elapsed=$(timed_cli raw fc06 42 "$act")
        echo "$elapsed" >> "$pa_file"
        sleep 0.05
    done
    print_stats "PARAM_ACTION" "$pa_file" "n=15"
    echo "" >&2
}

# ------------------------------------------------------------------
# Test 5: Sustained burst
# ------------------------------------------------------------------
test_sustained_burst() {
    local duration_s=$1 outfile
    outfile=$(new_temp)
    local errors=0 count=0 t0 deadline
    t0=$(now_s)
    deadline=$((t0 + duration_s))

    echo "TEST 5: SUSTAINED BURST POLLING (${duration_s}s, single reg read)" >&2
    echo "------------------------------------------------------------" >&2

    while true; do
        local now
        now=$(now_s)
        [[ $now -ge $deadline ]] && break

        local elapsed rc
        elapsed=$(timed_cli raw fc04 13 1) || { errors=$((errors+1)); continue; }
        echo "$elapsed" >> "$outfile"
        count=$((count+1))

        if (( count % 100 == 0 )); then
            local dt=$((now - t0))
            local rate=0
            [[ $dt -gt 0 ]] && rate=$(awk "BEGIN {printf \"%.1f\", $count / $dt}")
            echo "  [$count] rate=${rate} Hz  errors=$errors" >&2
        fi
    done

    local dt=$(( $(now_s) - t0 ))
    local hz=0
    [[ $dt -gt 0 ]] && hz=$(awk "BEGIN {printf \"%.1f\", ($count - $errors) / $dt}")

    print_stats "sustained_latency" "$outfile" "n=$count"
    echo "  Sustained rate: ${hz} Hz over ${dt}s ($errors errors)" >&2
    echo "" >&2

    echo "$outfile|$hz|$errors|$count"
}

# ------------------------------------------------------------------
# Report
# ------------------------------------------------------------------
generate_report() {
    local sr_file sr_hz sr_avg sr_err
    local br_ch0_file br_ch0_hz br_sys_file br_sys_hz
    local cw_file cw_err cw_avg
    local sb_file sb_hz sb_avg

    IFS='|' read -r sr_file sr_hz sr_err _ <<< "$SR_RESULT"
    IFS='|' read -r br_ch0_file br_ch0_hz _ _ <<< "$BR_CH0_RESULT"
    IFS='|' read -r br_sys_file br_sys_hz _ _ <<< "$BR_SYS_RESULT"
    IFS='|' read -r cw_file cw_err <<< "$CW_RESULT"
    IFS='|' read -r sb_file sb_hz _ _ <<< "$SB_RESULT"

    sr_avg=$(file_avg "$sr_file")
    cw_avg=$(file_avg "$cw_file")
    sb_avg=$(file_avg "$sb_file")

    mkdir -p "$(dirname "$REPORT")"

    cat > "$REPORT" << EOF
# HVB Board Modbus Stress Test Report (Native CLI)

## Metadata
- **Generated**: $(date -u -Iseconds)
- **Tool**: \`psb_demo_cli\` (\`$CLI\`)
- **Port**: $PORT
- **Baud rate**: $BAUD
- **Slave ID**: $SLAVE
- **Firmware**: protocol v${PROTO_MAJOR}.${PROTO_MINOR}, ${CHANNELS} channel(s)
- **Test rounds (reads)**: $ROUNDS
- **Burst duration**: ${BURST_DURATION} s
- **Report path**: $REPORT

## Source of Truth
- \`include/reg_store/reg_map.h\` — block layout, protocol constants
- \`include/reg_store/modbus_view.def\` — X-macro expanded register offsets

## Engine Comparison

| Aspect | \`stress_test.py\` | \`stress_test_native.sh\` |
|---|---|---|
| Modbus library | \`minimalmodbus\` (Python) | \`psb_demo_cli\` → \`libmodbus\` (C) |
| Connection | Persistent (one serial open) | Per-invocation (connects on each call) |
| Process overhead | None (~0us) | ~50ms (binary load + connect + exit) |
| Per-call latency | ~6.7ms | ~60ms |
| Use case | High-freq polling, latency measurement | Smoke testing, one-shot commands |

## Results

### Test 1 — Single Register Polling Read (FC04 addr=13)
Rounds: $ROUNDS, errors: $sr_err

| Metric | Value |
|---|---|
| Blocking time (avg) | ${sr_avg} us |
| Throughput | ${sr_hz} Hz |

### Test 2 — Block Polling Read

#### ch0 input (10 reg, addr=40-49)
| Metric | Value |
|---|---|
| Blocking time (avg) | $(file_avg "$br_ch0_file") us |
| Throughput | ${br_ch0_hz} Hz |

#### sys input (15 reg, addr=0-14)
| Metric | Value |
|---|---|
| Blocking time (avg) | $(file_avg "$br_sys_file") us |
| Throughput | ${br_sys_hz} Hz |

### Test 3 — Config Write (FC06, CFG_TARGET_VOLTAGE)
Verify errors: $cw_err

| Metric | Value |
|---|---|
| Write time (avg) | ${cw_avg} us |

### Test 5 — Sustained Burst (${BURST_DURATION}s)

| Metric | Value |
|---|---|
| Latency (avg) | ${sb_avg} us |
| Throughput | ${sb_hz} Hz |

## Recommendation

The native CLI adds ~50ms process-spawn overhead per Modbus transaction.
For high-frequency polling (>10 Hz), use a persistent connection approach
(Python \`stress_test.py\` or directly use \`PsbModbusClient\` C++ library).
The native CLI is suitable for one-shot config/cmd writes and low-frequency monitoring.

---
*Report generated by \`tools/stress_test/stress_test_native.sh\` on $(date -u -Iseconds)*
EOF

    echo "Report saved: $REPORT"
}

# ==================================================================
# Main
# ==================================================================
main() {
    echo "=== HVB Board Modbus Stress Test (Native CLI) ==="
    echo "CLI: $CLI"
    echo "Port: $PORT  Baud: $BAUD  Slave: $SLAVE"
    echo "Rounds: $ROUNDS  Burst: ${BURST_DURATION}s"
    echo "" >&2

    print_theory
    check_connect

    SR_RESULT=$(test_single_read "$ROUNDS")
    BR_CH0_RESULT=$(test_block_read "$((ROUNDS / 2))" 40 10 "ch0 input addr=40-49" fc04)
    BR_SYS_RESULT=$(test_block_read "$((ROUNDS / 2))" 0 15 "sys input addr=0-14" fc04)
    CW_RESULT=$(test_config_write)
    test_cmd_write
    SB_RESULT=$(test_sustained_burst "$BURST_DURATION")

    generate_report
    echo "" >&2
    echo "Stress test complete."
}

main
