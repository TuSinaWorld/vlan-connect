# VLan CLI Client

Command-line client for VLan relay networking.

## Defaults

- Server port: `11510`
- Room MTU: `1400`
- TCP traffic policy: `Raw UDP`, FEC off
- UDP and non-TCP traffic policy: `KCP`, FEC off, realtime profile
- Room password: optional access control only
- Server auth password: cached in memory while the process is running

## Start

```bash
vlan-cli -s 127.0.0.1 -p 11510 -n Player1
```

Options:

- `-s HOST`: server address, default `127.0.0.1`
- `-p PORT`: server port, default `11510`
- `-n NAME`: player name
- `--auth PASSWORD`: cache a server auth password for this process
- `--auth-file PATH`: read the server auth password from the first line of a file
- `-v`: verbose logs

## Interactive Commands

```text
connect
server <host[:port]> [serverPassword]
server <host> <port> [serverPassword]
server-password <password>
list
create <name> [maxPlayers] [roomPassword] [mtu] [opts]
join <roomId> [roomPassword]
leave
status
peers
quit
```

Notes:

- `server` changes the current endpoint and clears the old cached server password unless a new one is supplied.
- `server-password` updates the cached server auth password for reconnects in this process.
- `create` uses default TCP/UDP policies when policy options are omitted.
- Policy options: `tcp=raw|kcp|tcp`, `udp=raw|kcp|tcp`, `tcp-fec=none|10|30|50|70|100|200`, `udp-fec=none|10|30|50|70|100|200`, `tcp-profile=realtime|bulk`, `udp-profile=realtime|bulk`.
- Valid MTU values are `1280`, `1400`, and `1420`.
