# PCL integration branch (Windows CLI machine interface v1)

This branch adds a Windows launcher control interface to the existing v0.3.4 / v9 CLI. It does not change server sources or the network protocol. Existing published v0.3.4 assets do not contain this interface; use a build from this branch. `vlan-cli --version` includes `machine=1`.

## Starting and controlling the helper

The launcher creates a byte-mode named-pipe server named `PCL-VLAN-IPC-` followed by 32 hexadecimal characters, then starts:

```text
vlan-cli.exe --machine-pipe PCL-VLAN-IPC-<32 hex> --parent-pid <launcher PID>
```

Actual Windows virtual networking requires elevation. PCL starts the helper through `runas`; the launcher and game stay unelevated. Status/authentication-only tests can use a normal process. CLI and official x64 Wintun must be in the same directory.

Restrict pipe ACLs to the launcher user, administrators and SYSTEM. The CLI checks the actual pipe server PID against the requested parent, and PCL checks the client PID against the launched process. No credentials are command-line arguments. The interface cannot execute commands or scripts.

Requests are one UTF-8 JSON object per line, at most 16 KiB. `v` is integer 1 (`"1"` is also accepted), `requestId` is a nonempty string of at most 64 bytes, and `op` is a string. Other request fields are strings. Duplicate keys, nested values and trailing data are rejected.

| op | Additional fields |
|---|---|
| configure | host, port, password (server authentication), name (valid unique network nickname) |
| create | name (ASCII letters/digits), roomPassword (empty or omitted means unprotected), compatibility (`true` / `false`; TCP Relay versus existing defaults) |
| join | roomId, optional roomPassword |
| leave | none |
| status / peers | none |
| shutdown | none |

Events have integer `v=1`, an `event` name and, for responses, `requestId`. `accepted` only acknowledges command submission, not authentication or room readiness. `status` is pushed once per second, with authenticated, ready, roomId, peerId, virtualIp, lifecycle phase, numeric TCP/UDP transport modes and peers (ID, name, IP, RTT, sampling, timeoutPercent, outage). `peer_joined` / `peer_left` identify changed peers. `error` contains a stable code, never a request payload or password.

Important errors: `ROOM_NOT_FOUND`, `ROOM_FULL`, `ROOM_PASSWORD_REQUIRED`, `ROOM_PASSWORD_INVALID`, `ROOM_EXPIRED`, `AUTHENTICATION_FAILED`, `SERVER_UNREACHABLE`, `ADAPTER_FAILED`, `RECONNECT_EXHAUSTED`, `BUSY`, `NOT_AUTHENTICATED`, `INVALID_REQUEST`, `UNKNOWN_OPERATION`. Legacy v9 general room errors are normalized at the CLI boundary from the existing service responses. Room creation/join has the existing 15 s request / 30 s preparation deadlines. Readiness is only true after the virtual adapter is ready.

No-password rooms still require server authentication. Machine mode never silently creates a substitute room after resume failure. The launcher must verify its game-host session separately because room IDs can be reused and v9 does not carry application metadata.

Pipe closure, parent exit and shutdown trigger graceful cleanup using the existing lifecycle worker. Writes have a 3 s deadline. The helper does not kill an outstanding driver thread; PCL prevents starting a second instance until the previous one exits. Human stdout is suppressed in Windows machine mode; normal diagnostic logs remain separate and contain no credentials.

## Validation

Build with the existing Windows `client-cli/windows/build.bat`. CMake tests now include `machine_codec_tests`; existing CLI lifecycle, authentication, protocol, transport, Wintun-worker and GUI tests remain enabled. PCL additionally exercises real machine-pipe shutdown, local TCP proxying, host-session challenges and credential/invitation handling without requiring a virtual NIC.

Real UAC, actual Wintun configuration and two-device Minecraft operation must be recorded separately from these isolated checks. No production server or real authentication password is required for the automated suites.
