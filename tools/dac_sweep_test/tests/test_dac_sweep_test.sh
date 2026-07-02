#!/usr/bin/env bash
set -euo pipefail

TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$TEST_DIR/../../.." && pwd)"
RUNNER="$ROOT_DIR/tools/dac_sweep_test/dac_sweep_test.sh"
MOCK="$TEST_DIR/mock_hvb_demo_cli.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

sleep() { :; }
export -f sleep

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

run_success_case() {
    local state="$TMP/success-state" report="$TMP/success.md"
    mkdir -p "$state"
    MOCK_STATE_DIR="$state" "$RUNNER" --cli "$MOCK" --report "$report" \
        --port /dev/mock --timeout 10 >/dev/null

    grep -q '^# DAC Sweep Test Report — PASS$' "$report" || fail "success heading"
    grep -q '| 60000 | 600000 | -600000 | 6000 | 600.0 | -6000 | -6000 |' "$report" || fail "signed/scaled row"
    grep -q 'CH1 measurement capability: none; measurement columns are N/A.' "$report" || fail "CH1 capability note"
    grep -q 'CH2 skipped: missing RAW_OUTPUT_DRIVE capability.' "$report" || fail "CH2 skip note"
    grep -q 'raw fc06 71 60000' "$state/commands.log" || fail "CH0 upper DAC write"
    grep -q 'raw fc06 111 60000' "$state/commands.log" || fail "CH1 upper DAC write"
    grep -q 'raw fc06 71 0' "$state/commands.log" || fail "CH0 DAC cleanup"
    grep -q 'raw fc06 70 0' "$state/commands.log" || fail "CH0 output cleanup"
    grep -q 'raw fc06 111 0' "$state/commands.log" || fail "CH1 DAC cleanup"
    grep -q 'raw fc06 110 0' "$state/commands.log" || fail "CH1 output cleanup"
    grep -q 'raw fc06 681 1' "$state/commands.log" || fail "calibration exit"
}

run_failure_case() {
    local state="$TMP/failure-state" report="$TMP/failure.md"
    mkdir -p "$state"
    set +e
    MOCK_STATE_DIR="$state" MOCK_FAIL_DAC=30000 \
        "$RUNNER" --cli "$MOCK" --report "$report" --port /dev/mock --timeout 10 \
        >/dev/null 2>&1
    local rc=$?
    set -e

    (( rc != 0 )) || fail "injected failure returned success"
    grep -q '^# DAC Sweep Test Report — FAIL$' "$report" || fail "failure heading"
    grep -q 'raw fc06 71 0' "$state/commands.log" || fail "failure DAC cleanup"
    grep -q 'raw fc06 70 0' "$state/commands.log" || fail "failure output cleanup"
    grep -q 'raw fc06 681 1' "$state/commands.log" || fail "failure calibration exit"
}

[[ -x "$RUNNER" ]] || fail "runner not executable: $RUNNER"
run_success_case
run_failure_case
echo "PASS: DAC sweep script regression tests"
