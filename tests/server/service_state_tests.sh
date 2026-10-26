#!/usr/bin/env bash

set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT_DIR="$(cd "$TEST_DIR/../.." && pwd -P)"
INSTALLER="$ROOT_DIR/server/install.sh"
FIXTURE_ROOT="$ROOT_DIR/build-tests"
mkdir -p -- "$FIXTURE_ROOT"
[[ "$(cd "$FIXTURE_ROOT" && pwd -P)" == "$FIXTURE_ROOT" ]] || {
    printf 'FAIL: fixture root must resolve inside the workspace\n' >&2
    exit 1
}
FIXTURE_DIR="$(mktemp -d "$FIXTURE_ROOT/service-state.XXXXXX")"

cleanup_service_fixtures() {
    local resolved
    [[ -d "$FIXTURE_DIR" ]] || return 0
    resolved="$(cd "$FIXTURE_DIR" && pwd -P)" || return
    case "$resolved/" in
    "$FIXTURE_ROOT"/service-state.*/)
        rm -rf -- "$resolved"
        ;;
    *)
        printf 'Refusing to remove unexpected fixture directory: %s\n' "$resolved" >&2
        return 1
        ;;
    esac
}

# Loading the installer must not run its deployment entrypoint. The trap name is
# distinct from its cleanup() function so sourcing cannot replace this cleanup.
# shellcheck source=../../server/install.sh
source "$INSTALLER" --help
trap cleanup_service_fixtures EXIT
FAILURES=0
MOCK_STATE=""
MOCK_ERROR=""
MOCK_STATUS=0
SYSTEMCTL_EVENTS="$FIXTURE_DIR/systemctl-events"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    FAILURES=$((FAILURES + 1))
}

# Every test intercepts systemctl. No host service may be enabled, started,
# stopped, or otherwise changed, even if a tested helper regresses.
systemctl() {
    printf '%s\n' "$*" >> "$SYSTEMCTL_EVENTS"
    if [[ "$*" != "show --no-pager --all --property=LoadState --property=ActiveState --property=UnitFileState $SERVICE_NAME" ]]; then
        printf 'Unexpected systemctl invocation: %s\n' "$*" >&2
        return 99
    fi
    [[ -z "$MOCK_STATE" ]] || printf '%s\n' "$MOCK_STATE"
    [[ -z "$MOCK_ERROR" ]] || printf '%s\n' "$MOCK_ERROR" >&2
    return "$MOCK_STATUS"
}

run_success() {
    local label="$1" state="$2" enabled="$3" active="$4"
    local output="$FIXTURE_DIR/$label.stdout"
    local errors="$FIXTURE_DIR/$label.stderr"
    local flags="$FIXTURE_DIR/$label.flags"
    MOCK_STATE="$state"
    MOCK_ERROR=""
    MOCK_STATUS=0
    if ! (
        snapshot_service_state
        printf '%s,%s\n' "$PREVIOUS_SERVICE_ENABLED" "$PREVIOUS_SERVICE_ACTIVE" > "$flags"
    ) > "$output" 2> "$errors"; then
        fail "$label unexpectedly failed: $(cat "$errors")"
        return
    fi
    [[ "$(cat "$flags")" == "$enabled,$active" ]] ||
        fail "$label lost rollback state (expected $enabled,$active; got $(cat "$flags"))"
    [[ ! -s "$errors" ]] || fail "$label wrote unexpected stderr: $(cat "$errors")"
    [[ -s "$output" ]] || fail "$label must produce a normal informational message"
}

run_failure() {
    local label="$1" state="$2" diagnostic="$3" status="$4"
    local output="$FIXTURE_DIR/$label.stdout"
    local errors="$FIXTURE_DIR/$label.stderr"
    MOCK_STATE="$state"
    MOCK_ERROR="$diagnostic"
    MOCK_STATUS="$status"
    if (
        snapshot_service_state
        printf '%s\n' UNEXPECTED-FOLLOWUP
    ) > "$output" 2> "$errors"; then
        fail "$label must stop on an unreliable service-state query"
    fi
    if grep -Fq UNEXPECTED-FOLLOWUP "$output"; then
        fail "$label continued after its failed query"
    fi
    [[ -s "$errors" ]] || fail "$label did not explain the failure"
    if [[ -n "$diagnostic" ]] && ! grep -Fq "$diagnostic" "$errors"; then
        fail "$label hid the original systemctl diagnostic"
    fi
}

fresh_state=$'LoadState=not-found\nActiveState=inactive\nUnitFileState='
PREVIOUS_SERVICE_ENABLED=1
PREVIOUS_SERVICE_ACTIVE=1
run_success first-install "$fresh_state" 0 0
grep -Fq 'No previous VLan service found; a new service will be installed.' \
    "$FIXTURE_DIR/first-install.stdout" ||
    fail "first installation must explain the expected missing unit without an error"

run_success enabled-active $'LoadState=loaded\nActiveState=active\nUnitFileState=enabled' 1 1
run_success enabled-inactive $'LoadState=loaded\nActiveState=inactive\nUnitFileState=enabled' 1 0
run_success disabled-active $'LoadState=loaded\nActiveState=active\nUnitFileState=disabled' 0 1
run_success disabled-inactive $'LoadState=loaded\nActiveState=inactive\nUnitFileState=disabled' 0 0

# Installation detection knows this installer's own paths, not every systemd
# unit search directory. An existing vendor/runtime unit still needs a snapshot.
INSTALL_MODE=install
SERVICE_UNIT="$FIXTURE_DIR/nonexistent/unit/vlan-server.service"
[[ ! -e "$SERVICE_UNIT" ]] || fail "elsewhere-unit fixture unexpectedly exists"
run_success elsewhere-unit $'LoadState=loaded\nActiveState=active\nUnitFileState=enabled' 1 1
grep -Fq 'load=loaded, state=active, autostart=enabled; saving state for rollback.' \
    "$FIXTURE_DIR/elsewhere-unit.stdout" ||
    fail "existing-service information must identify load, runtime and autostart state"
run_success missing-but-active $'LoadState=not-found\nActiveState=active\nUnitFileState=' 0 1
if grep -Fq 'No previous VLan service found' "$FIXTURE_DIR/missing-but-active.stdout"; then
    fail "a still-active unit with a missing file must not be called a fresh installation"
fi

for state in enabled enabled-runtime alias static indirect generated transient; do
    run_success "autostart-$state" \
        "$(printf 'LoadState=loaded\nActiveState=inactive\nUnitFileState=%s' "$state")" 1 0
done
for state in disabled linked linked-runtime masked masked-runtime bad not-found ''; do
    run_success "manual-${state:-empty}" \
        "$(printf 'LoadState=loaded\nActiveState=inactive\nUnitFileState=%s' "$state")" 0 0
done
run_success reloading $'LoadState=loaded\nActiveState=reloading\nUnitFileState=enabled' 1 1
run_success refreshing $'LoadState=loaded\nActiveState=refreshing\nUnitFileState=disabled' 0 1
run_success failed-service $'LoadState=loaded\nActiveState=failed\nUnitFileState=enabled' 1 0

# Bad existing configuration is eligible for the installer's repair path; a
# valid query must still preserve its independent autostart/runtime state.
run_success repair-load-error $'LoadState=error\nActiveState=inactive\nUnitFileState=disabled' 0 0
run_success repair-bad-setting $'LoadState=bad-setting\nActiveState=active\nUnitFileState=enabled' 1 1

# Multiple snapshots in one process may not retain stale true flags.
MOCK_ERROR=""
MOCK_STATUS=0
if ! (
    MOCK_STATE=$'LoadState=loaded\nActiveState=active\nUnitFileState=enabled'
    snapshot_service_state
    [[ "$PREVIOUS_SERVICE_ENABLED,$PREVIOUS_SERVICE_ACTIVE" == 1,1 ]] || exit 1
    MOCK_STATE="$fresh_state"
    snapshot_service_state
    [[ "$PREVIOUS_SERVICE_ENABLED,$PREVIOUS_SERVICE_ACTIVE" == 0,0 ]] || exit 1
) > "$FIXTURE_DIR/repeated.stdout" 2> "$FIXTURE_DIR/repeated.stderr"; then
    fail "repeated snapshots did not reset rollback flags"
fi
[[ ! -s "$FIXTURE_DIR/repeated.stderr" ]] || fail "repeated snapshots produced unexpected stderr"

run_failure bus-error '' 'Failed to connect to bus: Connection refused' 1
run_failure permission-error '' 'Failed to get properties: Access denied' 1
run_failure missing-load $'ActiveState=inactive\nUnitFileState=disabled' '' 0
run_failure missing-active $'LoadState=loaded\nUnitFileState=disabled' '' 0
run_failure missing-unit-file-state $'LoadState=loaded\nActiveState=inactive' '' 0
run_failure empty-load $'LoadState=\nActiveState=inactive\nUnitFileState=disabled' '' 0
run_failure empty-active $'LoadState=loaded\nActiveState=\nUnitFileState=disabled' '' 0
run_failure empty-output '' '' 0
run_failure unknown-autostart $'LoadState=loaded\nActiveState=inactive\nUnitFileState=unknown-state' '' 0

# Linux CI hosts with a live system manager can additionally prove that a real
# missing unit is a quiet structured query. No unit is created or changed, and
# the wrapper rejects every command except this exact read-only show invocation.
if [[ -d /run/systemd/system ]] &&
   (unset -f systemctl; command -v systemctl >/dev/null 2>&1); then
    if ! (
        SERVICE_NAME="vlan-installer-missing-state-probe-${BASHPID}.service"
        systemctl() {
            if [[ "$*" != "show --no-pager --all --property=LoadState --property=ActiveState --property=UnitFileState $SERVICE_NAME" ]]; then
                printf 'Refusing non-probe systemctl command: %s\n' "$*" >&2
                return 99
            fi
            command systemctl "$@"
        }
        PREVIOUS_SERVICE_ENABLED=1
        PREVIOUS_SERVICE_ACTIVE=1
        snapshot_service_state
        printf '%s,%s\n' "$PREVIOUS_SERVICE_ENABLED" "$PREVIOUS_SERVICE_ACTIVE" \
            > "$FIXTURE_DIR/real-systemd.flags"
    ) > "$FIXTURE_DIR/real-systemd.stdout" 2> "$FIXTURE_DIR/real-systemd.stderr"; then
        fail "real missing-unit query failed: $(cat "$FIXTURE_DIR/real-systemd.stderr")"
    else
        [[ "$(cat "$FIXTURE_DIR/real-systemd.flags")" == 0,0 ]] ||
            fail "real missing-unit query must reset rollback flags"
        [[ ! -s "$FIXTURE_DIR/real-systemd.stderr" ]] ||
            fail "real missing-unit query wrote unexpected stderr: $(cat "$FIXTURE_DIR/real-systemd.stderr")"
        grep -Fq 'No previous VLan service found; a new service will be installed.' \
            "$FIXTURE_DIR/real-systemd.stdout" ||
            fail "real missing-unit query omitted the normal first-install message"
    fi
fi

if (( FAILURES != 0 )); then
    printf 'service_state_tests: %d failure(s)\n' "$FAILURES" >&2
    exit 1
fi
printf 'service_state_tests: ok\n'
