#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="/dev/ttyUSB0"
BAUD=115200
SLAVE=1
TIMEOUT_MS=3000
CLI="$SCRIPT_DIR/../bin/hvb_demo_cli"
REPORT=""

SWEEP_CODES=(0 10000 20000 30000 40000 50000 60000)
SETTLE_SECONDS=5
SAMPLE_DELAY_SECONDS=0.1

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]
  --port PATH       Serial port (default: /dev/ttyUSB0)
  --baud RATE       Baud rate (default: 115200)
  --slave ID        Modbus slave ID (default: 1)
  --timeout MS      CLI timeout in milliseconds (default: 3000)
  --cli PATH        hvb_demo_cli executable
  --report PATH     Markdown report path
  -h, --help        Show this help
EOF
}

while (($#)); do
    case "$1" in
    --port) PORT="$2"; shift 2 ;;
    --baud) BAUD="$2"; shift 2 ;;
    --slave) SLAVE="$2"; shift 2 ;;
    --timeout) TIMEOUT_MS="$2"; shift 2 ;;
    --cli) CLI="$2"; shift 2 ;;
    --report) REPORT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$REPORT" ]]; then
    REPORT="$SCRIPT_DIR/reports/dac_sweep_$(date -u +%Y%m%d_%H%M%S).md"
fi
mkdir -p "$(dirname "$REPORT")"
BODY="$(mktemp)"

CAL_ENTERED=0
CLEANUP_OK=1
ERROR_MESSAGE=""
DAC_CHANNELS=()
CHANNEL_CAPS=()
WORDS=()

cli_raw() {
    "$CLI" -p "$PORT" -b "$BAUD" -i "$SLAVE" -t "$TIMEOUT_MS" raw "$@"
}

write_reg() {
    cli_raw fc06 "$1" "$2" >/dev/null
}

read_regs() {
    local addr=$1 count=$2 output token
    if ! output="$(cli_raw fc04 "$addr" "$count")"; then
        return 1
    fi
    WORDS=()
    while IFS= read -r token; do
        [[ -z "$token" ]] && continue
        if [[ ! "$token" =~ ^[[:xdigit:]]{4}$ ]]; then
            echo "Invalid CLI register token: $token" >&2
            return 1
        fi
        WORDS+=("${token^^}")
    done < <(printf '%s\n' "$output" | tr '[:space:]' '\n')
    if ((${#WORDS[@]} != count)); then
        echo "Expected $count registers at address $addr, got ${#WORDS[@]}" >&2
        return 1
    fi
}

unsigned16() {
    echo "$((16#$1))"
}

signed16() {
    local value=$((16#$1))
    ((value >= 0x8000)) && value=$((value - 0x10000))
    echo "$value"
}

signed32() {
    local value=$(((16#$1 << 16) | 16#$2))
    ((value >= 0x80000000)) && value=$((value - 0x100000000))
    echo "$value"
}

volts_from_raw() {
    awk -v raw="$1" 'BEGIN { printf "%.1f", raw * 0.1 }'
}

fail() {
    ERROR_MESSAGE="$*"
    echo "ERROR: $*" >&2
    exit 1
}

render_report() {
    local result=$1 cleanup_result=$2 temp
    temp="${REPORT}.tmp"
    {
        echo "# DAC Sweep Test Report — $result"
        echo
        cat "$BODY"
        if [[ -n "$ERROR_MESSAGE" ]]; then
            echo
            echo "## Error"
            echo
            echo "$ERROR_MESSAGE"
        fi
        echo
        echo "## Cleanup"
        echo
        echo "Cleanup result: **$cleanup_result**"
        echo
        echo "Overall result: **$result**"
    } > "$temp"
    mv "$temp" "$REPORT"
}

finish() {
    local original_rc=$? ch cleanup_result="PASS" result="PASS"
    trap - EXIT INT TERM
    set +e

    if ((CAL_ENTERED)); then
        for ch in "${DAC_CHANNELS[@]}"; do
            write_reg "$((40 + ch * 40 + 31))" 0 || CLEANUP_OK=0
            write_reg "$((40 + ch * 40 + 30))" 0 || CLEANUP_OK=0
        done
        write_reg 681 1 || CLEANUP_OK=0
    fi

    ((CLEANUP_OK)) || cleanup_result="FAIL"
    if ((original_rc != 0 || CLEANUP_OK == 0)); then
        result="FAIL"
    fi
    render_report "$result" "$cleanup_result"
    rm -f "$BODY"
    echo "Report: $REPORT"

    if [[ "$result" == "PASS" ]]; then
        exit 0
    fi
    ((original_rc != 0)) && exit "$original_rc"
    exit 1
}

trap finish EXIT
trap 'ERROR_MESSAGE="Interrupted by SIGINT"; exit 130' INT
trap 'ERROR_MESSAGE="Interrupted by SIGTERM"; exit 143' TERM

[[ -x "$CLI" ]] || fail "CLI is not executable: $CLI"

read_regs 0 15 || fail "Unable to read system input registers"
protocol_major=$(unsigned16 "${WORDS[0]}")
protocol_minor=$(unsigned16 "${WORDS[1]}")
variant_id=$(unsigned16 "${WORDS[2]}")
system_caps=$(unsigned16 "${WORDS[3]}")
channel_count=$(unsigned16 "${WORDS[4]}")
firmware_hex="0x${WORDS[10]}${WORDS[11]}"

((protocol_major == 3)) || fail "Unsupported protocol $protocol_major.$protocol_minor; v3 required"
((system_caps & 0x0004)) || fail "Board does not advertise Calibration Mode capability"
((channel_count >= 1 && channel_count <= 16)) || fail "Invalid supported channel count: $channel_count"

{
    echo "- **Time**: $(date -u -Iseconds)"
    echo "- **Port**: $PORT"
    echo "- **Baud / slave / timeout**: $BAUD / $SLAVE / ${TIMEOUT_MS} ms"
    echo "- **Protocol**: $protocol_major.$protocol_minor"
    echo "- **Variant**: $variant_id"
    echo "- **Firmware**: $firmware_hex"
    echo "- **Supported channels**: $channel_count"
    echo "- **Sweep**: 0–60000, step 10000, settle ${SETTLE_SECONDS}s"
    echo
} >> "$BODY"

for ((ch = 0; ch < channel_count; ch++)); do
    base=$((40 + ch * 40))
    read_regs "$((base + 9))" 1 || fail "Unable to read CH$ch capability flags"
    caps=$(unsigned16 "${WORDS[0]}")
    printf -v cap_hex '0x%04X' "$caps"
    CHANNEL_CAPS[ch]=$caps
    echo "CH$ch capabilities: \`$cap_hex\`" >> "$BODY"
    echo >> "$BODY"
    if ((caps & 0x0002)); then
        DAC_CHANNELS+=("$ch")
    else
        echo "CH$ch skipped: missing RAW_OUTPUT_DRIVE capability." >> "$BODY"
        echo >> "$BODY"
    fi
done

((${#DAC_CHANNELS[@]} > 0)) || fail "No channel supports RAW_OUTPUT_DRIVE"

write_reg 680 51739 || fail "Calibration unlock step 1 failed"
write_reg 680 41243 || fail "Calibration unlock step 2 failed"
write_reg 0 2 || fail "Unable to enter Calibration Mode"
CAL_ENTERED=1

for ch in "${DAC_CHANNELS[@]}"; do
    base=$((40 + ch * 40))
    caps=${CHANNEL_CAPS[ch]}
    has_v=$(( (caps & 0x0004) != 0 ))
    has_i=$(( (caps & 0x0008) != 0 ))

    echo "Sweeping CH$ch..."
    {
        echo "## Channel $ch"
        echo
        if ((has_v == 0 && has_i == 0)); then
            echo "CH$ch measurement capability: none; measurement columns are N/A."
            echo
        fi
        echo "| DAC | Raw ADC V | Raw ADC I | Measured V raw | Measured V (V) | Measured I raw | Measured I (nA) |"
        echo "|---:|---:|---:|---:|---:|---:|---:|"
    } >> "$BODY"

    write_reg "$((base + 30))" 1 || fail "CH$ch calibration output enable failed"

    for dac in "${SWEEP_CODES[@]}"; do
        write_reg "$((base + 31))" "$dac" || fail "CH$ch DAC write failed at $dac"
        sleep "$SETTLE_SECONDS"

        raw_v="N/A" raw_i="N/A"
        measured_v_raw="N/A" measured_v="N/A"
        measured_i_raw="N/A" measured_i="N/A"

        if ((has_v || has_i)); then
            write_reg "$((base + 32))" 1 || fail "CH$ch sample command failed at DAC $dac"
            sleep "$SAMPLE_DELAY_SECONDS"
        fi
        if ((has_v)); then
            read_regs "$((base + 10))" 1 || fail "CH$ch measured voltage read failed"
            measured_v_raw=$(signed16 "${WORDS[0]}")
            measured_v=$(volts_from_raw "$measured_v_raw")
            read_regs "$((base + 12))" 2 || fail "CH$ch raw voltage ADC read failed"
            raw_v=$(signed32 "${WORDS[0]}" "${WORDS[1]}")
        fi
        if ((has_i)); then
            read_regs "$((base + 11))" 1 || fail "CH$ch measured current read failed"
            measured_i_raw=$(signed16 "${WORDS[0]}")
            measured_i=$measured_i_raw
            read_regs "$((base + 14))" 2 || fail "CH$ch raw current ADC read failed"
            raw_i=$(signed32 "${WORDS[0]}" "${WORDS[1]}")
        fi

        echo "| $dac | $raw_v | $raw_i | $measured_v_raw | $measured_v | $measured_i_raw | $measured_i |" >> "$BODY"
    done

    write_reg "$((base + 31))" 0 || fail "CH$ch DAC zero failed"
    write_reg "$((base + 30))" 0 || fail "CH$ch calibration output disable failed"
    echo >> "$BODY"
done
