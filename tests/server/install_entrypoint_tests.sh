#!/usr/bin/env bash

set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALLER="$TEST_DIR/../../server/install.sh"

assert_output() {
    local expected="$1" label="$2" actual
    shift 2
    if ! actual="$("$@" 2>&1)"; then
        printf 'FAIL: %s exited unsuccessfully: %s\n' "$label" "$actual" >&2
        exit 1
    fi
    if [[ "$actual" != "$expected" ]]; then
        printf 'FAIL: %s produced unexpected output: %s\n' "$label" "$actual" >&2
        exit 1
    fi
}

# --help returns before root checks, downloads or deployment in every mode.
expected_help="$(bash "$INSTALLER" --help)"
[[ "$expected_help" == VLan\ server\ installer/updater* ]]
assert_output "$expected_help" "redirected stdin" \
    bash -c 'bash -s -- --help < "$1"' bash "$INSTALLER"
assert_output "$expected_help" "piped stdin with script arguments" \
    bash -o pipefail -c 'cat "$1" | bash -s -- --help' bash "$INSTALLER"
assert_output "$expected_help" "piped stdin without Bash options" \
    bash -o pipefail -c '{ printf "set -- --help\n"; cat "$1"; } | bash' bash "$INSTALLER"
assert_output "$expected_help" "stdin action and option forwarding" \
    bash -o pipefail -c 'cat "$1" | bash -s -- update --non-interactive --help' bash "$INSTALLER"

# Sourcing loads functions without executing main or replacing the caller's trap.
assert_output $'sourced\ncaller-trap-preserved' "sourced library" \
    bash -c '
        trap '\''printf "%s\n" caller-trap-preserved'\'' EXIT
        source "$1" --help
        declare -F main >/dev/null
        printf "%s\n" sourced
    ' bash "$INSTALLER"

printf 'install_entrypoint_tests: ok\n'
