# VLan CLI Client

纯 C++11 实现的 VLan 命令行客户端，适用于 Linux 和 Windows，可在无图形界面的服务器上运行。

---

## 目录

- [编译](#编译)
  - [Linux (g++)](#linux-g)
  - [Windows (MinGW)](#windows-mingw)
  - [Windows (MSVC)](#windows-msvc)
- [使用方法](#使用方法)
  - [启动参数](#启动参数)
  - [交互命令](#交互命令)
  - [典型流程](#典型流程)
- [功能说明](#功能说明)
  - [自动重连](#自动重连)
  - [传输模式](#传输模式)
- [Linux 服务器部署](#linux-服务器部署)
  - [编译并安装](#编译并安装)
  - [systemd 服务配置](#systemd-服务配置)
  - [管理服务](#管理服务)
- [常见问题](#常见问题)

---

## 编译

### 前提条件

项目依赖已包含在源码树中（`3rdparty/` 目录），无需安装额外的第三方库。

目录结构要求：

```
network/
├── common/           # 协议定义、通用工具
├── 3rdparty/
│   ├── kcp/          # ikcp
│   ├── monocypher/   # 加密库
│   └── cm256cc/      # FEC 库
├── client-cli/
│   ├── src/          # 共享源码
│   ├── linux/        # Linux Makefile
│   └── windows/      # Windows Makefile + build.bat
└── server/
```

### Linux (g++)

需要：g++ 4.8+ (支持 C++11)、make、pthread

```bash
cd client-cli/linux
make
```

生成可执行文件 `vlan-cli`。

### Windows (MinGW)

需要：MinGW-w64 (g++ 支持 C++11)

```cmd
cd client-cli\windows
mingw32-make
```

生成可执行文件 `vlan-cli.exe`（静态链接，可独立分发）。

### Windows (MSVC)

需要：Visual Studio 2015+ 或 Build Tools (支持 C++14)

```cmd
REM 先打开 "Developer Command Prompt" 或执行 vcvarsall.bat
cd client-cli\windows
build.bat
```

生成可执行文件 `vlan-cli.exe`。

---

## 使用方法

### 启动参数

```
vlan-cli [选项]

  -s HOST   服务器地址         (默认: 127.0.0.1)
  -p PORT   服务器端口         (默认: 11510)
  -n NAME   玩家名称           (不指定则交互式输入)
  -v        启用详细日志
  -h        显示帮助
```

示例：

```bash
# 连接到远程服务器
./vlan-cli -s example.com -p 11510 -n "Player1"

# 连接到本机服务器，启用详细日志
./vlan-cli -s 127.0.0.1 -v
```

### 交互命令

连接成功后可使用以下命令：

| 命令 | 说明 |
|---|---|
| `list` | 列出所有房间 |
| `create <name> [max] [mode] [fec] [password]` | 创建房间 |
| `join <roomId> [password]` | 加入房间 |
| `leave` | 离开当前房间 |
| `status` | 显示连接状态 |
| `peers` | 显示当前房间内的对等端 |
| `connect` | 手动重新连接服务器 |
| `quit` | 退出程序 |

**create 命令参数说明：**

- `name` — 房间名称
- `max` — 最大玩家数（默认 8）
- `mode` — 传输模式：`1`=P2P直连, `2`=KCP中继(默认), `3`=TCP中继, `4`=RawUDP中继
- `fec` — 前向纠错：`0`=关闭(默认), `1`=10%, `2`=30%, `3`=50%, `4`=70%, `5`=100%, `6`=200%
- `password` — 设置后启用加密，加入时需输入相同密码

### 典型流程

```
$ ./vlan-cli -s myserver.com -n "Host"
VLan CLI Client - Type 'help' for commands.
* Connecting to 1.2.3.4:11510 ...
* Connected, logging in as "Host"...
* Logged in, peerId=1
* NAT type: Full Cone
> create MyRoom 8 2 2
* Creating room "MyRoom" max=8 mode=2 fec=FEC 30% ...
* Room created (ID=1, IP=10.10.0.1)
* TUN adapter started, IP=10.10.0.1
> list
=== Room List (1 rooms) ===
  [1] "MyRoom"  1/8  mode=2 fec=None
=============================
> peers
--- Peers (0) ---
------------------
```

其他客户端加入后，`peers` 命令可看到对等端列表及其延迟。

---

## 功能说明

### 自动重连

客户端在与服务器的连接意外中断时会自动尝试重连：

- **最多 3 次**尝试，每次间隔 **3 秒**
- 如果断线前**在房间中**：重连成功后自动按房间名查找并加入原房间
  - 原房间仍存在 → 直接加入
  - 原房间已消失 → 以相同名称和设置创建新房间
- 如果断线前**未在房间中**：仅重连服务器，不加入任何房间
- 3 次尝试全部失败后，提示手动输入 `connect` 重试
- 主动执行 `leave` 后的断线不会自动加入房间
- 手动输入 `connect` 会取消正在进行的自动重连

服务器端对空房间有 **60 秒宽限期**：所有成员断开后，房间保留 60 秒而非立即删除。这确保快速重连的客户端能找到原房间。

### 传输模式

| 模式 | 说明 | 适用场景 |
|---|---|---|
| 1 (P2P) | UDP 直连 + NAT 打洞 | 低延迟场景，需要 NAT 兼容 |
| 2 (KCP) | 服务器 KCP 中继 | 默认推荐，稳定可靠 |
| 3 (TCP) | 服务器 TCP 中继 | NAT 极严格的环境 |
| 4 (RawUDP) | 服务器 Raw UDP 中继 | 需要低延迟中继 |

服务器自身使用 CLI 客户端连接时，**请使用中继模式**（2/3/4）。P2P 模式在服务器本机上无法工作（STUN 检测返回 loopback 地址）。

---

## Linux 服务器部署

以下说明如何在 Linux 服务器上安装 CLI 客户端，并通过 systemd 服务保持一个房间长期开放。

### 编译并安装

```bash
# 1. 将项目文件上传到服务器（假设在 /opt/vlan/）
cd /opt/vlan/client-cli/linux
make

# 2. 复制到系统目录
sudo cp vlan-cli /usr/local/bin/
sudo chmod +x /usr/local/bin/vlan-cli

# 3. 验证
vlan-cli -h
```

### systemd 服务配置

创建一个 expect 风格的 wrapper 脚本来自动执行创建房间命令：

```bash
sudo tee /usr/local/bin/vlan-room-keeper.sh << 'SCRIPT'
#!/bin/bash
# VLan Room Keeper - 自动创建并维持房间
# 用法: vlan-room-keeper.sh <server> <port> <name> <room> [max] [mode] [fec] [password]

SERVER="${1:-127.0.0.1}"
PORT="${2:-11510}"
NAME="${3:-RoomKeeper}"
ROOM="${4:-PublicRoom}"
MAX="${5:-8}"
MODE="${6:-2}"
FEC="${7:-0}"
PWD_ARG="${8}"

CMD="create $ROOM $MAX $MODE $FEC"
[ -n "$PWD_ARG" ] && CMD="$CMD $PWD_ARG"

exec /usr/local/bin/vlan-cli -s "$SERVER" -p "$PORT" -n "$NAME" <<EOF
$CMD
EOF
SCRIPT
sudo chmod +x /usr/local/bin/vlan-room-keeper.sh
```

> 说明：CLI 客户端启动后自动连接服务器，通过 stdin 传入 `create` 命令来创建房间。由于自动重连机制，即使连接中断也会自动恢复并回到同名房间。

创建 systemd 服务文件：

```bash
sudo tee /etc/systemd/system/vlan-room.service << 'EOF'
[Unit]
Description=VLan Room Keeper
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
ExecStart=/usr/local/bin/vlan-cli -s 127.0.0.1 -p 11510 -n RoomKeeper
StandardInput=text
StandardInputText=create PublicRoom 8 2 2
Restart=always
RestartSec=10
StartLimitIntervalSec=300
StartLimitBurst=10

[Install]
WantedBy=multi-user.target
EOF
```

**关键参数说明：**

| 参数 | 说明 |
|---|---|
| `-s 127.0.0.1` | 服务器地址（本机直接用 127.0.0.1） |
| `-p 11510` | 服务器端口（按实际修改） |
| `-n RoomKeeper` | 玩家名称（其他玩家可见） |
| `StandardInputText=create PublicRoom 8 2 2` | 自动创建房间：名称=PublicRoom, 最多8人, KCP模式, FEC 30% |
| `Restart=always` | 进程退出后自动重启 |
| `RestartSec=10` | 重启等待 10 秒 |

如果服务器是远程地址，将 `127.0.0.1` 改为对应 IP/域名。

如果需要连接远程服务器（非本机），请确保 CLI 运行在 **root** 权限下（需要创建 TUN 虚拟网卡），或授予 CAP_NET_ADMIN 权限：

```bash
sudo setcap cap_net_admin+ep /usr/local/bin/vlan-cli
```

### 管理服务

```bash
# 启用开机自启
sudo systemctl enable vlan-room.service

# 启动服务
sudo systemctl start vlan-room.service

# 查看状态
sudo systemctl status vlan-room.service

# 查看实时日志
sudo journalctl -u vlan-room.service -f

# 停止服务
sudo systemctl stop vlan-room.service

# 重启服务
sudo systemctl restart vlan-room.service
```

### 双重保障

服务配置了两层保活机制：

1. **应用层自动重连**：CLI 客户端内建的 3 次/3 秒间隔自动重连 + 自动回到原房间
2. **系统层进程重启**：如果 CLI 进程本身崩溃退出，systemd 的 `Restart=always` 会在 10 秒后重新启动进程

这意味着：
- 网络短暂中断 → 应用层自动重连（秒级恢复）
- 进程崩溃 → systemd 自动重启 → 新进程创建同名房间（10 秒级恢复）
- 服务器重启 → 应用层重连失败 → 进程退出 → systemd 重启 → 服务器恢复后自动连接创建房间

---

## 常见问题

### 1. "TUN init failed (need admin/root)"

CLI 客户端需要创建虚拟网络适配器，必须以管理员/root 权限运行：

- **Linux**: `sudo ./vlan-cli ...`
- **Windows**: 以管理员身份运行命令提示符

### 2. Windows 上提示找不到 WinTun

CLI 客户端使用 WinTun 驱动创建虚拟网卡。确保 `wintun.dll` 位于以下位置之一：
- 与 `vlan-cli.exe` 相同的目录
- 系统 PATH 中的目录

### 3. 编译时提示 "protocol.h not found"

确保你在正确的目录下编译：
- Linux: `cd client-cli/linux && make`
- Windows: `cd client-cli\windows && mingw32-make`（或 `build.bat`）

Makefile 使用相对路径引用 `../../common/` 下的头文件，必须保持项目目录结构完整。

### 4. 连接超时

- 确认服务器地址和端口正确
- 确认服务器正在运行
- 检查防火墙是否放行对应端口（默认 11510 TCP+UDP）
- 使用 `-v` 参数启用详细日志排查

### 5. P2P 模式打洞失败

P2P 直连模式依赖 NAT 打洞，不保证在所有网络环境下成功。如果打洞失败，建议改用 KCP 中继模式（mode=2）。

### 6. 服务器上使用 CLI 客户端 P2P 模式不工作

在 VLan 服务器同一台机器上运行 CLI 客户端时，STUN 检测返回 loopback 地址，P2P 模式无法正常工作。请使用中继模式（2=KCP, 3=TCP, 4=RawUDP）。

### 7. systemd 服务一直重启

检查 `journalctl -u vlan-room.service -n 50` 查看错误原因。常见问题：
- 服务器尚未启动（先启动 VLan 服务器）
- 权限不足（确保以 root 运行或已设置 CAP_NET_ADMIN）
- 地址/端口错误

### 8. 断线重连后看不到之前的房间

服务器对空房间有 60 秒宽限期。如果所有成员都断开超过 60 秒，房间会被删除。重连后客户端会自动以相同名称创建新房间。其他重连的客户端也能通过房间名找到并加入这个新房间。

### 9. 房间名重复问题

如果多个客户端同时断线重连，可能出现短暂的同名房间。这不影响功能——后续重连的客户端会找到第一个同名房间并加入。

### 10. 如何在后台运行（不使用 systemd）

```bash
# 使用 nohup
sudo nohup bash -c 'echo "create MyRoom 8 2" | /usr/local/bin/vlan-cli -s 127.0.0.1 -n Keeper' &

# 或使用 screen/tmux
sudo screen -dmS vlan bash -c 'echo "create MyRoom 8 2" | /usr/local/bin/vlan-cli -s 127.0.0.1 -n Keeper'
```

推荐使用 systemd 方案，它提供了进程监控、自动重启和日志管理。
