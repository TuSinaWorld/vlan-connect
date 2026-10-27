# VLan CLI 客户端

`vlan-cli` 是 VLan 的命令行客户端，可连接服务端、创建/加入房间并查看连接状态。v0.3.4 使用协议 v9，可连接现有 v9 服务端；客户端等待/取消修复无需强制升级服务端，新版服务端另有关键响应发送失败修复。v8 与 v9 不能混连，从 v8 升级仍需同步更新服务端及所有客户端。客户端只连接强制鉴权的服务端；收到 `authRequired=0` 时会拒绝连接。

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
| `--log-file PATH` | 轮转文件日志，继续保留终端输出；文件父目录须已存在且可写 | `--log-file ./vlan-cli.log` |
| `--version` | 打印软件版本、协议版本和构建提交后退出；不读取密码文件或连接网络 | `--version` |
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
| `list` | 获取完整 v9 分页房间快照 |
| `create <name> [maxPlayers] [roomPassword] [mtu] [opts]` | 创建房间 |
| `join <roomId> [roomPassword]` | 加入房间 |
| `leave` | 离开房间并回到大厅 |
| `status` | 查看服务端、房间和传输状态，汇总异常成员路径 |
| `peers` | 查看成员各路径 RTT、60 秒探测超时率及样本明细 |
| `quit` | 优雅退出 CLI |

链路质量按成员的两条实际传输路径每秒探测，统计最近 60 秒；10 秒内没有回应计为超时，等待中的探测不进入分母，少于 5 个已判定样本显示采样中。本地发送失败单独记录。只有自己时无法评估，不会告警。Raw UDP/KCP 持续无及时回应时提示 UDP 路径可能受限；TCP Relay 使用通用链路提示。KCP/FEC/TCP 补回的丢包不计入该指标，因此它不是底层 UDP 原始丢包率。

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

已入房后的 TUN 运行期发生终止性错误时，CLI 会发送离房请求、清理租约和传输状态并回到大厅。准备阶段失败按下述注销与恢复流程处理。临时 TUN 发送环满只计为丢包，不触发退房。

v0.3.4 将网卡准备和清理放到生命周期工作线程。请求等待最多 15 秒，本机初始化最多 30 秒（包含最多 10 秒就绪检查），心跳和房间列表不会延长期限；`status` 显示当前阶段，`leave` 可取消等待。超时、取消或无法确认服务端是否已提交时，注销旧会话、等待确认最多 1 秒后关闭，并仅尝试一次登录大厅，不自动重复建房。用户主动断开会取消恢复。后台清理完成前禁止创建第二块网卡；不可取消的驱动调用尚未返回时明确显示清理未完成，不能安全地强杀线程。旧服务端未收到注销时，原房间租约可能保留最多 120 秒。

Windows 默认将普通诊断写入当前用户的 `%LOCALAPPDATA%\VLan\logs\cli.log`，实际位置以启动日志为准，也可用 `--log-file` 覆盖。文件日志轮转后缀为 `.1`、`.2`。Linux 默认保留终端日志，可用 `--log-file` 显式启用文件日志。防火墙预清理、添加、重试和删除操作记录结果、错误码及耗时；密码、密钥、证明和数据包正文不写入普通日志。

Windows 命令诊断统一为 UTF-8，优先保留有效 UTF-8，识别带 BOM 的 UTF-16，必要时按记录的 OEM/ANSI 码页回退。日志包含 `encoding`、`codepage`、`captured_bytes`、`observed_bytes` 和 `truncated`；超限时不会切断 UTF-8 字符。命令成功与否仍由退出码、Win32 错误和超时决定，不依赖本地化文本。

Windows CLI 源码构建须在 x64 MSVC 2015 或更新的 Developer Command Prompt 中执行：

```cmd
client-cli\windows\build.bat D:\build\vlan-cli
```

省略输出目录时使用 `client-cli\windows`。脚本遇到任一编译或链接错误即退出失败；发行打包只能使用本次成功构建的输出。运行时保留同目录的官方 x64 `wintun.dll`，并以管理员权限运行实际联网操作；`--version` 不需要提升权限。
