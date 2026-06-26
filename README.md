# VLan - 虚拟局域网游戏联机工具

通过虚拟网卡 + UDP 隧道，让不同网络的玩家像在同一局域网内一样联机游戏。

## 开源许可证

本项目以 GNU General Public License v3.0 发布。完整许可证正文见根目录 `LICENSE`，第三方组件说明见 `NOTICE`。

## 架构概览

```
客户端 (Windows)                    服务器 (Ubuntu 22.04)
┌──────────────┐                   ┌──────────────────┐
│ Qt 5.9.9 UI  │                   │ TCP 信令 (11510)  │
│ WinTun 虚拟网卡│   ◄── UDP ──►   │ UDP STUN/中继(11510)│
│ KCP 可靠传输  │                   │ UDP/TCP 中继      │
│ NAT 穿透     │                   │ 房间管理          │
└──────────────┘                   └──────────────────┘
```

- **客户端**: C++11 / Qt 5.9.9 / MSVC 2015 / Windows
- **服务端**: C++11 / 纯 POSIX sockets + epoll / Ubuntu 22.04 LTS (无 Qt 依赖)

---

## 服务器部署指南 (Ubuntu 22.04 LTS)

### 1. 安装编译环境

```bash
# 安装编译工具 (gcc/g++/make)
sudo apt install -y build-essential

# 验证版本
g++ --version    # 应显示 11.x 或更高
make --version
```

> **⚠ 注意**: 如果服务器上还运行着其他服务，请**不要**随意执行 `sudo apt upgrade`。
> 全量升级可能更新正在运行的依赖库/内核，导致其他服务异常或宕机。
> 只需确保 `build-essential` 已安装即可，无需升级整个系统。

这就是服务器所需的**全部依赖**。不需要 Qt、不需要 CMake、不需要任何第三方库。

### 2. 上传代码

将整个 `network/` 目录上传到服务器，例如放到 `/opt/vlan/`：

```bash
# 方式1: scp (从本地机器执行)
scp -r network/ user@your-server:/opt/vlan/

# 方式2: 如果用 git
git clone <your-repo-url> /opt/vlan
```

### 3. 确认 KCP 源码

```bash
cd /opt/vlan/3rdparty/kcp
# 如果目录中已经有 ikcp.h 和 ikcp.c，可跳过下载
curl -LO https://raw.githubusercontent.com/skywind3000/kcp/master/ikcp.h
curl -LO https://raw.githubusercontent.com/skywind3000/kcp/master/ikcp.c
```

### 4. 编译

```bash
cd /opt/vlan/server
make
```

编译成功后会在 `server/` 目录下生成 `vlan-server` 可执行文件。

### 5. 配置防火墙

```bash
# 开放所需端口
sudo ufw allow 11510/tcp    # TCP 信令 + TCP 数据通道
sudo ufw allow 11510/udp    # UDP STUN + UDP 中继
sudo ufw reload
```

如果服务器在云平台 (阿里云/腾讯云/AWS 等)，还需要在**安全组规则**中放行这些端口。

### 6. 运行

```bash
# 前台运行 (调试)
./vlan-server

# 自定义端口
./vlan-server -p 11510

# 写入日志文件并限制日志大小
./vlan-server -p 11510 -l /var/log/vlan-server.log -L 10

# 后台运行
nohup ./vlan-server > /var/log/vlan-server.log 2>&1 &
```

### 7. 设置开机自启 (可选)

创建 systemd 服务：

```bash
sudo tee /etc/systemd/system/vlan-server.service > /dev/null << 'EOF'
[Unit]
Description=VLan Virtual LAN Server
After=network.target

[Service]
Type=simple
ExecStart=/opt/vlan/server/vlan-server -p 11510 -l /var/log/vlan-server.log -L 10
Restart=always
RestartSec=5
User=nobody
Group=nogroup
LimitNOFILE=65536
# 如需修改端口，编辑上方 ExecStart 行的 -p 参数：
#   -p  服务端口 (默认 11510，同时用于 TCP 信令/TCP 数据通道和 UDP STUN/中继)
#   -l  日志文件路径
#   -L  日志文件大小上限，单位 MB

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable vlan-server
sudo systemctl start vlan-server

# 查看状态
sudo systemctl status vlan-server
# 查看日志
sudo journalctl -u vlan-server -f
```

---

## 客户端构建指南 (Windows)

### 前置条件

- Visual Studio 2015 (MSVC 14.0)
- Qt 5.9.9 (已配置好 qmake)

### 1. 下载依赖

**KCP:**
```
3rdparty/kcp/ 目录下放入 ikcp.h 和 ikcp.c
下载: https://github.com/skywind3000/kcp
```

**WinTun:**
```
3rdparty/wintun/ 目录下放入 wintun.h
构建输出目录下放入 wintun.dll (amd64)
下载: https://www.wintun.net/
```

### 2. 编译

```cmd
cd network
qmake network.pro
nmake      (或用 Qt Creator 打开 network.pro 直接构建)
```

### 3. 运行

- 以**管理员身份**运行 `VLanClient.exe`（WinTun 需要管理员权限创建虚拟网卡）
- 确保 `wintun.dll` 在 exe 同目录下

---

## 使用流程

1. 服务器启动 `vlan-server`
2. 所有玩家启动客户端，填入服务器地址 (如 `1.2.3.4:11510`)，输入昵称，点击"连接"
3. 一人创建房间，其他人刷新房间列表后加入
4. 系统自动完成：NAT 检测 → 按房间传输模式建立 KCP/Raw UDP/TCP 中继或 P2P 直连。默认推荐使用中继 KCP；P2P 直连失败时需要改用中继模式
5. 虚拟网卡就绪后，游戏中选择局域网联机，即可看到对方

---

## 端口说明

| 端口  | 协议 | 用途                        |
|-------|------|-----------------------------|
| 11510 | TCP  | 信令/房间管理 + TCP 数据通道 |
| 11510 | UDP  | STUN 探测 + UDP 数据中继    |

---

## 网络不影响说明

虚拟网卡使用独立子网 `10.10.0.0/24`，**不设为默认网关**，不修改 DNS，不影响正常上网。
软件退出时自动清理虚拟网卡。仅游戏的 LAN 流量经过虚拟网卡，其它流量走物理网卡。
