# VLan

VLan 是一个虚拟局域网联机工具。它由 Linux 中继服务端、Windows 图形客户端、命令行客户端和虚拟网卡组成，用于让不同网络中的玩家像在同一个局域网内一样通信。

## 快速入口

- 服务端部署和 systemd 安装: [server/DEPLOY.md](server/DEPLOY.md)
- CLI 客户端启动和交互命令: [client-cli/README.md](client-cli/README.md)
- GUI 客户端构建: 见本文的 [GUI 客户端](#gui-客户端)

Linux systemd 服务器可以从最新正式 tag 自动源码编译并安装:

```bash
curl -fsSL https://raw.githubusercontent.com/TuSinaWorld/vlan-connect/main/server/install.sh | sudo bash
```

同一条命令在已有安装上会执行更新并保留端口和密码。脚本支持 apt、dnf/yum，安装成功后会显示需要在防火墙和云安全组开放的 TCP/UDP 端口；完整参数和回滚行为见 [server/DEPLOY.md](server/DEPLOY.md)。

如果从 GitHub Release 安装服务端，不要只下载 `vlan-server-linux-*.tar.gz` 后直接运行。服务端安装还需要 `server/vlan-server.service`、`server/vlan-server.env.example`、`server/auth.password.example` 等文件，这些文件在 `vlan-connect-source-*.tar.gz` 源码包里。具体步骤见 [server/DEPLOY.md](server/DEPLOY.md)。

## 功能概览

- 中继网络模式: Raw UDP、KCP、TCP Relay。
- 流量分流:
  - TCP 流量可以单独设置传输模式、FEC 和 KCP profile。
  - UDP 及其它非 TCP 流量可以单独设置传输模式、FEC 和 KCP profile。
- 默认房间策略:
  - TCP: Raw UDP，关闭 FEC。
  - UDP/非 TCP: KCP，关闭 FEC，realtime profile。
- 房间设置: 房间名、最大人数、房间密码、MTU。
- MTU 选项: `1280`、`1400`、`1420`，程序内部会扣除传输开销。
- 房间密码只用于加入房间校验，不参与传输加密。
- 服务端鉴权是强制要求。信令、TCP Relay data channel、UDP relay frame 和 TUN payload 只在完成鉴权并建立安全会话后传输；GUI/CLI 会拒绝未要求鉴权的服务端。
- 服务端、GUI 和 CLI 仅支持协议 v8，必须作为同一版本批次发布。

## 目录结构

- `server/`: Linux 中继服务端，C++11，POSIX sockets，epoll。
- `client/`: Windows GUI 客户端，Qt 5.9.9，MSVC 2015。
- `client-cli/`: 命令行客户端。
- `common/`: 协议、字节缓冲区、加密辅助代码。
- `3rdparty/`: 随源码一起携带的第三方代码。

## 服务端

完整部署文档见 [server/DEPLOY.md](server/DEPLOY.md)。

一键安装或更新:

```bash
curl -fsSL https://raw.githubusercontent.com/TuSinaWorld/vlan-connect/main/server/install.sh | sudo bash
```

源码构建:

```bash
cd server
make
```

前台运行（`--auth-file` 必填）:

```bash
./vlan-server -p 11510 --auth-file /path/to/password.txt
```

密码文件只允许第一行密码及后续空行，密码必须为 8–256 字节，不能包含 NUL，也不能全部为空白。服务端不再接受 `--auth` 或服务端密码环境变量。

容量参数 `--max-clients`、`--max-pending`、`--max-rooms`、`--max-clients-per-ip`、`--max-pending-per-ip` 和 `--max-send-buffer-mb` 只能从内置硬上限向下调整；完整说明见部署文档。

服务端端口:

| 端口 | 协议 | 用途 |
|---|---|---|
| 11510 | TCP | 信令和 TCP Relay data channel |
| 11510 | UDP | UDP relay traffic |

## GUI 客户端

使用 Qt 5.9.9 和 MSVC 2015 构建:

```cmd
qmake network.pro
nmake
```

运行 GUI 客户端时需要管理员权限，因为程序需要创建和配置虚拟网卡。

GUI 客户端会在本机用户配置中保存语言、默认服务器地址、默认端口、默认昵称和详细日志开关。服务端鉴权密码和房间密码不会持久化保存。

## CLI 客户端

完整使用文档见 [client-cli/README.md](client-cli/README.md)。

启动示例:

```bash
vlan-cli -s 127.0.0.1 -p 11510 -n Player1 --auth-file ./auth.password
```

## 许可证与第三方组件

本项目使用 GPL-3.0-only 开源协议，完整文本见 [LICENSE](LICENSE)。项目源码中包含或引用 KCP、Monocypher、cm256cc、Wintun header 等第三方组件；Windows 二进制包还可能随带 Qt 5.9.9 运行库、官方 Wintun DLL 和 Microsoft Direct3D compiler runtime。各组件保留其原始许可证和声明，汇总信息见 [NOTICE](NOTICE)。
