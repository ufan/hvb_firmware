#!/usr/bin/env bash
# board_test.sh — psb_demo_cli end-to-end board test
#
# Drives the compiled psb_demo_cli binary itself (not raw pymodbus) against
# a live board over its Modbus RTU interface — the same commands an operator
# would type. Variant-agnostic by construction: channel count and every
# per-channel capability flag (CH_CAP_*) are discovered at runtime from the
# connected device via `raw fc04`, never hardcoded, so this one script
# covers jw_hvb, jw_lvb, and any future variant without modification.
#
# Ground truth for register addresses and capability gating:
#   include/reg_store/modbus_view.def   (wire offsets used below)
#   lib/voltage_control/vc_runtime.c    (vc_catalog_supported() gate table
#                                         this script's expectations mirror)
#
# Safety model:
#   - All per-channel CONFIG-field writes (ramp, recovery, safe-band,
#     prot-i, derate, voltage target, enable-cfg) are SAME-VALUE round
#     trips: read the current value, write it straight back, verify it
#     read back unchanged. This exercises the full write path (CLI parsing,
#     capability gating, wire encoding, firmware write, readback) without
#     ever changing configured behavior.
#   - `channel <n> fault CLEAR-HISTORY` is a one-way but harmless command;
#     run unconditionally.
#   - The one command that is genuinely, instantly live is
#     `channel <n> output ENABLE|DISABLE-GRACEFUL`. On a locked always-on
#     channel (no CH_CAP_OUTPUT_ENABLE) this script always attempts
#     DISABLE-GRACEFUL and asserts it is REJECTED with the live enable bit
#     unchanged — this is the core hardware-safety invariant and is safe by
#     construction. On a switchable channel, actually toggling it off/on is
#     skipped by default (a real connected load could be affected) and only
#     runs with --exercise-outputs.
#
# Usage: ./board_test.sh [--port /dev/ttyUSB0] [--baud 115200] [--slave 1]
#                         [--timeout 2000] [--cli PATH] [--report PATH]
#                         [--channel N] [--read-only] [--exercise-outputs]
#                         [--assert-fresh]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="/dev/ttyUSB0"
BAUD=115200
SLAVE=1
TIMEOUT_MS=2000
CLI="$SCRIPT_DIR/../bin/psb_demo_cli"
REPORT=""
ONLY_CHANNEL=-1
READ_ONLY=0
EXERCISE_OUTPUTS=0
ASSERT_FRESH=0
EXPECT_DISABLED=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]
  --port PATH           Serial port (default: /dev/ttyUSB0)
  --baud RATE           Baud rate (default: 115200)
  --slave ID            Modbus slave ID (default: 1)
  --timeout MS          CLI timeout in milliseconds (default: 2000)
  --cli PATH            psb_demo_cli executable
  --report PATH         Markdown report path
  --channel N           Test only channel N (default: all supported channels)
  --read-only            Skip every write test; only exercise read/describe commands
  --exercise-outputs      Also toggle ENABLE/DISABLE-GRACEFUL on switchable channels
  --assert-fresh          Also assert every channel matches documented factory
                          defaults (CFG_OUTPUT_ENABLED=1 on switchable channels,
                          CFG_TARGET_VOLTAGE=0 on DAC channels). Only meaningful
                          right after a clean erase+flash — a board with any
                          legitimate prior configuration will fail these checks.
                          See factory_bringup.sh, which always passes this.
  --expect-disabled LIST  Comma-separated channel numbers whose documented
                          factory default is CFG_OUTPUT_ENABLED=0 instead of
                          the normal 1. Only affects --assert-fresh. Example:
                          --expect-disabled 3 or --expect-disabled 3,7
  -h, --help              Show this help
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
    --channel) ONLY_CHANNEL="$2"; shift 2 ;;
    --read-only) READ_ONLY=1; shift ;;
    --exercise-outputs) EXERCISE_OUTPUTS=1; shift ;;
    --assert-fresh) ASSERT_FRESH=1; shift ;;
    --expect-disabled) EXPECT_DISABLED="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$REPORT" ]]; then
    REPORT="$SCRIPT_DIR/reports/board_test_$(date -u +%Y%m%d_%H%M%S).md"
fi
mkdir -p "$(dirname "$REPORT")"
BODY="$(mktemp)"
OUTF="$(mktemp)"
ERRF="$(mktemp)"

PASS=0
FAIL=0
SKIP=0

# ------------------------------------------------------------------
# CLI / raw Modbus helpers
# ------------------------------------------------------------------

cli() {
    # Each invocation is a fresh process that re-opens the serial port from
    # scratch; on USB-serial adapters (e.g. CH340) back-to-back reopens with
    # no gap intermittently fail to re-establish before the first Modbus
    # transaction. A short settle delay plus a single retry on the
    # connection-level error absorbs that without masking real failures.
    # Retry once on a plausibly-transient failure. Write-command callbacks
    # always print something on a *legitimate* outcome: "OK" or a Modbus
    # error string, both to stdout, on success or firmware-level rejection;
    # empty stdout only happens on a connection-level failure or a hang, or
    # on one of this test's own recognized client-side pre-check rejections
    # (which land on stderr and are exempted below since retrying them would
    # never change the outcome by design).
    local attempt
    for attempt in 1 2; do
        sleep 0.15
        "$CLI" -p "$PORT" -b "$BAUD" -i "$SLAVE" -t "$TIMEOUT_MS" "$@" >"$OUTF" 2>"$ERRF"
        LAST_RC=$?
        LAST_OUT="$(cat "$OUTF")"
        LAST_ERR="$(cat "$ERRF")"
        if [[ $attempt -eq 1 && -z "$LAST_OUT" ]]; then
            case "$LAST_ERR" in
                *"has no DAC"*|*"has a DAC"*|*"locked always-on"*) break ;;
                *) continue ;;
            esac
        fi
        break
    done
}

# raw fc04 read: addr count -> WORDS[] (uppercase hex strings)
# Retries up to 3 total attempts — raw subcommand callbacks print a
# Modbus-level error to stdout without setting a nonzero exit code, so
# cli()'s own connection-error retry never sees this failure mode; the
# word-count/hex-format check below is what actually detects it.
raw_read() {
    local attempt
    for attempt in 1 2 3; do
        cli raw fc04 "$1" "$2"
        if [[ $LAST_RC -eq 0 ]]; then
            WORDS=()
            local tok ok=1
            while IFS= read -r tok; do
                [[ -z "$tok" ]] && continue
                if [[ "$tok" =~ ^[[:xdigit:]]{4}$ ]]; then WORDS+=("${tok^^}"); else ok=0; fi
            done < <(printf '%s\n' "$LAST_OUT" | tr '[:space:]' '\n')
            if [[ $ok -eq 1 && ${#WORDS[@]} -eq $2 ]]; then return 0; fi
        fi
        sleep 0.2
    done
    return 1
}

# raw fc03 read: addr -> WORD_DEC (single unsigned decimal word)
holding_word() {
    local attempt tok
    for attempt in 1 2 3; do
        cli raw fc03 "$1" 1
        if [[ $LAST_RC -eq 0 ]]; then
            tok="$(printf '%s' "$LAST_OUT" | tr -d '[:space:]')"
            if [[ "$tok" =~ ^[[:xdigit:]]{4}$ ]]; then
                WORD_DEC=$((16#$tok))
                return 0
            fi
        fi
        sleep 0.2
    done
    return 1
}

# raw fc03 read: addr count -> WD[] (unsigned decimal array)
holding_words() {
    local attempt
    for attempt in 1 2 3; do
        cli raw fc03 "$1" "$2"
        if [[ $LAST_RC -eq 0 ]]; then
            WD=()
            local tok ok=1
            while IFS= read -r tok; do
                [[ -z "$tok" ]] && continue
                if [[ "$tok" =~ ^[[:xdigit:]]{4}$ ]]; then WD+=($((16#$tok))); else ok=0; fi
            done < <(printf '%s\n' "$LAST_OUT" | tr '[:space:]' '\n')
            if [[ $ok -eq 1 && ${#WD[@]} -eq $2 ]]; then return 0; fi
        fi
        sleep 0.2
    done
    return 1
}

u16() { echo "$((16#$1))"; }

# Is channel $1 listed in --expect-disabled? (comma-separated channel numbers
# whose documented factory default is CFG_OUTPUT_ENABLED=0, not the normal 1)
expect_disabled() {
    local ch="$1" tok
    IFS=',' read -ra toks <<<"$EXPECT_DISABLED"
    for tok in "${toks[@]}"; do
        [[ "$tok" == "$ch" ]] && return 0
    done
    return 1
}

recovery_policy_name() { case "$1" in 0) echo MANUAL-LATCH;; 1) echo AUTO-RETRY;; 2) echo AUTO-DERATE;; 3) echo NEVER-RETRY;; *) echo MANUAL-LATCH;; esac; }
protection_mode_name()  { case "$1" in 0) echo DISABLED;; 1) echo FLAG-ONLY;; 2) echo APPLY-ACTION;; *) echo DISABLED;; esac; }
protection_action_name() { case "$1" in 0) echo NONE;; 2) echo DISABLE-GRACEFUL;; 3) echo DISABLE-IMMEDIATE;; 4) echo FORCE-ZERO;; *) echo NONE;; esac; }

pass() { PASS=$((PASS+1)); echo "  PASS: $*"; printf '| PASS | %s |\n' "$*" >> "$BODY"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $*" >&2; printf '| FAIL | %s |\n' "$*" >> "$BODY"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP: $*"; printf '| SKIP | %s |\n' "$*" >> "$BODY"; }

finish() {
    local rc=$?
    trap - EXIT INT TERM
    {
        echo "# psb_demo_cli Board Test Report"
        echo
        echo "- **Time**: $(date -u -Iseconds)"
        echo "- **Port**: $PORT  **Baud**: $BAUD  **Slave**: $SLAVE  **Timeout**: ${TIMEOUT_MS}ms"
        echo "- **CLI**: $CLI"
        [[ -n "${protocol_major:-}" ]] && echo "- **Protocol**: ${protocol_major}.${protocol_minor}  **Variant**: ${variant_id}  **FW**: ${fw_version}  **Channels**: ${channel_count}"
        echo "- **Read-only**: $((READ_ONLY)) **Exercise-outputs**: $((EXERCISE_OUTPUTS)) **Assert-fresh**: $((ASSERT_FRESH))${EXPECT_DISABLED:+ **Expect-disabled**: $EXPECT_DISABLED}"
        echo
        echo "| Result | Check |"
        echo "|---|---|"
        cat "$BODY"
        echo
        echo "## Summary"
        echo
        echo "PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
        echo
        if [[ $FAIL -eq 0 ]]; then
            echo "**Overall: PASS**"
        else
            echo "**Overall: FAIL**"
        fi
    } > "$REPORT"
    rm -f "$BODY" "$OUTF" "$ERRF"
    echo
    echo "Report: $REPORT"
    echo "PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
    if [[ $rc -ne 0 ]]; then exit "$rc"; fi
    [[ $FAIL -eq 0 ]] && exit 0 || exit 1
}
trap finish EXIT
trap 'echo "Interrupted" >&2; exit 130' INT
trap 'echo "Terminated" >&2; exit 143' TERM

[[ -x "$CLI" ]] || { echo "CLI is not executable: $CLI" >&2; exit 2; }

# ------------------------------------------------------------------
# Phase 0 — connectivity + system input block ground truth
# ------------------------------------------------------------------
echo "== Phase 0: connectivity =="
echo "## Preflight" >> "$BODY"
if raw_read 0 15; then
    protocol_major=$(u16 "${WORDS[0]}")
    protocol_minor=$(u16 "${WORDS[1]}")
    variant_id=$(u16 "${WORDS[2]}")
    sys_caps=$(u16 "${WORDS[3]}")
    channel_count=$(u16 "${WORDS[4]}")
    fw_version="0x${WORDS[10]}${WORDS[11]}"
    if [[ $protocol_major -eq 3 && $channel_count -ge 1 && $channel_count -le 16 ]]; then
        pass "connectivity + sys info block sane (protocol=${protocol_major}.${protocol_minor} variant=$variant_id channels=$channel_count fw=$fw_version)"
    else
        fail "sys info block out of range (protocol_major=$protocol_major channels=$channel_count)"
        exit 1
    fi
else
    echo "Cannot reach board via raw fc04 — aborting." >&2
    fail "board unreachable via Modbus (port=$PORT baud=$BAUD slave=$SLAVE)"
    exit 1
fi

# ------------------------------------------------------------------
# Phase 1 — system-level command surface
# ------------------------------------------------------------------
echo "== Phase 1: system-level commands =="
echo >> "$BODY"; echo "## System-level commands" >> "$BODY"

# A single-shot multi-register text dump with no retry protection of its
# own (unlike raw_read/holding_word*); retry the whole check a couple of
# times before concluding the mismatch is real rather than a transient
# single-transaction glitch on this serial link.
info_ok=0
for attempt in 1 2; do
    cli info
    if [[ $LAST_RC -eq 0 ]] \
       && grep -q "Protocol:.*${protocol_major}\.${protocol_minor}" <<<"$LAST_OUT" \
       && grep -q "Variant ID:.*${variant_id}" <<<"$LAST_OUT" \
       && grep -q "Channels:.*${channel_count} " <<<"$LAST_OUT"; then
        info_ok=1; break
    fi
done
if [[ $info_ok -eq 1 ]]; then
    pass "info: matches raw ground truth (protocol/variant/channels)"
else
    fail "info: output mismatch vs raw ground truth"
fi

cli status
blocks=$(grep -c '^=== Channel ' <<<"$LAST_OUT" || true)
if [[ $LAST_RC -eq 0 && "$blocks" -eq "$channel_count" ]]; then
    pass "status: printed $blocks/$channel_count channel blocks"
else
    fail "status: expected $channel_count channel blocks, got $blocks"
fi

cli system config
sc_slave=$(grep "Slave Address:" <<<"$LAST_OUT" | awk '{print $NF}')
sc_baud=$(grep "Baud Rate:" <<<"$LAST_OUT" | awk '{print $NF}')
if [[ "$sc_slave" == "$SLAVE" && "$sc_baud" == "$BAUD" ]]; then
    pass "system config: slave=$sc_slave baud=$sc_baud match connection params"
else
    fail "system config: slave=$sc_slave baud=$sc_baud, expected $SLAVE/$BAUD"
fi

cli list ports
[[ $LAST_RC -eq 0 ]] && pass "list ports: ran OK" || fail "list ports: exit $LAST_RC"

sleep 0.15
mon_out="$(timeout 8s "$CLI" -p "$PORT" -b "$BAUD" -i "$SLAVE" -t "$TIMEOUT_MS" monitor 1 2>&1 || true)"
if grep -q '=== PSB Monitor \[' <<<"$mon_out"; then
    pass "monitor: rendered at least one frame"
else
    fail "monitor: expected header not found"
fi

cli channel 99 info
if [[ $LAST_RC -ne 0 ]]; then
    pass "channel 99: rejected by CLI11 arg validation (exit $LAST_RC)"
else
    fail "channel 99: expected CLI11 to reject out-of-range channel"
fi

# ------------------------------------------------------------------
# Phase 2 — per-channel tests
# ------------------------------------------------------------------
echo "== Phase 2: per-channel tests =="
echo >> "$BODY"; echo "## Per-channel tests" >> "$BODY"

for ((ch = 0; ch < channel_count; ch++)); do
    if [[ $ONLY_CHANNEL -ge 0 && $ch -ne $ONLY_CHANNEL ]]; then continue; fi
    base=$((40 + ch * 40))
    echo "-- channel $ch --"
    echo >> "$BODY"; echo "### Channel $ch" >> "$BODY"

    if ! raw_read "$((base + 9))" 1; then
        fail "ch$ch: cannot read capability flags"
        continue
    fi
    caps=$(u16 "${WORDS[0]}")
    has_en=$(( (caps & 0x0001) != 0 ))
    has_drive=$(( (caps & 0x0002) != 0 ))
    has_v=$(( (caps & 0x0004) != 0 ))
    has_i=$(( (caps & 0x0008) != 0 ))
    printf -v caps_hex '0x%04X' "$caps"
    echo "Capabilities: \`$caps_hex\` (en=$has_en drive=$has_drive v=$has_v i=$has_i)" >> "$BODY"
    echo >> "$BODY"

    cli channel "$ch" info
    if [[ $LAST_RC -eq 0 ]] && grep -q "Measured V:" <<<"$LAST_OUT"; then
        pass "ch$ch info: ran OK"
    else
        fail "ch$ch info: unexpected output"
    fi

    # Capability-branch regression check (task #16's CLI fix). Like `info`
    # above, this is a multi-register text dump with no retry protection of
    # its own; retry the whole check before concluding a mismatch is real.
    branch_ok=0
    for attempt in 1 2; do
        cli channel "$ch" config
        cfg_out="$LAST_OUT"
        ok=1
        if (( has_drive )); then
            grep -q "Configured Target:" <<<"$cfg_out" || ok=0
            grep -q "no DAC" <<<"$cfg_out" && ok=0
        else
            grep -q "no DAC" <<<"$cfg_out" || ok=0
            if (( has_en )); then
                grep -q "Output Enabled (cfg):" <<<"$cfg_out" || ok=0
                grep -q "locked always-on" <<<"$cfg_out" && ok=0
            else
                grep -q "locked always-on" <<<"$cfg_out" || ok=0
            fi
        fi
        if (( has_i )); then
            grep -q "no current measurement" <<<"$cfg_out" && ok=0
        else
            grep -q "no current measurement" <<<"$cfg_out" || ok=0
        fi
        if (( has_drive && has_v )); then
            grep -q "needs DAC" <<<"$cfg_out" && ok=0
        else
            grep -q "needs DAC" <<<"$cfg_out" || ok=0
        fi
        if [[ $LAST_RC -eq 0 && $ok -eq 1 ]]; then branch_ok=1; break; fi
    done
    if [[ $branch_ok -eq 1 ]]; then
        pass "ch$ch config: capability-branched fields correct"
    else
        fail "ch$ch config: capability-branch mismatch (caps=$caps_hex)"
    fi

    cli channel "$ch" cal
    if [[ $LAST_RC -eq 0 ]] && grep -q "Output:" <<<"$LAST_OUT"; then
        pass "ch$ch cal: coefficients read OK"
    else
        fail "ch$ch cal: read failed"
    fi

    if (( READ_ONLY )); then
        skip "ch$ch: write tests skipped (--read-only)"
        continue
    fi

    # ---- voltage / enable-cfg: capability-exclusive same-value round-trip ----
    if (( has_drive )); then
        if holding_word "$((base + 3))"; then
            before=$WORD_DEC
            if (( ASSERT_FRESH )); then
                if [[ $before -eq 0 ]]; then
                    pass "ch$ch voltage: factory default confirmed (target=0)"
                else
                    fail "ch$ch voltage: NOT at factory default (target=$before, expected 0) — board may have stale NVS state; see factory_bringup.sh"
                fi
            fi
            rt_ok=0
            for attempt in 1 2; do
                cli channel "$ch" voltage "$before"; wr_out="$LAST_OUT"
                if [[ "$wr_out" == "OK" ]] && holding_word "$((base + 3))" && [[ $WORD_DEC -eq $before ]]; then
                    rt_ok=1; break
                fi
            done
            if [[ $rt_ok -eq 1 ]]; then
                pass "ch$ch voltage: same-value round-trip OK (raw=$before)"
            else
                fail "ch$ch voltage: round-trip failed (before=$before after=${WORD_DEC:-?} out=$wr_out)"
            fi
        else
            fail "ch$ch voltage: could not read baseline"
        fi
        cli channel "$ch" enable-cfg 1
        if [[ $LAST_RC -eq 0 ]] && grep -q "has a DAC" <<<"$LAST_ERR"; then
            pass "ch$ch enable-cfg: correctly client-rejected (has DAC)"
        else
            fail "ch$ch enable-cfg: expected client-side DAC rejection (err='$LAST_ERR' out='$LAST_OUT')"
        fi
    elif (( has_en )); then
        if holding_word "$((base + 17))"; then
            before=$WORD_DEC
            if (( ASSERT_FRESH )); then
                if expect_disabled "$ch"; then
                    if [[ $before -eq 0 ]]; then
                        pass "ch$ch enable-cfg: factory default confirmed (enabled=0, per --expect-disabled)"
                    else
                        fail "ch$ch enable-cfg: NOT at factory default (enabled=$before, expected 0 per --expect-disabled) — board may have stale NVS state; see factory_bringup.sh"
                    fi
                elif [[ $before -eq 1 ]]; then
                    pass "ch$ch enable-cfg: factory default confirmed (enabled=1)"
                else
                    fail "ch$ch enable-cfg: NOT at factory default (enabled=$before, expected 1) — board may have stale NVS state; see factory_bringup.sh"
                fi
            fi
            rt_ok=0
            for attempt in 1 2; do
                cli channel "$ch" enable-cfg "$before"; wr_out="$LAST_OUT"
                if [[ "$wr_out" == "OK" ]] && holding_word "$((base + 17))" && [[ $WORD_DEC -eq $before ]]; then
                    rt_ok=1; break
                fi
            done
            if [[ $rt_ok -eq 1 ]]; then
                pass "ch$ch enable-cfg: same-value round-trip OK (val=$before)"
            else
                fail "ch$ch enable-cfg: round-trip failed (before=$before after=${WORD_DEC:-?} out=$wr_out)"
            fi
        else
            fail "ch$ch enable-cfg: could not read baseline"
        fi
        cli channel "$ch" voltage 0
        if grep -q "has no DAC" <<<"$LAST_ERR"; then
            pass "ch$ch voltage: correctly client-rejected (no DAC)"
        else
            fail "ch$ch voltage: expected client-side no-DAC rejection (err='$LAST_ERR' out='$LAST_OUT')"
        fi
    else
        cli channel "$ch" voltage 0
        if grep -q "has no DAC" <<<"$LAST_ERR"; then
            pass "ch$ch voltage: correctly client-rejected (locked channel, no DAC)"
        else
            fail "ch$ch voltage: expected client-side rejection (err='$LAST_ERR')"
        fi
        cli channel "$ch" enable-cfg 1
        if grep -q "locked always-on" <<<"$LAST_ERR"; then
            pass "ch$ch enable-cfg: correctly client-rejected (locked always-on)"
        else
            fail "ch$ch enable-cfg: expected client-side rejection (err='$LAST_ERR')"
        fi
    fi

    # ---- ramp-up / ramp-down: RAW_OUTPUT_DRIVE only ----
    # Baseline reads at this address range are themselves capability-gated,
    # so on a channel without RAW_OUTPUT_DRIVE the read fails with a Modbus
    # exception before any write is attempted — check capability first and
    # only require a readable baseline when the field is actually supported.
    if (( has_drive )); then
        if holding_words "$((base + 4))" 4; then
            rus=${WD[0]}; rui=${WD[1]}; rds=${WD[2]}; rdi=${WD[3]}
            rt_ok=0
            for attempt in 1 2; do
                cli channel "$ch" ramp-up "$rus" "$rui"; ru_out="$LAST_OUT"
                cli channel "$ch" ramp-down "$rds" "$rdi"; rd_out="$LAST_OUT"
                if [[ "$ru_out" == "OK" && "$rd_out" == "OK" ]] && holding_words "$((base + 4))" 4 \
                   && [[ ${WD[0]} -eq $rus && ${WD[1]} -eq $rui && ${WD[2]} -eq $rds && ${WD[3]} -eq $rdi ]]; then
                    rt_ok=1; break
                fi
            done
            if [[ $rt_ok -eq 1 ]]; then
                pass "ch$ch ramp-up/down: same-value round-trip OK"
            else
                fail "ch$ch ramp-up/down: round-trip mismatch"
            fi
        else
            fail "ch$ch ramp-up/down: could not read baseline"
        fi
    else
        cli channel "$ch" ramp-up 0 0; ru_out="$LAST_OUT"
        cli channel "$ch" ramp-down 0 0; rd_out="$LAST_OUT"
        if [[ "$ru_out" != "OK" && "$rd_out" != "OK" ]]; then
            pass "ch$ch ramp-up/down: correctly rejected by firmware (no DAC)"
        else
            fail "ch$ch ramp-up/down: expected firmware rejection (ru='$ru_out' rd='$rd_out')"
        fi
    fi

    # ---- recovery policy: always accessible ----
    # The write itself has no retry protection of its own (unlike the
    # holding_word* baseline/verify reads); retry the whole round trip
    # before concluding a mismatch is real rather than a transient glitch.
    if holding_words "$((base + 8))" 4; then
        pol=${WD[0]}; delay=${WD[1]}; mx=${WD[2]}; win=${WD[3]}
        pol_name=$(recovery_policy_name "$pol")
        rt_ok=0
        for attempt in 1 2; do
            cli channel "$ch" recovery "$pol_name" "$delay" "$mx" "$win"
            if [[ "$LAST_OUT" == "OK" ]] && holding_words "$((base + 8))" 4 \
               && [[ ${WD[0]} -eq $pol && ${WD[1]} -eq $delay && ${WD[2]} -eq $mx && ${WD[3]} -eq $win ]]; then
                rt_ok=1; break
            fi
        done
        if [[ $rt_ok -eq 1 ]]; then
            pass "ch$ch recovery: same-value round-trip OK ($pol_name delay=$delay max=$mx window=$win)"
        else
            fail "ch$ch recovery: round-trip mismatch"
        fi
    else
        fail "ch$ch recovery: could not read baseline"
    fi

    # ---- safe-band: always accessible ----
    if holding_word "$((base + 12))"; then
        pct=$WORD_DEC
        rt_ok=0
        for attempt in 1 2; do
            cli channel "$ch" safe-band "$pct"
            if [[ "$LAST_OUT" == "OK" ]] && holding_word "$((base + 12))" && [[ $WORD_DEC -eq $pct ]]; then
                rt_ok=1; break
            fi
        done
        if [[ $rt_ok -eq 1 ]]; then
            pass "ch$ch safe-band: same-value round-trip OK (${pct}%)"
        else
            fail "ch$ch safe-band: round-trip mismatch"
        fi
    else
        fail "ch$ch safe-band: could not read baseline"
    fi

    # ---- prot-i: CURRENT_MEASUREMENT only ----
    if holding_words "$((base + 13))" 3; then
        pi_mode=${WD[0]}; pi_act=${WD[1]}; pi_thr=${WD[2]}
        pi_mode_name=$(protection_mode_name "$pi_mode")
        pi_act_name=$(protection_action_name "$pi_act")
        if (( has_i )); then
            rt_ok=0
            for attempt in 1 2; do
                cli channel "$ch" prot-i "$pi_mode_name" "$pi_act_name" "$pi_thr"
                if [[ "$LAST_OUT" == "OK" ]] && holding_words "$((base + 13))" 3 \
                   && [[ ${WD[0]} -eq $pi_mode && ${WD[1]} -eq $pi_act && ${WD[2]} -eq $pi_thr ]]; then
                    rt_ok=1; break
                fi
            done
            if [[ $rt_ok -eq 1 ]]; then
                pass "ch$ch prot-i: same-value round-trip OK"
            else
                fail "ch$ch prot-i: round-trip mismatch"
            fi
        else
            cli channel "$ch" prot-i "$pi_mode_name" "$pi_act_name" "$pi_thr"
            if [[ "$LAST_OUT" != "OK" ]]; then
                pass "ch$ch prot-i: correctly rejected by firmware (no CURRENT_MEASUREMENT)"
            else
                fail "ch$ch prot-i: expected firmware rejection, got OK"
            fi
        fi
    else
        fail "ch$ch prot-i: could not read baseline"
    fi

    # ---- derate step: RAW_OUTPUT_DRIVE + VOLTAGE_MEASUREMENT only ----
    # Same capability-first ordering as ramp-up/down above: the baseline
    # read itself is gated, so check capability before requiring it.
    if (( has_drive && has_v )); then
        if holding_word "$((base + 16))"; then
            dstep=$WORD_DEC
            rt_ok=0
            for attempt in 1 2; do
                cli channel "$ch" derate "$dstep"
                if [[ "$LAST_OUT" == "OK" ]] && holding_word "$((base + 16))" && [[ $WORD_DEC -eq $dstep ]]; then
                    rt_ok=1; break
                fi
            done
            if [[ $rt_ok -eq 1 ]]; then
                pass "ch$ch derate: same-value round-trip OK"
            else
                fail "ch$ch derate: round-trip mismatch"
            fi
        else
            fail "ch$ch derate: could not read baseline"
        fi
    else
        cli channel "$ch" derate 0
        if [[ "$LAST_OUT" != "OK" ]]; then
            pass "ch$ch derate: correctly rejected by firmware (needs DAC+Vmeas)"
        else
            fail "ch$ch derate: expected firmware rejection, got OK"
        fi
    fi

    # ---- fault clear-history: harmless one-way command ----
    cli channel "$ch" fault CLEAR-HISTORY
    if [[ "$LAST_OUT" == "OK" ]]; then
        pass "ch$ch fault CLEAR-HISTORY: accepted"
    else
        fail "ch$ch fault CLEAR-HISTORY: rejected ('$LAST_OUT')"
    fi

    # ---- output ENABLE/DISABLE-GRACEFUL: the one truly-live command ----
    en_before=-1
    if raw_read "$((base + 0))" 1; then
        status_before=$(u16 "${WORDS[0]}")
        en_before=$(( (status_before & 2) != 0 ))
    fi
    if (( has_en )); then
        if (( EXERCISE_OUTPUTS )); then
            cli channel "$ch" output DISABLE-GRACEFUL; d_out="$LAST_OUT"
            sleep 0.3
            cli channel "$ch" output ENABLE; e_out="$LAST_OUT"
            if [[ "$d_out" == "OK" && "$e_out" == "OK" ]]; then
                pass "ch$ch output DISABLE-GRACEFUL/ENABLE: accepted + restored (switchable channel)"
            else
                fail "ch$ch output toggle: disable='$d_out' enable='$e_out'"
            fi
        else
            skip "ch$ch output DISABLE/ENABLE toggle: switchable channel, skipped (pass --exercise-outputs to test live toggle)"
        fi
    else
        cli channel "$ch" output DISABLE-GRACEFUL
        d_out="$LAST_OUT"; d_err="$LAST_ERR"
        en_after=-2
        if raw_read "$((base + 0))" 1; then
            status_after=$(u16 "${WORDS[0]}")
            en_after=$(( (status_after & 2) != 0 ))
        fi
        if [[ "$d_out" != "OK" ]] && grep -q "locked always-on" <<<"$d_err"; then
            pass "ch$ch output DISABLE-GRACEFUL: correctly rejected (locked always-on channel)"
        else
            fail "ch$ch output DISABLE-GRACEFUL: expected rejection (out='$d_out' err='$d_err')"
        fi
        if [[ $en_before -ge 0 && $en_before -eq $en_after ]]; then
            pass "ch$ch: OUTPUT_ENABLE_ACTIVE unchanged after rejected disable (safety invariant holds, bit=$en_before)"
        else
            fail "ch$ch: OUTPUT_ENABLE_ACTIVE bit CHANGED after rejected disable! before=$en_before after=$en_after — SAFETY INVARIANT VIOLATED"
        fi
    fi
done

echo "== Done =="
