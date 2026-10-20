#!/usr/bin/env python3
"""Exercise actual server/CLI authentication on loopback without a TUN device."""
import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time


def wait_for(process, log, text, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        content = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
        if text in content:
            return content
        if process.poll() is not None:
            raise AssertionError(f"Process exited before {text!r}:\n{content[-12000:]}")
        time.sleep(0.05)
    raise AssertionError(f"Timeout waiting for {text!r}:\n{content[-12000:]}")


def stop(process):
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def read_exact(connection, size):
    data = b""
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise AssertionError("Unexpected EOF during version rejection")
        data += chunk
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    args = parser.parse_args()
    server_path, cli_path = args.server.resolve(), args.cli.resolve()
    for binary in (server_path, cli_path):
        version = subprocess.check_output([str(binary), "--version"], text=True, timeout=5)
        assert "0.3.1 protocol=9" in version, version
    with tempfile.TemporaryDirectory(prefix="vlan-auth-smoke-") as directory:
        work = Path(directory)
        auth = work / "auth.password"
        auth.write_text("SmokePass12\n")
        os.chmod(auth, 0o600)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        server_log = work / "server.log"
        with server_log.open("wb") as output:
            server = subprocess.Popen(
                [str(server_path), "-p", str(port), "--auth-file", str(auth)],
                stdout=output, stderr=subprocess.STDOUT)
            try:
                wait_for(server, server_log, "VLan Server started")
                for suffix, password, expected in (
                        ("good", "SmokePass12", "Logged in, peerId="),
                        ("wrong", "WrongPass12", "Automatic reconnect stopped:")):
                    log = work / f"cli-{suffix}.log"
                    with log.open("wb") as cli_output:
                        client = subprocess.Popen(
                            [str(cli_path), "-s", "127.0.0.1", "-p", str(port),
                             "-n", f"Smoke{suffix}", "--auth", password,
                             "--log-file", str(work / f"diagnostics-{suffix}.log")],
                            stdin=subprocess.PIPE, stdout=cli_output,
                            stderr=subprocess.STDOUT)
                        try:
                            wait_for(client, log, expected)
                            if suffix == "good":
                                client.stdin.write(b"list\n")
                                client.stdin.flush()
                                wait_for(client, log, "Room List (0 rooms)")
                            else:
                                time.sleep(1.2)
                                content = log.read_text(encoding="utf-8", errors="replace")
                                assert "Auto-reconnect" not in content, content
                                assert "Logged in, peerId=" not in content, content
                            client.stdin.write(b"quit\n")
                            client.stdin.flush()
                            assert client.wait(timeout=5) == 0, log.read_text(encoding="utf-8", errors="replace")
                        finally:
                            stop(client)
                # Old clients are rejected before password work or key agreement.
                with socket.create_connection(("127.0.0.1", port), timeout=3) as old:
                    # CLIENT_HELLO contains version + nonce + public key.
                    header = (Path(__file__).resolve().parents[2] / "common/protocol.h").read_text(encoding="utf-8")
                    import re
                    hello = int(re.search(r"MSG_CLIENT_HELLO\s*=\s*(0x[0-9A-Fa-f]+|\d+)", header)[1], 0)
                    error = int(re.search(r"MSG_ERROR\s*=\s*(0x[0-9A-Fa-f]+|\d+)", header)[1], 0)
                    payload = struct.pack("!H", 8) + bytes(range(48))
                    old.sendall(struct.pack("!BH", hello, len(payload)) + payload)
                    kind, length = struct.unpack("!BH", read_exact(old, 3))
                    body = read_exact(old, length)
                    assert kind == error and struct.unpack("!H", body[:2])[0] == 1, body
            finally:
                stop(server)
    print("Production server/CLI v9 auth, room list, wrong-password retry stop and v8 rejection passed; no TUN was configured.")


if __name__ == "__main__":
    main()
