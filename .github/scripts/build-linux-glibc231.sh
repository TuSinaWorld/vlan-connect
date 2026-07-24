#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <server|cli> <x86_64|arm64>" >&2
    exit 2
fi

if [ ! -r /etc/os-release ]; then
    echo "Cannot determine the Linux build environment." >&2
    exit 1
fi

# This script is intentionally restricted to the release compatibility image.
# Building on a newer distribution can silently raise the required glibc ABI.
. /etc/os-release
if [ "${ID:-}" != "ubuntu" ] || [ "${VERSION_ID:-}" != "20.04" ]; then
    echo "Compatibility builds must run in ubuntu:20.04, got ${ID:-unknown} ${VERSION_ID:-unknown}." >&2
    exit 1
fi

case "$1" in
    server)
        source_dir="server"
        binary="server/vlan-server"
        ;;
    cli)
        source_dir="client-cli/linux"
        binary="client-cli/linux/vlan-cli"
        ;;
    *)
        echo "Unknown Linux build target: $1" >&2
        exit 2
        ;;
esac

expected_arch="$2"
case "$expected_arch" in
    x86_64|amd64)
        expected_uname="x86_64"
        ;;
    aarch64|arm64)
        expected_uname="aarch64"
        ;;
    *)
        echo "Unknown Linux build architecture: $expected_arch" >&2
        exit 2
        ;;
esac

if [ "$(uname -m)" != "$expected_uname" ]; then
    echo "Build runner architecture mismatch: expected $expected_uname, got $(uname -m)." >&2
    exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    binutils \
    build-essential \
    ca-certificates

echo "Building $1 in ${PRETTY_NAME} for $expected_arch"
make -C "$source_dir"

bash .github/scripts/verify-linux-abi.sh "$binary" "$expected_arch"
