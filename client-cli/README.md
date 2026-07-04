# VLan CLI 客户端

`vlan-cli` 是 VLan 的命令行客户端，适合在终端环境中连接服务端、创建房间、加入房间和查看连接状态。

## 相关文档

- 服务端部署: [../server/DEPLOY.md](../server/DEPLOY.md)
- 项目入口和 GUI 客户端说明: [../README.md](../README.md)

## 默认值

- 服务端端口: `11510`
- 房间 MTU: `1400`
- TCP 流量策略: Raw UDP，关闭 FEC
- UDP 及非 TCP 流量策略: KCP，关闭 FEC，realtime profile
- 房间密码: 可选，只用于加入房间校验
- 服务端鉴权密码: 只缓存在当前进程内，程序退出后失效

## 启动

```bash
vlan-cli -s 127.0.0.1 -p 11510 -n Player1
```

启动参数:

| 参数 | 说明 | 示例 |
|---|---|---|
| `-s HOST` | 服务端地址，默认 `127.0.0.1`。 | `-s example.com` |
| `-p PORT` | 服务端端口，默认 `11510`。 | `-p 11510` |
| `-n NAME` | 玩家昵称。 | `-n Player1` |
| `--auth PASSWORD` | 为当前进程设置服务端鉴权密码。 | `--auth my-server-password` |
| `--auth-file PATH` | 从文件第一行读取服务端鉴权密码。 | `--auth-file ./auth.password` |
| `-v` | 输出详细日志。 | `-v` |

如果服务端启用了鉴权，但启动时没有传入密码，CLI 会提示使用 `server-password <password>` 输入密码。

## 交互命令

进入 CLI 后可以输入以下命令。

| 命令 | 说明 | 示例 |
|---|---|---|
| `connect` | 连接当前服务端地址。地址来自启动参数或 `server` 命令。 | `connect` |
| `server <host[:port]> [serverPassword]` | 设置服务端地址，可同时设置端口和当前进程内的服务端鉴权密码。未提供密码时会清除旧的进程内密码。 | `server example.com:11510 my-server-password` |
| `server <host> <port> [serverPassword]` | 用分开的 host 和 port 设置服务端地址。 | `server example.com 11510` |
| `server-password <password>` | 设置当前进程内的服务端鉴权密码，用于连接或重连。不会写入磁盘。 | `server-password my-server-password` |
| `list` | 获取服务端房间列表。 | `list` |
| `create <name> [maxPlayers] [roomPassword] [mtu] [opts]` | 创建房间。未填写的参数使用默认值，`opts` 用于覆盖 TCP/UDP 传输策略。 | `create TestRoom 8 roompass 1400 tcp=raw udp=kcp udp-profile=realtime udp-fec=30` |
| `join <roomId> [roomPassword]` | 加入指定房间。如果房间有密码，需要提供房间密码。 | `join 1001 roompass` |
| `leave` | 离开当前房间。 | `leave` |
| `status` | 查看当前服务端、连接状态、房间状态和传输策略。 | `status` |
| `peers` | 查看当前房间中的成员和连接信息。 | `peers` |
| `quit` | 退出 CLI。 | `quit` |

## create 命令参数

`create` 的位置参数依次为:

```text
create <name> [maxPlayers] [roomPassword] [mtu] [opts]
```

- `<name>`: 房间名，必填。
- `[maxPlayers]`: 最大人数，未填写时使用默认值。
- `[roomPassword]`: 房间密码，未填写时房间不设密码。
- `[mtu]`: 房间 MTU，只支持 `1280`、`1400`、`1420`。
- `[opts]`: 传输策略选项，使用 `key=value` 格式，可以写多个。

`opts` 支持:

| 选项 | 可选值 | 说明 |
|---|---|---|
| `tcp` | `raw`、`kcp`、`tcp` | TCP 流量使用的传输模式。 |
| `udp` | `raw`、`kcp`、`tcp` | UDP 及非 TCP 流量使用的传输模式。 |
| `tcp-fec` | `none`、`10`、`30`、`50`、`70`、`100`、`200` | TCP 流量传输的 FEC 冗余比例。 |
| `udp-fec` | `none`、`10`、`30`、`50`、`70`、`100`、`200` | UDP 及非 TCP 流量传输的 FEC 冗余比例。 |
| `tcp-profile` | `realtime`、`bulk` | TCP 流量在 KCP 模式下的 profile。 |
| `udp-profile` | `realtime`、`bulk` | UDP 及非 TCP 流量在 KCP 模式下的 profile。 |

示例:

```text
create MyRoom
create MyRoom 8
create MyRoom 8 roompass
create MyRoom 8 roompass 1400
create MyRoom 8 roompass 1400 tcp=raw udp=kcp udp-profile=realtime udp-fec=30
create BulkRoom 4 1420 tcp=tcp udp=kcp udp-profile=bulk
```

如果不确定传输策略，直接使用默认创建命令即可:

```text
create MyRoom
```
