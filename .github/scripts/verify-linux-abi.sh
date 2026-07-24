#!/usr/bin/env bash
set -euo pipefail

readonly MAX_GLIBC_VERSION="2.31"
readonly MAX_GLIBCXX_VERSION="3.4.28"
readonly MAX_CXXABI_VERSION="1.3.12"

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <binary> <x86_64|aarch64>" >&2
    exit 2
fi

binary="$1"
expected_arch="$2"

if [ ! -f "$binary" ]; then
    echo "Linux ABI check failed: binary not found: $binary" >&2
    exit 1
fi

export LC_ALL=C

machine="$(
    readelf --file-header "$binary" |
        awk -F: '/^[[:space:]]*Machine:/ {
            sub(/^[[:space:]]+/, "", $2)
            print $2
            exit
        }'
)"

case "$expected_arch" in
    x86_64|amd64)
        expected_machine="Advanced Micro Devices X86-64"
        ;;
    aarch64|arm64)
        expected_machine="AArch64"
        ;;
    *)
        echo "Linux ABI check failed: unsupported expected architecture: $expected_arch" >&2
        exit 2
        ;;
esac

if [ "$machine" != "$expected_machine" ]; then
    echo "Linux ABI check failed: expected $expected_machine, got ${machine:-unknown}." >&2
    exit 1
fi

version_info="$(readelf --version-info "$binary")"

max_required_version() {
    prefix="$1"
    printf '%s\n' "$version_info" |
        grep -oE "${prefix}_[0-9]+(\.[0-9]+)*" |
        sed "s/^${prefix}_//" |
        sort -Vu |
        tail -n 1 ||
        true
}

check_max_version() {
    label="$1"
    required="$2"
    allowed="$3"
    must_exist="$4"

    if [ -z "$required" ]; then
        if [ "$must_exist" = "true" ]; then
            echo "Linux ABI check failed: $label requirement was not found." >&2
            exit 1
        fi
        echo "$label: not required by this binary"
        return
    fi

    if [ "$(printf '%s\n%s\n' "$allowed" "$required" | sort -V | tail -n 1)" != "$allowed" ]; then
        echo "Linux ABI check failed: $label required $required, maximum allowed is $allowed." >&2
        exit 1
    fi

    echo "$label: required $required, maximum allowed $allowed"
}

required_glibc="$(max_required_version GLIBC)"
required_glibcxx="$(max_required_version GLIBCXX)"
required_cxxabi="$(max_required_version CXXABI)"

check_max_version GLIBC "$required_glibc" "$MAX_GLIBC_VERSION" true
check_max_version GLIBCXX "$required_glibcxx" "$MAX_GLIBCXX_VERSION" false
check_max_version CXXABI "$required_cxxabi" "$MAX_CXXABI_VERSION" false

echo "ELF architecture: $machine"
echo "Dynamic dependencies:"
readelf --dynamic "$binary" |
    awk '/\(NEEDED\)/ {
        print "  " $NF
    }'

echo "Linux ABI compatibility check passed for $binary."
