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

assert_png() {
    local path=$1 signature
    [[ -s "$path" ]] || fail "missing PNG: $path"
    signature="$(od -An -tx1 -N8 "$path" | tr -d ' \n')"
    [[ "$signature" == "89504e470d0a1a0a" ]] || fail "invalid PNG signature: $path"
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
    grep -Fq '| Raw ADC V | raw_adc = 10.000000 × DAC + 0.000 | 1.000000 | 0.000 | 0.000000% | 7 |' "$report" || fail "voltage linearity fit"
    grep -Fq '| Raw ADC I | raw_adc = -10.000000 × DAC + 0.000 | 1.000000 | 0.000 | 0.000000% | 7 |' "$report" || fail "current linearity fit"
    test "$(grep -c '^### Linearity Fit$' "$report")" -eq 1 || fail "fit section capability gating"
    grep -Fq '![CH0 DAC sweep plot](success_ch0.png)' "$report" || fail "CH0 plot link"
    grep -Fq '![CH1 DAC sweep plot](success_ch1.png)' "$report" || fail "CH1 plot link"
    assert_png "$TMP/success_ch0.png"
    assert_png "$TMP/success_ch1.png"
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

run_constant_series_case() {
    local state="$TMP/constant-state" report="$TMP/constant.md"
    mkdir -p "$state"
    MOCK_STATE_DIR="$state" MOCK_CONSTANT_RAW_V=1 \
        "$RUNNER" --cli "$MOCK" --report "$report" --port /dev/mock --timeout 10 \
        >/dev/null

    grep -Fq '| Raw ADC V | raw_adc = 0.000000 × DAC + 1234.000 | N/A | 0.000 | N/A | 7 |' "$report" || fail "constant-series fit"
}

[[ -x "$RUNNER" ]] || fail "runner not executable: $RUNNER"
run_success_case
run_failure_case
run_constant_series_case
echo "PASS: DAC sweep script regression tests"
