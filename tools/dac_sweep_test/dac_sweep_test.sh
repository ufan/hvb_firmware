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
TEMP_FILES=("$BODY")

CAL_ENTERED=0
CLEANUP_OK=1
ERROR_MESSAGE=""
DAC_CHANNELS=()
CHANNEL_CAPS=()
WORDS=()
declare -A FIT_SLOPES FIT_INTERCEPTS FIT_R2

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

calculate_linearity_fit() {
    local points_file=$1
    local values
    values="$(awk '
        {
            n++
            x[n] = $1
            y[n] = $2
            sx += $1
            sy += $2
            sxx += $1 * $1
            sxy += $1 * $2
            if (n == 1 || $1 < xmin) xmin = $1
            if (n == 1 || $1 > xmax) xmax = $1
        }
        END {
            denominator = n * sxx - sx * sx
            if (n < 2 || denominator == 0) {
                printf "N/A\tN/A\tN/A\tN/A\tN/A\t%d\n", n
                exit
            }

            slope = (n * sxy - sx * sy) / denominator
            intercept = (sy - slope * sx) / n
            mean = sy / n
            for (i = 1; i <= n; i++) {
                fitted = slope * x[i] + intercept
                residual = y[i] - fitted
                absolute = residual < 0 ? -residual : residual
                if (absolute > max_residual) max_residual = absolute
                ss_res += residual * residual
                centered = y[i] - mean
                ss_total += centered * centered
            }

            fitted_span = slope * (xmax - xmin)
            if (fitted_span < 0) fitted_span = -fitted_span
            r2 = ss_total > 0 ? sprintf("%.6f", 1 - ss_res / ss_total) : "N/A"
            residual_pct = fitted_span > 0 \
                ? sprintf("%.6f", max_residual * 100 / fitted_span) : "N/A"
            printf "%.12g\t%.12g\t%s\t%.12g\t%s\t%d\n", \
                slope, intercept, r2, max_residual, residual_pct, n
        }
    ' "$points_file")"
    IFS=$'\t' read -r FIT_SLOPE FIT_INTERCEPT FIT_R2_VALUE \
        FIT_MAX_RESIDUAL FIT_RESIDUAL_PCT FIT_POINT_COUNT <<< "$values"
}

append_linearity_fit() {
    local axis=$1 points_file=$2 fit_key=$3 sign intercept_abs pct
    calculate_linearity_fit "$points_file"
    if [[ "$FIT_SLOPE" == "N/A" ]]; then
        printf '| %s | N/A | N/A | N/A | N/A | %s |\n' \
            "$axis" "$FIT_POINT_COUNT" >> "$BODY"
        return
    fi

    FIT_SLOPES["$fit_key"]=$FIT_SLOPE
    FIT_INTERCEPTS["$fit_key"]=$FIT_INTERCEPT
    FIT_R2["$fit_key"]=$FIT_R2_VALUE
    sign="+"
    intercept_abs=$FIT_INTERCEPT
    if awk -v value="$FIT_INTERCEPT" 'BEGIN { exit !(value < 0) }'; then
        sign="-"
        intercept_abs="${FIT_INTERCEPT#-}"
    fi
    pct=$FIT_RESIDUAL_PCT
    [[ "$pct" != "N/A" ]] && pct="${pct}%"
    printf '| %s | raw_adc = %.6f × DAC %s %.3f | %s | %.3f | %s | %s |\n' \
        "$axis" "$FIT_SLOPE" "$sign" "$intercept_abs" "$FIT_R2_VALUE" \
        "$FIT_MAX_RESIDUAL" "$pct" "$FIT_POINT_COUNT" >> "$BODY"
}

gnuplot_escape() {
    local value=${1//\\/\\\\}
    printf '%s' "${value//\'/\\\'}"
}

generate_channel_plot() {
    local ch=$1 has_v=$2 fit_v_file=$3 has_i=$4 fit_i_file=$5
    local plot_path plot_name escaped_plot panels=$((has_v + has_i))
    local escaped_v="" escaped_i="" key slope intercept r2
    if [[ "$REPORT" == *.md ]]; then
        plot_path="${REPORT%.md}_ch${ch}.png"
    else
        plot_path="${REPORT}_ch${ch}.png"
    fi
    plot_name="$(basename "$plot_path")"
    escaped_plot="$(gnuplot_escape "$plot_path")"
    ((has_v)) && escaped_v="$(gnuplot_escape "$fit_v_file")"
    ((has_i)) && escaped_i="$(gnuplot_escape "$fit_i_file")"

    {
        echo "set terminal pngcairo size 1200,800 enhanced font 'Sans,11'"
        echo "set output '$escaped_plot'"
        echo "set grid"
        echo "set xlabel 'DAC code'"
        echo "set key top left"
        if ((panels == 0)); then
            echo "unset border"
            echo "unset tics"
            echo "set xrange [0:1]"
            echo "set yrange [0:1]"
            echo "set label 'CH$ch: no raw ADC measurement capability' at 0.5,0.5 center"
            echo "plot NaN notitle"
        else
            ((panels > 1)) && echo "set multiplot layout $panels,1 title 'CH$ch DAC Sweep Linearity'"
            if ((has_v)); then
                key="$ch:v"; slope=${FIT_SLOPES[$key]}; intercept=${FIT_INTERCEPTS[$key]}; r2=${FIT_R2[$key]}
                echo "set ylabel 'Raw ADC V (counts)'"
                echo "set title sprintf('Raw ADC V: y = %.6f x + %.3f, R^2 = %s', $slope, $intercept, '$r2')"
                echo "plot '$escaped_v' using 1:2 with points pointtype 7 pointsize 1.2 title 'Samples', $slope*x+$intercept with lines linewidth 2 title 'OLS fit'"
            fi
            if ((has_i)); then
                key="$ch:i"; slope=${FIT_SLOPES[$key]}; intercept=${FIT_INTERCEPTS[$key]}; r2=${FIT_R2[$key]}
                echo "set ylabel 'Raw ADC I (counts)'"
                echo "set title sprintf('Raw ADC I: y = %.6f x + %.3f, R^2 = %s', $slope, $intercept, '$r2')"
                echo "plot '$escaped_i' using 1:2 with points pointtype 7 pointsize 1.2 title 'Samples', $slope*x+$intercept with lines linewidth 2 title 'OLS fit'"
            fi
            ((panels > 1)) && echo "unset multiplot"
        fi
    } | gnuplot || return 1

    [[ -s "$plot_path" ]] || return 1
    echo "![CH$ch DAC sweep plot]($plot_name)" >> "$BODY"
    echo >> "$BODY"
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
    rm -f "${TEMP_FILES[@]}"
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
command -v gnuplot >/dev/null || fail "gnuplot is required to generate PNG plots"

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
    fit_v_file=""
    fit_i_file=""
    if ((has_v)); then
        fit_v_file="$(mktemp)"
        TEMP_FILES+=("$fit_v_file")
    fi
    if ((has_i)); then
        fit_i_file="$(mktemp)"
        TEMP_FILES+=("$fit_i_file")
    fi

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
        ((has_v)) && echo "$dac $raw_v" >> "$fit_v_file"
        ((has_i)) && echo "$dac $raw_i" >> "$fit_i_file"
    done

    write_reg "$((base + 31))" 0 || fail "CH$ch DAC zero failed"
    write_reg "$((base + 30))" 0 || fail "CH$ch calibration output disable failed"
    echo >> "$BODY"
    if ((has_v || has_i)); then
        {
            echo "### Linearity Fit"
            echo
            echo "| Axis | Equation | R² | Max residual | Max residual (% span) | Points |"
            echo "|---|---|---:|---:|---:|---:|"
        } >> "$BODY"
        ((has_v)) && append_linearity_fit "Raw ADC V" "$fit_v_file" "$ch:v"
        ((has_i)) && append_linearity_fit "Raw ADC I" "$fit_i_file" "$ch:i"
        echo >> "$BODY"
    fi
    generate_channel_plot "$ch" "$has_v" "$fit_v_file" "$has_i" "$fit_i_file" \
        || fail "CH$ch PNG plot generation failed"
done
