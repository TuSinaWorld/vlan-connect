#!/usr/bin/env bash

set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$TEST_DIR/../.." && pwd)"
INSTALLER="$ROOT_DIR/server/install.sh"
FAILURES=0

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    FAILURES=$((FAILURES + 1))
}

assert_equal() {
    local expected="$1"
    local actual="$2"
    local label="$3"
    [[ "$expected" == "$actual" ]] ||
        fail "$label (expected '$expected', got '$actual')"
}

assert_success() {
    local label="$1"
    shift
    "$@" || fail "$label"
}

assert_failure() {
    local label="$1"
    shift
    if "$@"; then
        fail "$label"
    fi
}

bash -n "$INSTALLER" || fail "installer must pass bash -n"
if command -v shellcheck >/dev/null 2>&1; then
    shellcheck "$INSTALLER" || fail "installer must pass ShellCheck"
else
    fail "ShellCheck is required for installer tests"
fi

# shellcheck source=../../server/install.sh
source "$INSTALLER"

assert_success "minimum port" is_uint_in_range 1 1 65535
assert_success "maximum port" is_uint_in_range 65535 1 65535
assert_failure "zero port" is_uint_in_range 0 1 65535
assert_failure "non-numeric port" is_uint_in_range 11x 1 65535
assert_success "stable semantic tag" is_stable_version v1.2.30
assert_failure "pre-release tag excluded" is_stable_version v1.2.3-rc1
assert_failure "malformed tag excluded" is_stable_version release-1.2.3

tag_lines=$'a\trefs/tags/v0.9.9\nb\trefs/tags/v0.10.0\nc\trefs/tags/v1.0.0-rc1\nd\trefs/tags/not-a-version'
latest_tag="$(printf '%s\n' "$tag_lines" | select_latest_tag_from_lines)"
assert_equal "v0.10.0" "$latest_tag" "latest stable tag selection"
assert_success "version comparison" version_is_greater v0.10.0 v0.9.9
assert_failure "equal version is not greater" version_is_greater v0.10.0 v0.10.0

assert_equal "apt" "$(classify_distro ubuntu '')" "Ubuntu family"
assert_equal "apt" "$(classify_distro linuxmint 'ubuntu debian')" "Debian ID_LIKE"
assert_equal "rpm" "$(classify_distro rocky 'rhel fedora')" "Rocky family"
assert_failure "unsupported distribution" classify_distro arch arch

assert_success "8-byte password" validate_password_value 12345678
assert_failure "7-byte password" validate_password_value 1234567
assert_failure "all-whitespace password" validate_password_value $'        '
assert_failure "newline in password" validate_password_value $'12345678\n'

tmp_dir="$(mktemp -d)"
trap 'rm -rf -- "$tmp_dir"' EXIT

archive_root="$tmp_dir/vlan-connect-v1.2.3"
install -d "$archive_root/server"
printf 'all:\n' > "$archive_root/server/Makefile"
printf '[Unit]\n' > "$archive_root/server/vlan-server.service"
printf '# Deploy\n' > "$archive_root/server/DEPLOY.md"
valid_archive="$tmp_dir/vlan-connect-source-v1.2.3.tar.gz"
tar -C "$tmp_dir" -czf "$valid_archive" "$(basename "$archive_root")"
archive_metadata="$(inspect_source_archive "$valid_archive")"
assert_equal $'vlan-connect-v1.2.3\tv1.2.3' "$archive_metadata" \
    "local source archive metadata"

invalid_root="$tmp_dir/vlan-connect-1.2.3"
cp -a "$archive_root" "$invalid_root"
invalid_root_archive="$tmp_dir/invalid-root.tar.gz"
tar -C "$tmp_dir" -czf "$invalid_root_archive" "$(basename "$invalid_root")"
assert_failure "archive root requires vX.Y.Z" \
    inspect_source_archive "$invalid_root_archive"

printf 'extra\n' > "$tmp_dir/extra-root"
multi_root_archive="$tmp_dir/multi-root.tar.gz"
tar -C "$tmp_dir" -czf "$multi_root_archive" \
    "$(basename "$archive_root")" "$(basename "$tmp_dir/extra-root")"
assert_failure "archive must have one top-level directory" \
    inspect_source_archive "$multi_root_archive"

printf 'escape\n' > "$tmp_dir/escape"
traversal_archive="$tmp_dir/traversal.tar.gz"
tar -C "$tmp_dir" -czf "$traversal_archive" \
    --transform='s|^escape$|vlan-connect-v1.2.3/../escape|' \
    "$(basename "$archive_root")" escape
assert_failure "archive path traversal is rejected" \
    inspect_source_archive "$traversal_archive"

ln -s server/Makefile "$archive_root/link"
linked_archive="$tmp_dir/linked.tar.gz"
tar -C "$tmp_dir" -czf "$linked_archive" "$(basename "$archive_root")"
assert_failure "archive links are rejected" inspect_source_archive "$linked_archive"
rm -f -- "$archive_root/link"

SOURCE_ARCHIVE_OPTION="$valid_archive"
VERSION_OPTION=""
CURRENT_VERSION=""
TARGET_VERSION=""
WORK_DIR=""
SOURCE_DIR=""
SOURCE_ARCHIVE_CACHE=""
SOURCE_ARCHIVE_ROOT=""
git() { return 99; }
assert_success "local archive resolution bypasses git" resolve_target_version
assert_equal "v1.2.3" "$TARGET_VERSION" "local archive detected version"
assert_equal "vlan-connect-v1.2.3" "$SOURCE_ARCHIVE_ROOT" \
    "local archive detected root"
[[ -f "$SOURCE_ARCHIVE_CACHE" ]] || fail "local archive must be cached before deployment"
make_calls="$tmp_dir/make-calls"
make() {
    printf '%s\n' "$*" >> "$make_calls"
    printf '#!/bin/sh\nexit 0\n' > "$SOURCE_DIR/server/vlan-server"
    chmod 0755 "$SOURCE_DIR/server/vlan-server"
}
assert_success "local archive build extracts cached source" build_release
grep -Fqx -- "-C $SOURCE_DIR/server" "$make_calls" ||
    fail "local archive build must invoke the server Makefile"
unset -f make
rm -rf -- "$WORK_DIR"
WORK_DIR=""
SOURCE_DIR=""
SOURCE_ARCHIVE_CACHE=""
SOURCE_ARCHIVE_ROOT=""
SOURCE_ARCHIVE_OPTION=""
unset -f git

git_calls="$tmp_dir/git-calls"
git() {
    printf '%s\n' "$*" >> "$git_calls"
    printf 'deadbeef\trefs/tags/v9.8.7\n'
}
VERSION_OPTION=""
CURRENT_VERSION=""
TARGET_VERSION=""
assert_success "default version resolution still queries git" resolve_target_version
assert_equal "v9.8.7" "$TARGET_VERSION" "default remote version selection"
grep -Fqx "ls-remote --tags --refs $REPO_URL" "$git_calls" ||
    fail "default version resolution must query the official repository"
unset -f git

printf '12345678\r\n\n' > "$tmp_dir/password-valid"
PASSWORD_VALUE=""
assert_success "valid password file" load_password_file "$tmp_dir/password-valid"
assert_equal "12345678" "$PASSWORD_VALUE" "loaded password"
printf '12345678\nextra\n' > "$tmp_dir/password-extra-line"
assert_failure "extra password line" load_password_file "$tmp_dir/password-extra-line"
printf '12345678\0tail' > "$tmp_dir/password-nul"
assert_failure "NUL password" load_password_file "$tmp_dir/password-nul"

marker="$tmp_dir/should-not-exist"
cat > "$tmp_dir/server.env" <<EOF
# test configuration
VLAN_SERVER_PORT=12000
VLAN_SERVER_LOG_MAX_MB=20
VLAN_SERVER_AUTH_FILE=/tmp/auth.password
VLAN_SERVER_EXTRA_ARGS=--max-clients 100 --max-rooms 50
IGNORED_KEY=\$(touch "$marker")
EOF
parse_known_env_file "$tmp_dir/server.env"
assert_equal "12000" "${EXISTING_ENV[VLAN_SERVER_PORT]}" "known env parsing"
[[ ! -e "$marker" ]] || fail "env parser must not execute file content"

MAX_CLIENTS=""
MAX_PENDING=""
MAX_ROOMS=""
MAX_CLIENTS_PER_IP=""
MAX_PENDING_PER_IP=""
MAX_SEND_BUFFER_MB=""
assert_success "safe existing extra args" \
    parse_existing_extra_args "--max-clients 100 --max-rooms 50"
assert_equal "100" "$MAX_CLIENTS" "parsed max clients"
assert_equal "50" "$MAX_ROOMS" "parsed max rooms"
assert_failure "unknown existing extra args" \
    parse_existing_extra_args "--unknown 1"

PORT=11510
PORT_OPTION=13000
LOG_MAX_MB=10
LOG_MAX_OPTION=22
MAX_CLIENTS_OPTION=""
MAX_PENDING_OPTION=""
MAX_ROOMS_OPTION=""
MAX_CLIENTS_PER_IP_OPTION=""
MAX_PENDING_PER_IP_OPTION=""
MAX_SEND_BUFFER_MB_OPTION=""
apply_option_overrides
assert_equal "13000" "$PORT" "CLI/env port precedence"
assert_equal "22" "$LOG_MAX_MB" "CLI/env log precedence"

ROLLBACK_DIR="$tmp_dir/rollback"
install -d "$ROLLBACK_DIR"
original_path="$tmp_dir/config"
printf 'original\n' > "$original_path"
snapshot_path config "$original_path"
printf 'changed\n' > "$original_path"
restore_path config "$original_path"
assert_equal "original" "$(tr -d '\r\n' < "$original_path")" "file rollback"

rollback_root="$tmp_dir/full-rollback"
INSTALL_ROOT="$rollback_root/opt"
RELEASES_DIR="$INSTALL_ROOT/releases"
CURRENT_LINK="$INSTALL_ROOT/current"
BIN_PATH="$rollback_root/usr/local/bin/vlan-server"
ENV_PATH="$rollback_root/usr/local/bin/vlan-server.env"
AUTH_PATH="$rollback_root/usr/local/bin/auth.password"
SERVICE_UNIT="$rollback_root/etc/systemd/system/vlan-server.service"
DOC_PATH="$rollback_root/usr/local/share/doc/vlan-server/DEPLOY.md"
STATE_DIR="$rollback_root/var/lib/vlan-server"
VERSION_FILE="$STATE_DIR/installed-version"
TARGET_VERSION="v1.2.3"
ROLLBACK_DIR="$tmp_dir/full-rollback-snapshot"
service_events="$tmp_dir/systemctl-events"
systemctl() { printf '%s\n' "$*" >> "$service_events"; return 0; }
install -d "$RELEASES_DIR/$TARGET_VERSION" "$(dirname "$BIN_PATH")" \
    "$(dirname "$SERVICE_UNIT")" "$(dirname "$DOC_PATH")" "$STATE_DIR"
printf 'old-binary\n' > "$RELEASES_DIR/$TARGET_VERSION/vlan-server"
ln -s "$RELEASES_DIR/$TARGET_VERSION" "$CURRENT_LINK"
ln -s "$CURRENT_LINK/vlan-server" "$BIN_PATH"
printf 'old-env\n' > "$ENV_PATH"
printf 'old-password\n' > "$AUTH_PATH"
printf 'old-unit\n' > "$SERVICE_UNIT"
printf 'old-doc\n' > "$DOC_PATH"
printf '%s\n' "$TARGET_VERSION" > "$VERSION_FILE"
PREVIOUS_SERVICE_ENABLED=0
PREVIOUS_SERVICE_ACTIVE=0
snapshot_deployment
printf 'new-env\n' > "$ENV_PATH"
printf 'new-password\n' > "$AUTH_PATH"
printf 'new-unit\n' > "$SERVICE_UNIT"
printf 'new-binary\n' > "$RELEASES_DIR/$TARGET_VERSION/vlan-server"
DEPLOYMENT_ACTIVE=1
restore_deployment
assert_equal "old-env" "$(tr -d '\r\n' < "$ENV_PATH")" "full env rollback"
assert_equal "old-password" "$(tr -d '\r\n' < "$AUTH_PATH")" "full password rollback"
assert_equal "old-unit" "$(tr -d '\r\n' < "$SERVICE_UNIT")" "full unit rollback"
assert_equal "old-binary" \
    "$(tr -d '\r\n' < "$RELEASES_DIR/$TARGET_VERSION/vlan-server")" \
    "full binary rollback"
grep -qx 'restart vlan-server' "$service_events" ||
    fail "rollback must restart a previously active service"

events="$tmp_dir/events"
create_work_dir() { printf 'create\n' >> "$events"; }
build_release() { printf 'build\n' >> "$events"; return 1; }
deploy_release() { printf 'deploy\n' >> "$events"; }
assert_failure "build failure must propagate" execute_release_update
assert_equal $'create\nbuild' "$(tr -d '\r' < "$events")" \
    "deployment must not start after build failure"

if grep -Eq '(^|[[:space:]])(ufw|firewall-cmd)([[:space:]]|$)' "$INSTALLER"; then
    fail "installer must not execute firewall management commands"
fi

if (( FAILURES != 0 )); then
    printf 'install_script_tests: %d failure(s)\n' "$FAILURES" >&2
    exit 1
fi

printf 'install_script_tests: ok\n'
