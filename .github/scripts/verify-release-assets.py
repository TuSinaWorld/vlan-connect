#!/usr/bin/env python3
"""Require a complete, single-commit release before publishing its draft."""
import argparse
import hashlib
from pathlib import Path
import re
import tarfile
import zipfile


def metadata(path):
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            names = [n for n in archive.namelist() if n.endswith("SOURCE.txt")]
            if len(names) != 1:
                raise ValueError(f"{path.name}: expected one SOURCE.txt")
            return archive.read(names[0]).decode("utf-8-sig")
    with tarfile.open(path, "r:gz") as archive:
        names = [n for n in archive.getmembers() if n.name.endswith("SOURCE.txt")]
        if len(names) != 1:
            raise ValueError(f"{path.name}: expected one SOURCE.txt")
        return archive.extractfile(names[0]).read().decode("utf-8-sig")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    parser.add_argument("commit")
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"v\d+\.\d+\.\d+", args.tag):
        parser.error("expected stable vX.Y.Z tag")
    if not re.fullmatch(r"[0-9a-f]{40}", args.commit):
        parser.error("expected full Git commit")
    packages = [
        "vlan-server-linux-x86_64.tar.gz", "vlan-server-linux-arm64.tar.gz",
        "vlan-cli-linux-x86_64.tar.gz", "vlan-cli-linux-arm64.tar.gz",
        "vlan-cli-windows-x86_64.zip", f"vlan-gui-windows-x86_64-{args.tag}.zip",
    ]
    source = f"vlan-connect-source-{args.tag}.tar.gz"
    assets = [source, "LICENSE.txt", "NOTICE.txt"] + packages
    for name in assets:
        path = args.directory / name
        if not path.is_file() or not path.stat().st_size:
            raise ValueError(f"Required release asset missing: {name}; release must stay draft")
    for name in packages:
        info = metadata(args.directory / name)
        for expected in (f"Release tag: {args.tag}", f"Build commit: {args.commit}", "Protocol version: 9"):
            if expected not in info.splitlines():
                raise ValueError(f"{name}: missing or mismatched {expected}")
    with tarfile.open(args.directory / source, "r:gz") as archive:
        prefix = f"vlan-connect-{args.tag}/"
        commit = archive.extractfile(prefix + "SOURCE-COMMIT.txt").read().decode().strip()
        version = archive.extractfile(prefix + "common/version.h").read().decode()
        if commit != args.commit or f'#define VLAN_RELEASE_VERSION "{args.tag[1:]}"' not in version:
            raise ValueError("Source archive does not match release tag/commit")
    checksum = args.directory / "SHA256SUMS.txt"
    checksum.write_text("".join(
        f"{hashlib.sha256((args.directory / name).read_bytes()).hexdigest()}  {name}\n"
        for name in sorted(assets)), encoding="utf-8")
    print(f"Verified {len(assets)} assets for {args.tag} ({args.commit}); {checksum}")


if __name__ == "__main__":
    main()
