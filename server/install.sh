#!/usr/bin/env bash

set -Eeuo pipefail
umask 077

REPO_URL="https://github.com/TuSinaWorld/vlan-connect.git"
SERVICE_NAME="vlan-server"
SERVICE_UNIT="/etc/systemd/system/vlan-server.service"
INSTALL_ROOT="/opt/vlan-server"
RELEASES_DIR="${INSTALL_ROOT}/releases"
CURRENT_LINK="${INSTALL_ROOT}/current"
BIN_PATH="/usr/local/bin/vlan-server"
ENV_PATH="/usr/local/bin/vlan-server.env"
AUTH_PATH="/usr/local/bin/auth.password"
DOC_PATH="/usr/local/share/doc/vlan-server/DEPLOY.md"
STATE_DIR="/var/lib/vlan-server"
LOG_DIR="/var/log/vlan-server"
VERSION_FILE="${STATE_DIR}/installed-version"
LOCK_FILE="/run/lock/vlan-server-installer.lock"
LOCK_OWNED=0

DEFAULT_PORT=11510
DEFAULT_LOG_MAX_MB=10

ACTION="auto"
TARGET_VERSION=""
NON_INTERACTIVE=0
RECONFIGURE=0
SHOW_HELP=0
CONFIG_OVERRIDE=0

VERSION_OPTION="${VLAN_INSTALL_VERSION-}"
PORT_OPTION="${VLAN_INSTALL_PORT-}"
PASSWORD_FILE_OPTION="${VLAN_INSTALL_PASSWORD_FILE-}"
PASSWORD_DIRECT_OPTION="${VLAN_INSTALL_PASSWORD-}"
LOG_MAX_OPTION="${VLAN_INSTALL_LOG_MAX_MB-}"
MAX_CLIENTS_OPTION="${VLAN_INSTALL_MAX_CLIENTS-}"
MAX_PENDING_OPTION="${VLAN_INSTALL_MAX_PENDING-}"
MAX_ROOMS_OPTION="${VLAN_INSTALL_MAX_ROOMS-}"
MAX_CLIENTS_PER_IP_OPTION="${VLAN_INSTALL_MAX_CLIENTS_PER_IP-}"
MAX_PENDING_PER_IP_OPTION="${VLAN_INSTALL_MAX_PENDING_PER_IP-}"
MAX_SEND_BUFFER_MB_OPTION="${VLAN_INSTALL_MAX_SEND_BUFFER_MB-}"
unset VLAN_INSTALL_PASSWORD

PORT=""
PASSWORD_VALUE=""
GENERATED_PASSWORD=""
LOG_MAX_MB=""
MAX_CLIENTS=""
MAX_PENDING=""
MAX_ROOMS=""
MAX_CLIENTS_PER_IP=""
MAX_PENDING_PER_IP=""
MAX_SEND_BUFFER_MB=""
EXISTING_AUTH_PATH=""
CURRENT_VERSION=""
INSTALL_MODE=""
WORK_DIR=""
SOURCE_DIR=""
ROLLBACK_DIR=""
DEPLOYMENT_ACTIVE=0
PREVIOUS_SERVICE_ENABLED=0
PREVIOUS_SERVICE_ACTIVE=0
INTERACTIVE=0

declare -A EXISTING_ENV=()

log() {
    printf '[vlan-install] %s\n' "$*"
}

warn() {
    printf '[vlan-install] WARNING: %s\n' "$*" >&2
}

die() {
    printf '[vlan-install] ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
VLan server installer/updater

Usage:
  install.sh [auto|install|update] [options]

Actions:
  auto                         Install or update automatically (default)
  install                      Require a fresh installation
  update                       Require an existing installation

Options:
  --version vX.Y.Z             Install a specific stable tag
  --port N                     TCP and UDP listen port (1-65535)
  --password-file FILE         Read the server password from FILE
  --non-interactive            Do not prompt; use defaults when unset
  --reconfigure                Prompt for configuration during an update
  --log-max-mb N               Maximum application log size in MiB
  --max-clients N              Signaling clients (1-256)
  --max-pending N              Unclassified clients (1-64)
  --max-rooms N                Rooms (1-128)
  --max-clients-per-ip N       Clients per IPv4 (1-32)
  --max-pending-per-ip N       Pending clients per IPv4 (1-8)
  --max-send-buffer-mb N       Global send buffers in MiB (1-64)
  -h, --help                   Show this help

Environment equivalents:
  VLAN_INSTALL_VERSION, VLAN_INSTALL_PORT,
  VLAN_INSTALL_PASSWORD_FILE, VLAN_INSTALL_PASSWORD,
  VLAN_INSTALL_LOG_MAX_MB, VLAN_INSTALL_MAX_CLIENTS,
  VLAN_INSTALL_MAX_PENDING, VLAN_INSTALL_MAX_ROOMS,
  VLAN_INSTALL_MAX_CLIENTS_PER_IP,
  VLAN_INSTALL_MAX_PENDING_PER_IP,
  VLAN_INSTALL_MAX_SEND_BUFFER_MB

For unattended deployment, prefer VLAN_INSTALL_PASSWORD_FILE or
--password-file over VLAN_INSTALL_PASSWORD so the secret is not inherited by
unrelated child processes.
EOF
}

option_requires_value() {
    [[ $# -ge 2 && -n "$2" ]] || die "$1 requires a value"
}

parse_args() {
    local action_seen=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
        auto|install|update)
            (( action_seen == 0 )) || die "only one action may be specified"
            ACTION="$1"
            action_seen=1
            shift
            ;;
        --version)
            option_requires_value "$1" "${2-}"
            VERSION_OPTION="$2"
            shift 2
            ;;
        --port)
            option_requires_value "$1" "${2-}"
            PORT_OPTION="$2"
            CONFIG_OVERRIDE=1
            shift 2
            ;;
        --password-file)
            option_requires_value "$1" "${2-}"
            PASSWORD_FILE_OPTION="$2"
            PASSWORD_DIRECT_OPTION=""
            CONFIG_OVERRIDE=1
            shift 2
            ;;
        --non-interactive)
            NON_INTERACTIVE=1
            shift
            ;;
        --reconfigure)
            RECONFIGURE=1
            CONFIG_OVERRIDE=1
            shift
            ;;
        --log-max-mb)
            option_requires_value "$1" "${2-}"
            LOG_MAX_OPTION="$2"
            CONFIG_OVERRIDE=1
            shift 2
            ;;
        --max-clients)
            option_requires_value "$1" "${2-}"
            MAX_CLIENTS_OPTION="$2"
            CONFIG_OVERRIDE=1
            shift 2
            ;;
        --max-pending)
            option_requires_value "$1" "${2-}"
            MAX_PENDING_OPTION="$2"
            CONFIG_OVERRIDE=1
            shift 2
            ;;
        --max-rooms)
            option_requires_value "$1" "${2-}"
            MAX_ROOMS_OPTION="$2"
            CONFIG_OVERRIDE=1
            shift 2
            ;;
        --max-clients-per-ip)
            option_requires_value "$1" "${2-}"
            MAX_CLIENTS_PER_IP_OPTION="$2"
            CONFIG_OVERRIDE=1
            shift 2
            ;;
        --max-pending-per-ip)
            option_requires_value "$1" "${2-}"
            MAX_PENDING_PER_IP_OPTION="$2"
            CONFIG_OVERRIDE=1
            shift 2
            ;;
        --max-send-buffer-mb)
            option_requires_value "$1" "${2-}"
            MAX_SEND_BUFFER_MB_OPTION="$2"
            CONFIG_OVERRIDE=1
            shift 2
            ;;
        -h|--help)
            SHOW_HELP=1
            shift
            ;;
        --)
            shift
            [[ $# -eq 0 ]] || die "unexpected positional arguments: $*"
            ;;
        *)
            die "unknown argument: $1"
            ;;
        esac
    done

    local env_override
    for env_override in \
        "$PORT_OPTION" "$PASSWORD_FILE_OPTION" "$PASSWORD_DIRECT_OPTION" \
        "$LOG_MAX_OPTION" "$MAX_CLIENTS_OPTION" "$MAX_PENDING_OPTION" \
        "$MAX_ROOMS_OPTION" "$MAX_CLIENTS_PER_IP_OPTION" \
        "$MAX_PENDING_PER_IP_OPTION" "$MAX_SEND_BUFFER_MB_OPTION"; do
        if [[ -n "$env_override" ]]; then
            CONFIG_OVERRIDE=1
        fi
    done
}

is_uint_in_range() {
    local value="$1"
    local minimum="$2"
    local maximum="$3"
    [[ "$value" =~ ^[0-9]+$ ]] || return 1
    (( ${#value} <= 10 )) || return 1
    local number=$((10#$value))
    (( number >= minimum && number <= maximum ))
}

is_positive_uint() {
    local value="$1"
    [[ "$value" =~ ^[0-9]+$ ]] || return 1
    (( ${#value} <= 9 )) || return 1
    (( 10#$value >= 1 ))
}

is_stable_version() {
    [[ "$1" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]
}

select_latest_tag_from_lines() {
    local line tag
    local -a tags=()
    while IFS= read -r line; do
        tag="${line##*/}"
        if is_stable_version "$tag"; then
            tags+=("$tag")
        fi
    done
    (( ${#tags[@]} > 0 )) || return 1
    printf '%s\n' "${tags[@]}" | LC_ALL=C sort -V | tail -n 1
}

version_is_greater() {
    local left="$1"
    local right="$2"
    [[ "$left" != "$right" ]] || return 1
    [[ "$(printf '%s\n%s\n' "$left" "$right" | LC_ALL=C sort -V | tail -n 1)" == "$left" ]]
}

classify_distro() {
    local distro_id="${1,,}"
    local distro_like=" ${2,,} "
    case "$distro_id" in
    debian|ubuntu) printf 'apt\n'; return 0 ;;
    rhel|centos|rocky|almalinux|fedora|ol) printf 'rpm\n'; return 0 ;;
    esac
    if [[ "$distro_like" == *" debian "* || "$distro_like" == *" ubuntu "* ]]; then
        printf 'apt\n'
        return 0
    fi
    if [[ "$distro_like" == *" rhel "* || "$distro_like" == *" fedora "* ||
          "$distro_like" == *" centos "* ]]; then
        printf 'rpm\n'
        return 0
    fi
    return 1
}

validate_password_value() {
    local password="$1"
    local length
    [[ "$password" != *$'\r'* && "$password" != *$'\n'* ]] || return 1
    length="$(LC_ALL=C printf '%s' "$password" | wc -c)"
    (( length >= 8 && length <= 256 )) || return 1
    [[ ! "$password" =~ ^[[:space:]]*$ ]]
}

load_password_file() {
    local path="$1"
    local first stripped
    PASSWORD_VALUE=""
    [[ -f "$path" && -r "$path" ]] || return 1
    if LC_ALL=C od -An -v -tx1 -- "$path" | grep -Eq '(^|[[:space:]])00([[:space:]]|$)'; then
        return 1
    fi
    first="$(LC_ALL=C head -n 1 -- "$path")"
    first="${first%%$'\r'*}"
    stripped="$(LC_ALL=C tr -d '\r\n' < "$path")"
    [[ "$stripped" == "$first" ]] || return 1
    validate_password_value "$first" || return 1
    PASSWORD_VALUE="$first"
}

generate_password() {
    local password
    [[ -r /dev/urandom ]] || die "/dev/urandom is unavailable"
    password="$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')"
    validate_password_value "$password" || die "secure password generation failed"
    PASSWORD_VALUE="$password"
    GENERATED_PASSWORD="$password"
}

parse_known_env_file() {
    local path="$1"
    local line key value
    EXISTING_ENV=()
    [[ -f "$path" ]] || return 0
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        [[ -z "$line" || "$line" =~ ^[[:space:]]*# ]] && continue
        [[ "$line" == *=* ]] || continue
        key="${line%%=*}"
        value="${line#*=}"
        case "$key" in
        VLAN_SERVER_PORT|VLAN_SERVER_LOG|VLAN_SERVER_LOG_MAX_MB|\
        VLAN_SERVER_AUTH_FILE|VLAN_SERVER_EXTRA_ARGS)
            EXISTING_ENV["$key"]="$value"
            ;;
        esac
    done < "$path"
}

parse_existing_extra_args() {
    local extra="$1"
    local -a tokens=()
    local index=0 flag value
    [[ -n "$extra" ]] || return 0
    read -r -a tokens <<< "$extra"
    while (( index < ${#tokens[@]} )); do
        flag="${tokens[$index]}"
        (( index + 1 < ${#tokens[@]} )) || return 1
        value="${tokens[$((index + 1))]}"
        case "$flag" in
        --max-clients)
            is_uint_in_range "$value" 1 256 || return 1
            MAX_CLIENTS="$value"
            ;;
        --max-pending)
            is_uint_in_range "$value" 1 64 || return 1
            MAX_PENDING="$value"
            ;;
        --max-rooms)
            is_uint_in_range "$value" 1 128 || return 1
            MAX_ROOMS="$value"
            ;;
        --max-clients-per-ip)
            is_uint_in_range "$value" 1 32 || return 1
            MAX_CLIENTS_PER_IP="$value"
            ;;
        --max-pending-per-ip)
            is_uint_in_range "$value" 1 8 || return 1
            MAX_PENDING_PER_IP="$value"
            ;;
        --max-send-buffer-mb)
            is_uint_in_range "$value" 1 64 || return 1
            MAX_SEND_BUFFER_MB="$value"
            ;;
        *)
            return 1
            ;;
        esac
        index=$((index + 2))
    done
}

require_root_and_systemd() {
    (( EUID == 0 )) || die "run this installer as root, for example: curl ... | sudo bash"
    command -v systemctl >/dev/null 2>&1 || die "systemctl is required"
    [[ -d /run/systemd/system ]] || die "this host is not running systemd"
}

acquire_lock() {
    local previous_pid=""
    local lock_candidate="${LOCK_FILE}.$$"
    install -d -m 0755 /run/lock
    printf '%s\n' "$$" > "$lock_candidate"
    if ! ln "$lock_candidate" "$LOCK_FILE" 2>/dev/null; then
        if [[ -f "$LOCK_FILE" ]]; then
            IFS= read -r previous_pid < "$LOCK_FILE" || true
        fi
        if [[ "$previous_pid" =~ ^[0-9]+$ ]] && ! kill -0 "$previous_pid" 2>/dev/null; then
            rm -f -- "$LOCK_FILE"
            if ! ln "$lock_candidate" "$LOCK_FILE" 2>/dev/null; then
                rm -f -- "$lock_candidate"
                die "another VLan installation or update is already running"
            fi
        else
            rm -f -- "$lock_candidate"
            die "another VLan installation or update is already running"
        fi
    fi
    rm -f -- "$lock_candidate"
    LOCK_OWNED=1
}

read_os_release_value() {
    local wanted="$1"
    local line key value
    while IFS= read -r line || [[ -n "$line" ]]; do
        [[ "$line" == *=* ]] || continue
        key="${line%%=*}"
        [[ "$key" == "$wanted" ]] || continue
        value="${line#*=}"
        if (( ${#value} >= 2 )) && [[ "${value:0:1}" == '"' && "${value: -1}" == '"' ]]; then
            value="${value:1:${#value}-2}"
        elif (( ${#value} >= 2 )) && [[ "${value:0:1}" == "'" && "${value: -1}" == "'" ]]; then
            value="${value:1:${#value}-2}"
        fi
        printf '%s\n' "$value"
        return 0
    done < /etc/os-release
    return 1
}

install_dependencies() {
    [[ -r /etc/os-release ]] || die "/etc/os-release is missing"
    local distro_id distro_like family package_manager
    distro_id="$(read_os_release_value ID || true)"
    distro_like="$(read_os_release_value ID_LIKE || true)"
    family="$(classify_distro "$distro_id" "$distro_like")" ||
        die "unsupported Linux distribution: ${distro_id:-unknown}"

    log "Installing required build dependencies for ${distro_id:-$family}"
    if [[ "$family" == "apt" ]]; then
        command -v apt-get >/dev/null 2>&1 || die "apt-get is unavailable"
        apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y \
            build-essential git ca-certificates
    else
        if command -v dnf >/dev/null 2>&1; then
            package_manager=dnf
        elif command -v yum >/dev/null 2>&1; then
            package_manager=yum
        else
            die "dnf or yum is required"
        fi
        "$package_manager" install -y \
            gcc gcc-c++ make git ca-certificates
    fi

    local command_name
    for command_name in git make g++ cc install sort od readlink; do
        command -v "$command_name" >/dev/null 2>&1 ||
            die "required command is unavailable after dependency installation: $command_name"
    done
}

detect_installation() {
    if [[ -e "$BIN_PATH" || -L "$BIN_PATH" || -f "$SERVICE_UNIT" ||
          -f "$VERSION_FILE" || -f "$ENV_PATH" ]]; then
        INSTALL_MODE="update"
    else
        INSTALL_MODE="install"
    fi

    case "$ACTION" in
    auto) ;;
    install)
        [[ "$INSTALL_MODE" == "install" ]] ||
            die "VLan is already installed; use the update action"
        ;;
    update)
        [[ "$INSTALL_MODE" == "update" ]] ||
            die "VLan is not installed; use the install action"
        ;;
    esac
}

read_current_version() {
    CURRENT_VERSION=""
    if [[ -f "$VERSION_FILE" ]]; then
        IFS= read -r CURRENT_VERSION < "$VERSION_FILE" || true
        CURRENT_VERSION="${CURRENT_VERSION%$'\r'}"
        if [[ -n "$CURRENT_VERSION" ]] && ! is_stable_version "$CURRENT_VERSION"; then
            warn "ignoring invalid installed-version metadata"
            CURRENT_VERSION=""
        fi
    fi
}

resolve_target_version() {
    local refs
    if [[ -n "$VERSION_OPTION" ]]; then
        is_stable_version "$VERSION_OPTION" ||
            die "invalid version '$VERSION_OPTION'; expected vX.Y.Z"
        git ls-remote --exit-code --tags "$REPO_URL" \
            "refs/tags/$VERSION_OPTION" >/dev/null 2>&1 ||
            die "tag '$VERSION_OPTION' does not exist in the official repository"
        TARGET_VERSION="$VERSION_OPTION"
        if [[ -n "$CURRENT_VERSION" ]] && version_is_greater "$CURRENT_VERSION" "$TARGET_VERSION"; then
            warn "explicit downgrade requested: $CURRENT_VERSION -> $TARGET_VERSION"
        fi
        return
    fi

    refs="$(git ls-remote --tags --refs "$REPO_URL")" ||
        die "failed to query release tags from $REPO_URL"
    TARGET_VERSION="$(printf '%s\n' "$refs" | select_latest_tag_from_lines)" ||
        die "the official repository has no stable vX.Y.Z tag"

    if [[ -n "$CURRENT_VERSION" ]] && version_is_greater "$CURRENT_VERSION" "$TARGET_VERSION"; then
        die "refusing automatic downgrade from $CURRENT_VERSION to $TARGET_VERSION; use --version explicitly"
    fi
}

open_tty_if_available() {
    if (( NON_INTERACTIVE == 0 )) && [[ -r /dev/tty && -w /dev/tty ]]; then
        exec 3<>/dev/tty
        INTERACTIVE=1
    fi
}

prompt_value() {
    local prompt="$1"
    local default_value="$2"
    local answer=""
    printf '%s [%s]: ' "$prompt" "$default_value" >&3
    IFS= read -r -u 3 answer || die "failed to read interactive input"
    if [[ -n "$answer" ]]; then
        printf '%s\n' "$answer"
    else
        printf '%s\n' "$default_value"
    fi
}

prompt_yes_no() {
    local prompt="$1"
    local answer=""
    printf '%s [y/N]: ' "$prompt" >&3
    IFS= read -r -u 3 answer || die "failed to read interactive input"
    [[ "$answer" =~ ^([yY]|[yY][eE][sS])$ ]]
}

prompt_new_password() {
    local first="" second=""
    printf 'Server authentication password (leave blank to generate): ' >&3
    IFS= read -r -s -u 3 first || die "failed to read password"
    printf '\n' >&3
    if [[ -z "$first" ]]; then
        generate_password
        return
    fi
    validate_password_value "$first" ||
        die "password must be 8-256 bytes, contain no CR/LF, and not be all whitespace"
    printf 'Confirm password: ' >&3
    IFS= read -r -s -u 3 second || die "failed to read password confirmation"
    printf '\n' >&3
    [[ "$first" == "$second" ]] || die "password confirmation does not match"
    PASSWORD_VALUE="$first"
}

load_existing_configuration() {
    parse_known_env_file "$ENV_PATH"
    PORT="${EXISTING_ENV[VLAN_SERVER_PORT]-$DEFAULT_PORT}"
    LOG_MAX_MB="${EXISTING_ENV[VLAN_SERVER_LOG_MAX_MB]-$DEFAULT_LOG_MAX_MB}"
    EXISTING_AUTH_PATH="${EXISTING_ENV[VLAN_SERVER_AUTH_FILE]-$AUTH_PATH}"

    is_uint_in_range "$PORT" 1 65535 || die "existing VLAN_SERVER_PORT is invalid"
    is_positive_uint "$LOG_MAX_MB" || die "existing VLAN_SERVER_LOG_MAX_MB is invalid"
    [[ "$EXISTING_AUTH_PATH" == /* ]] || die "existing VLAN_SERVER_AUTH_FILE must be absolute"

    MAX_CLIENTS=""
    MAX_PENDING=""
    MAX_ROOMS=""
    MAX_CLIENTS_PER_IP=""
    MAX_PENDING_PER_IP=""
    MAX_SEND_BUFFER_MB=""
    parse_existing_extra_args "${EXISTING_ENV[VLAN_SERVER_EXTRA_ARGS]-}" ||
        die "existing VLAN_SERVER_EXTRA_ARGS contains unsupported or invalid arguments"
}

apply_option_overrides() {
    [[ -z "$PORT_OPTION" ]] || PORT="$PORT_OPTION"
    [[ -z "$LOG_MAX_OPTION" ]] || LOG_MAX_MB="$LOG_MAX_OPTION"
    [[ -z "$MAX_CLIENTS_OPTION" ]] || MAX_CLIENTS="$MAX_CLIENTS_OPTION"
    [[ -z "$MAX_PENDING_OPTION" ]] || MAX_PENDING="$MAX_PENDING_OPTION"
    [[ -z "$MAX_ROOMS_OPTION" ]] || MAX_ROOMS="$MAX_ROOMS_OPTION"
    [[ -z "$MAX_CLIENTS_PER_IP_OPTION" ]] || MAX_CLIENTS_PER_IP="$MAX_CLIENTS_PER_IP_OPTION"
    [[ -z "$MAX_PENDING_PER_IP_OPTION" ]] || MAX_PENDING_PER_IP="$MAX_PENDING_PER_IP_OPTION"
    [[ -z "$MAX_SEND_BUFFER_MB_OPTION" ]] || MAX_SEND_BUFFER_MB="$MAX_SEND_BUFFER_MB_OPTION"
}

prompt_configuration() {
    (( INTERACTIVE == 1 )) || return 0
    if [[ -z "$PORT_OPTION" &&
          ( "$INSTALL_MODE" == "install" || "$RECONFIGURE" == 1 ) ]]; then
        PORT="$(prompt_value "TCP/UDP listen port" "$PORT")"
    fi

    if [[ -z "$PASSWORD_FILE_OPTION" && -z "$PASSWORD_DIRECT_OPTION" ]]; then
        if [[ "$INSTALL_MODE" == "install" || -z "$PASSWORD_VALUE" ]]; then
            prompt_new_password
        elif (( RECONFIGURE == 1 )) && prompt_yes_no "Change the server authentication password?"; then
            prompt_new_password
        fi
    fi

    if (( RECONFIGURE == 1 )) || [[ "$INSTALL_MODE" == "install" ]]; then
        if prompt_yes_no "Configure advanced log and capacity limits?"; then
            LOG_MAX_MB="$(prompt_value "Maximum log size in MiB" "$LOG_MAX_MB")"
            MAX_CLIENTS="$(prompt_value "Maximum signaling clients" "${MAX_CLIENTS:-256}")"
            MAX_PENDING="$(prompt_value "Maximum pending clients" "${MAX_PENDING:-64}")"
            MAX_ROOMS="$(prompt_value "Maximum rooms" "${MAX_ROOMS:-128}")"
            MAX_CLIENTS_PER_IP="$(prompt_value "Maximum clients per IPv4" "${MAX_CLIENTS_PER_IP:-32}")"
            MAX_PENDING_PER_IP="$(prompt_value "Maximum pending per IPv4" "${MAX_PENDING_PER_IP:-8}")"
            MAX_SEND_BUFFER_MB="$(prompt_value "Global send buffers in MiB" "${MAX_SEND_BUFFER_MB:-64}")"
        fi
    fi
}

resolve_password() {
    if [[ -n "$PASSWORD_FILE_OPTION" ]]; then
        load_password_file "$PASSWORD_FILE_OPTION" ||
            die "password file is unreadable or invalid: $PASSWORD_FILE_OPTION"
        return
    fi
    if [[ -n "$PASSWORD_DIRECT_OPTION" ]]; then
        validate_password_value "$PASSWORD_DIRECT_OPTION" ||
            die "VLAN_INSTALL_PASSWORD must be 8-256 bytes and not all whitespace"
        PASSWORD_VALUE="$PASSWORD_DIRECT_OPTION"
        return
    fi
    if [[ "$INSTALL_MODE" == "update" ]] && load_password_file "$EXISTING_AUTH_PATH"; then
        return
    fi
    PASSWORD_VALUE=""
}

validate_configuration() {
    is_uint_in_range "$PORT" 1 65535 || die "port must be 1-65535"
    is_positive_uint "$LOG_MAX_MB" || die "log-max-mb must be a positive integer"
    [[ -z "$MAX_CLIENTS" ]] || is_uint_in_range "$MAX_CLIENTS" 1 256 || die "max-clients must be 1-256"
    [[ -z "$MAX_PENDING" ]] || is_uint_in_range "$MAX_PENDING" 1 64 || die "max-pending must be 1-64"
    [[ -z "$MAX_ROOMS" ]] || is_uint_in_range "$MAX_ROOMS" 1 128 || die "max-rooms must be 1-128"
    [[ -z "$MAX_CLIENTS_PER_IP" ]] || is_uint_in_range "$MAX_CLIENTS_PER_IP" 1 32 || die "max-clients-per-ip must be 1-32"
    [[ -z "$MAX_PENDING_PER_IP" ]] || is_uint_in_range "$MAX_PENDING_PER_IP" 1 8 || die "max-pending-per-ip must be 1-8"
    [[ -z "$MAX_SEND_BUFFER_MB" ]] || is_uint_in_range "$MAX_SEND_BUFFER_MB" 1 64 || die "max-send-buffer-mb must be 1-64"
    validate_password_value "$PASSWORD_VALUE" ||
        die "server password must be 8-256 bytes and not all whitespace"
}

resolve_configuration() {
    load_existing_configuration
    resolve_password
    apply_option_overrides
    open_tty_if_available
    prompt_configuration
    if [[ -z "$PASSWORD_VALUE" ]]; then
        generate_password
    fi
    validate_configuration
}

build_extra_args() {
    local -a args=()
    [[ -z "$MAX_CLIENTS" ]] || args+=(--max-clients "$MAX_CLIENTS")
    [[ -z "$MAX_PENDING" ]] || args+=(--max-pending "$MAX_PENDING")
    [[ -z "$MAX_ROOMS" ]] || args+=(--max-rooms "$MAX_ROOMS")
    [[ -z "$MAX_CLIENTS_PER_IP" ]] || args+=(--max-clients-per-ip "$MAX_CLIENTS_PER_IP")
    [[ -z "$MAX_PENDING_PER_IP" ]] || args+=(--max-pending-per-ip "$MAX_PENDING_PER_IP")
    [[ -z "$MAX_SEND_BUFFER_MB" ]] || args+=(--max-send-buffer-mb "$MAX_SEND_BUFFER_MB")
    printf '%s' "${args[*]}"
}

installation_is_healthy() {
    [[ -n "$CURRENT_VERSION" && "$CURRENT_VERSION" == "$TARGET_VERSION" ]] || return 1
    [[ -x "$BIN_PATH" && -f "$ENV_PATH" && -f "$AUTH_PATH" && -f "$SERVICE_UNIT" ]] || return 1
    [[ "$(readlink -f -- "$BIN_PATH")" == "$RELEASES_DIR/$TARGET_VERSION/vlan-server" ]] || return 1
    systemctl is-active --quiet "$SERVICE_NAME" || return 1
    systemctl is-enabled --quiet "$SERVICE_NAME" || return 1
}

create_work_dir() {
    WORK_DIR="$(mktemp -d /tmp/vlan-server-install.XXXXXX)"
    SOURCE_DIR="$WORK_DIR/source"
    ROLLBACK_DIR="$WORK_DIR/rollback"
}

build_release() {
    log "Cloning official source tag $TARGET_VERSION"
    git clone --quiet --depth 1 --branch "$TARGET_VERSION" --single-branch \
        "$REPO_URL" "$SOURCE_DIR"
    [[ -f "$SOURCE_DIR/server/Makefile" &&
       -f "$SOURCE_DIR/server/vlan-server.service" &&
       -f "$SOURCE_DIR/server/DEPLOY.md" ]] ||
        die "tag $TARGET_VERSION does not contain the required server deployment files"
    log "Building VLan server from source"
    make -C "$SOURCE_DIR/server"
    [[ -x "$SOURCE_DIR/server/vlan-server" ]] || die "server build did not produce an executable"
}

execute_release_update() {
    create_work_dir || return
    build_release || return
    deploy_release
}

snapshot_path() {
    local name="$1"
    local path="$2"
    if [[ -e "$path" || -L "$path" ]]; then
        cp -a -- "$path" "$ROLLBACK_DIR/$name"
        : > "$ROLLBACK_DIR/$name.present"
    fi
}

remove_exact_path() {
    local path="$1"
    if [[ -d "$path" && ! -L "$path" ]]; then
        rm -rf -- "$path"
    else
        rm -f -- "$path"
    fi
}

restore_path() {
    local name="$1"
    local path="$2"
    remove_exact_path "$path"
    if [[ -f "$ROLLBACK_DIR/$name.present" ]]; then
        mkdir -p -- "$(dirname "$path")"
        cp -a -- "$ROLLBACK_DIR/$name" "$path"
    fi
}

snapshot_deployment() {
    install -d -m 0700 "$ROLLBACK_DIR"
    snapshot_path current "$CURRENT_LINK"
    snapshot_path binary "$BIN_PATH"
    snapshot_path env "$ENV_PATH"
    snapshot_path auth "$AUTH_PATH"
    snapshot_path unit "$SERVICE_UNIT"
    snapshot_path doc "$DOC_PATH"
    snapshot_path version "$VERSION_FILE"
    snapshot_path release "$RELEASES_DIR/$TARGET_VERSION"
    if systemctl is-enabled --quiet "$SERVICE_NAME"; then
        PREVIOUS_SERVICE_ENABLED=1
    fi
    if systemctl is-active --quiet "$SERVICE_NAME"; then
        PREVIOUS_SERVICE_ACTIVE=1
    fi
}

write_environment_file() {
    local destination="$1"
    local extra_args
    extra_args="$(build_extra_args)"
    {
        printf '# Managed by the VLan server installer.\n'
        printf 'VLAN_SERVER_PORT=%s\n' "$PORT"
        printf 'VLAN_SERVER_LOG=/var/log/vlan-server/server.log\n'
        printf 'VLAN_SERVER_LOG_MAX_MB=%s\n' "$LOG_MAX_MB"
        printf 'VLAN_SERVER_AUTH_FILE=%s\n' "$AUTH_PATH"
        printf 'VLAN_SERVER_EXTRA_ARGS=%s\n' "$extra_args"
    } > "$destination"
    chmod 0600 "$destination"
}

atomic_symlink() {
    local target="$1"
    local link_path="$2"
    local temporary="${link_path}.new.$$"
    rm -f -- "$temporary"
    ln -s "$target" "$temporary"
    mv -Tf -- "$temporary" "$link_path"
}

wait_for_service() {
    local attempt
    for attempt in {1..10}; do
        if systemctl is-active --quiet "$SERVICE_NAME"; then
            return 0
        fi
        sleep 1
    done
    systemctl status "$SERVICE_NAME" --no-pager >&2 || true
    return 1
}

restore_deployment() {
    (( DEPLOYMENT_ACTIVE == 1 )) || return 0
    DEPLOYMENT_ACTIVE=0
    warn "deployment failed; restoring the previous installation"
    set +e
    systemctl stop "$SERVICE_NAME" >/dev/null 2>&1
    restore_path release "$RELEASES_DIR/$TARGET_VERSION"
    restore_path current "$CURRENT_LINK"
    restore_path binary "$BIN_PATH"
    restore_path env "$ENV_PATH"
    restore_path auth "$AUTH_PATH"
    restore_path unit "$SERVICE_UNIT"
    restore_path doc "$DOC_PATH"
    restore_path version "$VERSION_FILE"
    systemctl daemon-reload
    if (( PREVIOUS_SERVICE_ENABLED == 1 )); then
        systemctl enable "$SERVICE_NAME" >/dev/null 2>&1
    else
        systemctl disable "$SERVICE_NAME" >/dev/null 2>&1
    fi
    if (( PREVIOUS_SERVICE_ACTIVE == 1 )); then
        systemctl restart "$SERVICE_NAME"
    else
        systemctl stop "$SERVICE_NAME" >/dev/null 2>&1
    fi
    set -e
}

preserve_legacy_binary() {
    if [[ -f "$BIN_PATH" && ! -L "$BIN_PATH" ]]; then
        local legacy_dir="$RELEASES_DIR/legacy-$(date +%Y%m%d_%H%M%S)"
        install -d -o root -g root -m 0755 "$legacy_dir"
        install -o root -g root -m 0755 "$BIN_PATH" "$legacy_dir/vlan-server"
    fi
}

deploy_release() {
    local release_dir="$RELEASES_DIR/$TARGET_VERSION"
    local env_temp="/usr/local/bin/.vlan-server.env.$$"
    local auth_temp="/usr/local/bin/.auth.password.$$"
    local unit_temp="/etc/systemd/system/.vlan-server.service.$$"
    local doc_temp="/usr/local/share/doc/vlan-server/.DEPLOY.md.$$"
    local version_temp="${STATE_DIR}/.installed-version.$$"
    local deploy_status

    snapshot_deployment
    DEPLOYMENT_ACTIVE=1
    set +e
    (
        set -Eeuo pipefail
        if [[ -f "$SERVICE_UNIT" ]]; then
            systemctl stop "$SERVICE_NAME"
        fi

        install -d -o root -g root -m 0755 "$RELEASES_DIR"
        install -d -o root -g root -m 0750 "$STATE_DIR" "$LOG_DIR"
        install -d -o root -g root -m 0755 /usr/local/share/doc/vlan-server
        preserve_legacy_binary

        remove_exact_path "$release_dir"
        install -d -o root -g root -m 0755 "$release_dir"
        install -o root -g root -m 0755 \
            "$SOURCE_DIR/server/vlan-server" "$release_dir/vlan-server"

        write_environment_file "$env_temp"
        printf '%s\n' "$PASSWORD_VALUE" > "$auth_temp"
        chmod 0600 "$auth_temp"
        install -o root -g root -m 0644 \
            "$SOURCE_DIR/server/vlan-server.service" "$unit_temp"
        install -o root -g root -m 0644 \
            "$SOURCE_DIR/server/DEPLOY.md" "$doc_temp"

        atomic_symlink "$release_dir" "$CURRENT_LINK"
        atomic_symlink "$CURRENT_LINK/vlan-server" "$BIN_PATH"
        mv -f -- "$env_temp" "$ENV_PATH"
        mv -f -- "$auth_temp" "$AUTH_PATH"
        mv -f -- "$unit_temp" "$SERVICE_UNIT"
        mv -f -- "$doc_temp" "$DOC_PATH"
        chmod 0600 "$ENV_PATH" "$AUTH_PATH"
        chmod 0644 "$SERVICE_UNIT" "$DOC_PATH"

        if command -v restorecon >/dev/null 2>&1; then
            restorecon -RF "$INSTALL_ROOT" "$BIN_PATH" "$SERVICE_UNIT" \
                "$ENV_PATH" "$AUTH_PATH" >/dev/null 2>&1 || true
        fi

        systemctl daemon-reload
        systemctl enable "$SERVICE_NAME"
        systemctl restart "$SERVICE_NAME"
        wait_for_service

        printf '%s\n' "$TARGET_VERSION" > "$version_temp"
        chmod 0600 "$version_temp"
        mv -f -- "$version_temp" "$VERSION_FILE"
    )
    deploy_status=$?
    set -e

    rm -f -- "$env_temp" "$auth_temp" "$unit_temp" "$doc_temp" "$version_temp"
    if (( deploy_status != 0 )); then
        restore_deployment
        return "$deploy_status"
    fi
    DEPLOYMENT_ACTIVE=0
}

show_summary() {
    log "VLan server $TARGET_VERSION is installed and running"
    printf '\nConfiguration:\n'
    printf '  Version:       %s\n' "$TARGET_VERSION"
    printf '  TCP/UDP port:  %s\n' "$PORT"
    printf '  Binary:        %s\n' "$BIN_PATH"
    printf '  Environment:   %s\n' "$ENV_PATH"
    printf '  Password file: %s\n' "$AUTH_PATH"
    printf '  Log max MiB:   %s\n' "$LOG_MAX_MB"
    printf '  Max clients:   %s\n' "${MAX_CLIENTS:-256 (server default)}"
    printf '  Max pending:   %s\n' "${MAX_PENDING:-64 (server default)}"
    printf '  Max rooms:     %s\n' "${MAX_ROOMS:-128 (server default)}"
    printf '  Clients/IP:    %s\n' "${MAX_CLIENTS_PER_IP:-32 (server default)}"
    printf '  Pending/IP:    %s\n' "${MAX_PENDING_PER_IP:-8 (server default)}"
    printf '  Send buffer:   %s MiB\n' "${MAX_SEND_BUFFER_MB:-64 (server default)}"
    if [[ -n "$GENERATED_PASSWORD" ]]; then
        printf '\nGenerated server authentication password (shown once):\n'
        printf '  %s\n' "$GENERATED_PASSWORD"
    fi
    printf '\nFirewall was not modified. Allow both of these in the host firewall\n'
    printf 'and in the cloud security group:\n'
    printf '  TCP %s\n  UDP %s\n' "$PORT" "$PORT"
    printf '\nUseful commands:\n'
    printf '  systemctl status %s\n' "$SERVICE_NAME"
    printf '  journalctl -u %s -n 100 --no-pager\n' "$SERVICE_NAME"
}

cleanup() {
    local exit_code=$?
    if (( DEPLOYMENT_ACTIVE == 1 )); then
        restore_deployment || true
    fi
    PASSWORD_VALUE=""
    PASSWORD_DIRECT_OPTION=""
    GENERATED_PASSWORD=""
    if [[ -n "$WORK_DIR" && -d "$WORK_DIR" ]]; then
        rm -rf -- "$WORK_DIR"
    fi
    if (( LOCK_OWNED == 1 )) && [[ -f "$LOCK_FILE" ]]; then
        local lock_pid=""
        IFS= read -r lock_pid < "$LOCK_FILE" || true
        if [[ "$lock_pid" == "$$" ]]; then
            rm -f -- "$LOCK_FILE"
        fi
    fi
    return "$exit_code"
}

main() {
    parse_args "$@"
    if (( SHOW_HELP == 1 )); then
        usage
        return 0
    fi

    require_root_and_systemd
    acquire_lock
    detect_installation
    install_dependencies
    read_current_version
    resolve_target_version
    resolve_configuration

    log "Selected release: $TARGET_VERSION"
    if (( CONFIG_OVERRIDE == 0 && RECONFIGURE == 0 )) && installation_is_healthy; then
        log "The latest selected version is already installed and healthy; no update is required"
        show_summary
        return 0
    fi

    execute_release_update
    show_summary
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    trap cleanup EXIT
    trap 'exit 130' INT TERM
    main "$@"
fi
