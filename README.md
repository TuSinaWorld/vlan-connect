# VLan

VLan is a virtual LAN relay tool built around a virtual network adapter, a relay server, and selectable TCP/UDP traffic policies.

## Current Design

- Relay-only networking: Raw UDP, KCP, or TCP Relay.
- Inner traffic split:
  - TCP traffic uses its own transport/FEC/KCP profile policy.
  - UDP and non-TCP traffic use their own transport/FEC/KCP profile policy.
- Default room policy:
  - TCP: Raw UDP, FEC off.
  - UDP/non-TCP: KCP, FEC off, realtime profile.
- Shared room settings: room name, max players, room password, MTU.
- MTU options: `1280`, `1400`, `1420`; transport overhead is deducted internally.
- Room password is only room access control. It is not used for transport encryption.
- Server auth password is optional. When enabled, signaling, TCP relay data, UDP relay frames, and TUN payloads are encrypted after auth.

## Components

- `server/`: Linux relay server, C++11, POSIX sockets, epoll.
- `client/`: Windows GUI client, Qt 5.9.9, MSVC 2015.
- `client-cli/`: command-line client.
- `common/`: shared protocol, buffers, crypto helpers.
- `3rdparty/`: bundled third-party code.

## Server

Build:

```bash
cd server
make
```

Run without server auth:

```bash
./vlan-server -p 11510
```

Run with server auth:

```bash
./vlan-server -p 11510 --auth-file /path/to/password.txt
```

Supported auth sources:

- `--auth-file PATH`
- `VLAN_SERVER_AUTH_PASSWORD`
- `--auth PASSWORD` for testing

Ports:

| Port | Protocol | Purpose |
|---|---|---|
| 11510 | TCP | Signaling and TCP relay data channel |
| 11510 | UDP | UDP relay traffic |

## GUI Client

Build with Qt 5.9.9 and MSVC 2015:

```cmd
qmake network.pro
nmake
```

Run as Administrator so the virtual adapter can be created.

Room creation is simple by default: room name, max players, and optional room password. Enable advanced settings to edit TCP/UDP transport policy, FEC, KCP profile, and MTU.

## CLI Client

See `client-cli/README.md`.

## Notes

- Updated clients and server are not wire-compatible with older versions.
- TCP Relay is kept as an extreme fallback.
- No public default server address is embedded in this repository.
