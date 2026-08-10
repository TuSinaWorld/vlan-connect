# VLan Server 部署指南

本文说明如何把 VLan 服务端安装到 Linux 服务器，并用 systemd 管理服务。

## 相关文档

- 项目入口和客户端说明: [../README.md](../README.md)
- CLI 客户端使用: [../client-cli/README.md](../client-cli/README.md)

## 服务端文件和安装路径

推荐使用以下路径:

| 文件或目录 | 目标路径 | 用途 |
|---|---|---|
| `vlan-server` | `/usr/local/bin/vlan-server` | 服务端可执行文件 |
| `server/vlan-server.service` | `/etc/systemd/system/vlan-server.service` | systemd unit |
| `server/vlan-server.env.example` | `/usr/local/bin/vlan-server.env` | 服务端环境配置 |
| `server/auth.password.example` | `/usr/local/bin/auth.password` | 服务端鉴权密码文件模板 |
| `server/DEPLOY.md` | `/usr/local/share/doc/vlan-server/DEPLOY.md` | 本部署文档 |
| 工作目录 | `/var/lib/vlan-server` | systemd 管理的状态目录 |
| 日志目录 | `/var/log/vlan-server` | 服务端日志目录 |

不要只把 `vlan-server` 一个二进制文件上传到服务器后直接启动。默认的 systemd 配置还会读取 `/usr/local/bin/vlan-server.env` 和 `/usr/local/bin/auth.password`，缺少这些文件会导致服务启动失败或行为不符合预期。

## 运行模式

- 服务端强制启用鉴权；未指定合法 `--auth-file` 时会在创建监听 socket 前退出。
- 客户端先完成服务端密码鉴权，之后信令、TCP Relay data channel、UDP relay frame 和 TUN payload 使用会话密钥传输。
- GUI/CLI 会拒绝声明 `authRequired=0` 的不安全服务端。
- 房间密码只用于加入房间校验，不参与服务端鉴权，也不作为传输加密密钥。

本仓库提供的 `server/vlan-server.service` 默认启用服务端鉴权，并通过 `--auth-file /usr/local/bin/auth.password` 读取密码文件。

## 方式一: 使用 Release 二进制包安装

适合不想在服务器上编译的用户。

### 1. 下载文件

从 GitHub Release 下载两个文件:

- `vlan-server-linux-x86_64.tar.gz` 或 `vlan-server-linux-arm64.tar.gz`
- `vlan-connect-source-vX.Y.Z.tar.gz`

第一个包只包含服务端二进制和许可证文件。第二个源码包包含 systemd unit、环境配置模板、密码模板和部署文档。

### 2. 上传到服务器

在本机执行，按你的服务器地址替换 `user@example.com`:

```bash
scp vlan-server-linux-x86_64.tar.gz vlan-connect-source-vX.Y.Z.tar.gz \
  user@example.com:/tmp/
```

如果服务器是 ARM64，把文件名换成 `vlan-server-linux-arm64.tar.gz`。

### 3. 在服务器解压

登录服务器:

```bash
ssh user@example.com
```

解压到临时目录:

```bash
mkdir -p /tmp/vlan-install
cd /tmp/vlan-install
tar -xzf /tmp/vlan-server-linux-x86_64.tar.gz
tar -xzf /tmp/vlan-connect-source-vX.Y.Z.tar.gz
```

解压后通常会得到:

```text
vlan-server-linux-x86_64/
vlan-connect-vX.Y.Z/
```

### 4. 创建目录

```bash
sudo install -d -o root -g root -m 0750 /var/lib/vlan-server
sudo install -d -o root -g root -m 0750 /var/log/vlan-server
sudo install -d -o root -g root -m 0755 /usr/local/share/doc/vlan-server
```

### 5. 安装文件

```bash
cd /tmp/vlan-install

sudo install -o root -g root -m 0755 \
  vlan-server-linux-x86_64/vlan-server \
  /usr/local/bin/vlan-server

sudo install -o root -g root -m 0644 \
  vlan-connect-vX.Y.Z/server/vlan-server.service \
  /etc/systemd/system/vlan-server.service

sudo install -o root -g root -m 0644 \
  vlan-connect-vX.Y.Z/server/DEPLOY.md \
  /usr/local/share/doc/vlan-server/DEPLOY.md

sudo install -o root -g root -m 0600 \
  vlan-connect-vX.Y.Z/server/vlan-server.env.example \
  /usr/local/bin/vlan-server.env

sudo install -o root -g root -m 0600 \
  vlan-connect-vX.Y.Z/server/auth.password.example \
  /usr/local/bin/auth.password
```

如果使用 ARM64 包，把 `vlan-server-linux-x86_64` 换成 `vlan-server-linux-arm64`。

## 方式二: 从源码构建后安装

适合需要自己编译服务端的用户。

### 1. 安装编译工具

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential git
```

Rocky/RHEL/Fedora:

```bash
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y git
```

### 2. 获取源码

可以直接 clone 仓库:

```bash
git clone https://github.com/TuSinaWorld/vlan-connect.git
cd vlan-connect
```

也可以从 Release 下载 `vlan-connect-source-vX.Y.Z.tar.gz`，上传到服务器后解压:

```bash
mkdir -p /tmp/vlan-build
cd /tmp/vlan-build
tar -xzf /tmp/vlan-connect-source-vX.Y.Z.tar.gz
cd vlan-connect-vX.Y.Z
```

### 3. 构建服务端

```bash
make -C server
```

成功后会生成:

```text
server/vlan-server
```

### 4. 安装文件

在源码根目录执行:

```bash
sudo install -d -o root -g root -m 0750 /var/lib/vlan-server
sudo install -d -o root -g root -m 0750 /var/log/vlan-server
sudo install -d -o root -g root -m 0755 /usr/local/share/doc/vlan-server

sudo install -o root -g root -m 0755 server/vlan-server /usr/local/bin/vlan-server
sudo install -o root -g root -m 0644 server/vlan-server.service /etc/systemd/system/vlan-server.service
sudo install -o root -g root -m 0644 server/DEPLOY.md /usr/local/share/doc/vlan-server/DEPLOY.md
sudo install -o root -g root -m 0600 server/vlan-server.env.example /usr/local/bin/vlan-server.env
sudo install -o root -g root -m 0600 server/auth.password.example /usr/local/bin/auth.password
```

## 配置服务端

### 1. 设置鉴权密码

编辑密码文件:

```bash
sudoedit /usr/local/bin/auth.password
```

把第一行改成你自己的强密码。不要继续使用模板中的占位值，也不要把正式密码写入仓库。

### 2. 检查环境配置

编辑环境配置:

```bash
sudoedit /usr/local/bin/vlan-server.env
```

默认配置:

```ini
VLAN_SERVER_PORT=11510
VLAN_SERVER_LOG=/var/log/vlan-server/server.log
VLAN_SERVER_LOG_MAX_MB=10
VLAN_SERVER_AUTH_FILE=/usr/local/bin/auth.password
VLAN_SERVER_EXTRA_ARGS=
```

说明:

- `VLAN_SERVER_PORT`: 服务端 TCP/UDP 共用端口。
- `VLAN_SERVER_LOG`: 服务端日志文件路径。
- `VLAN_SERVER_LOG_MAX_MB`: 单个日志文件大小上限，单位 MB。
- `VLAN_SERVER_AUTH_FILE`: 服务端鉴权密码文件。
- `VLAN_SERVER_EXTRA_ARGS`: 额外命令行参数。

服务端只从 `--auth-file PATH` 读取密码，不支持 `--auth` 或 `VLAN_SERVER_AUTH_PASSWORD`。密码文件只允许第一行密码和后续空行；密码为 8–256 字节，不得包含 NUL、额外非空行或全空白内容。

### 3. 容量上限

内置硬上限为 256 个信令客户端、64 个待分类连接、128 个房间、单 IPv4 32 个客户端/8 个待连接，以及 64 MiB 全局发送缓冲。以下启动参数只能下调，不能突破硬上限：

```text
--max-clients N
--max-pending N
--max-rooms N
--max-clients-per-ip N
--max-pending-per-ip N
--max-send-buffer-mb N
```

协议版本为 v8，服务端、GUI 和 CLI 必须同步发布；v7 客户端会收到版本不匹配并被拒绝。

## 防火墙和安全组

服务端需要同时开放 TCP 和 UDP。

ufw:

```bash
sudo ufw allow 11510/tcp
sudo ufw allow 11510/udp
sudo ufw reload
```

firewalld:

```bash
sudo firewall-cmd --permanent --add-port=11510/tcp
sudo firewall-cmd --permanent --add-port=11510/udp
sudo firewall-cmd --reload
```

如果使用云服务器，还需要在云厂商安全组里放行相同的 TCP/UDP 端口。

## 启动服务

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now vlan-server
sudo systemctl status vlan-server
```

查看日志:

```bash
journalctl -u vlan-server -f
sudo tail -f /var/log/vlan-server/server.log
```

确认端口监听:

```bash
ss -lntup | grep 11510
ss -lnu | grep 11510
```

## 修改配置

修改 `/usr/local/bin/vlan-server.env` 或 `/usr/local/bin/auth.password` 后重启:

```bash
sudo systemctl restart vlan-server
```

如果鉴权密码文件不存在、无法读取、为空、格式非法或长度不在 8–256 字节，服务会在监听端口前启动失败。

## 前台验证

需要排查 systemd 之外的问题时，可以先停止服务，然后前台运行:

```bash
sudo systemctl stop vlan-server
/usr/local/bin/vlan-server -p 11510 -l /tmp/vlan-server.log -L 10 \
  --auth-file /usr/local/bin/auth.password
```

验证结束后重新启动 systemd 服务:

```bash
sudo systemctl start vlan-server
```

## 更新服务端

如果使用 Release 二进制包:

```bash
cd /tmp/vlan-install
tar -xzf /tmp/vlan-server-linux-x86_64.tar.gz
sudo systemctl stop vlan-server
sudo install -o root -g root -m 0755 \
  vlan-server-linux-x86_64/vlan-server \
  /usr/local/bin/vlan-server
sudo systemctl start vlan-server
sudo systemctl status vlan-server
```

如果从源码构建:

```bash
cd /path/to/vlan-connect
make -C server
sudo systemctl stop vlan-server
sudo install -o root -g root -m 0755 server/vlan-server /usr/local/bin/vlan-server
sudo systemctl start vlan-server
sudo systemctl status vlan-server
```

## 排错

服务启动失败:

```bash
journalctl -u vlan-server -n 100 --no-pager
```

常见原因:

- `/usr/local/bin/vlan-server` 不存在或不可执行。
- `/usr/local/bin/auth.password` 不存在、为空，或权限导致服务进程无法读取。
- TCP/UDP `11510` 被其它进程占用。
- 云安全组或系统防火墙只放行了 TCP，没有放行 UDP。
- `/usr/local/bin/vlan-server.env` 内容有误。

## 卸载

```bash
sudo systemctl disable --now vlan-server
sudo rm -f /etc/systemd/system/vlan-server.service
sudo systemctl daemon-reload
sudo rm -f /usr/local/bin/vlan-server
sudo rm -f /usr/local/bin/vlan-server.env /usr/local/bin/auth.password
```

如果需要同时删除日志和状态目录:

```bash
sudo rm -rf /var/log/vlan-server /var/lib/vlan-server
```
