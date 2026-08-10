# VLan CLI 客户端

`vlan-cli` 是 VLan 的命令行客户端，可连接服务端、创建/加入房间并查看连接状态。当前版本仅支持协议 v8，并且只连接强制鉴权的服务端；收到 `authRequired=0` 时会拒绝连接。

相关文档：

- [服务端部署](../server/DEPLOY.md)
- [项目与 GUI 说明](../README.md)

## 启动

```bash
vlan-cli -s 127.0.0.1 -p 11510 -n Player1 --auth-file ./auth.password
```

| 参数 | 说明 | 示例 |
|---|---|---|
| `-s HOST` | 服务端地址，默认 `127.0.0.1` | `-s example.com` |
| `-p PORT` | 服务端端口，默认 `11510` | `-p 11510` |
| `-n NAME` | 玩家昵称；省略时交互输入 | `-n Player1` |
| `--auth PASSWORD` | 当前进程使用的服务端鉴权密码 | `--auth my-server-password` |
| `--auth-file PATH` | 从文件第一行读取客户端提交的服务端鉴权密码 | `--auth-file ./auth.password` |
| `-v` | 输出详细日志 | `-v` |

客户端仍允许通过参数或交互命令输入密码，但服务端本身只能用 `--auth-file` 配置。客户端密码只保存在当前进程内，不写入磁盘。未在启动时提供密码时，CLI 会提示使用 `server-password <password>` 后继续鉴权。

## 默认房间参数

- MTU：`1400`
- TCP：Raw UDP，FEC 关闭
- UDP/其它流量：KCP，FEC 关闭，`realtime` profile
- 房间密码：可选，仅用于加入房间校验

## 交互命令

| 命令 | 说明 |
|---|---|
| `connect` | 连接当前服务端 |
| `server <host[:port]> [serverPassword]` | 设置服务端，可同时设置当前进程密码 |
| `server <host> <port> [serverPassword]` | 分开指定地址和端口 |
| `server-password <password>` | 设置当前进程鉴权密码并继续握手 |
| `list` | 获取完整 v8 分页房间快照 |
| `create <name> [maxPlayers] [roomPassword] [mtu] [opts]` | 创建房间 |
| `join <roomId> [roomPassword]` | 加入房间 |
| `leave` | 离开房间并回到大厅 |
| `status` | 查看服务端、房间和传输状态 |
| `peers` | 查看房间成员与连接信息 |
| `quit` | 优雅退出 CLI |

`create` 的 `opts` 使用 `key=value`，支持：

- `tcp=raw|kcp|tcp`、`udp=raw|kcp|tcp`
- `tcp-fec=none|10|30|50|70|100|200`
- `udp-fec=none|10|30|50|70|100|200`
- `tcp-profile=realtime|bulk`、`udp-profile=realtime|bulk`

MTU 只支持 `1280`、`1400`、`1420`。示例：

```text
create MyRoom
create MyRoom 8 roompass 1400 tcp=raw udp=kcp udp-profile=realtime udp-fec=30
join 1001 roompass
```

TUN 初始化或运行期发生终止性错误时，CLI 会自动发送离房请求、清理租约和传输状态并回到大厅，同时保持服务端登录。临时 TUN 发送环满只计为丢包，不触发退房。
