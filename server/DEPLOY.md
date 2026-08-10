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
| 版本目录 | `/opt/vlan-server/releases/<tag>/` | 一键安装器保留的已安装版本 |
| 当前版本 | `/opt/vlan-server/current` | 原子指向当前版本的软链接 |
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

## 一键源码安装和更新

推荐在使用 systemd 的 Debian、Ubuntu、RHEL、Rocky Linux、AlmaLinux 或 Fedora 服务器上使用一键安装器。脚本必须以 root 运行，会自动安装编译依赖，从官方仓库选择最新的 `vX.Y.Z` 正式 tag，在临时目录完成源码编译，然后部署并启用服务。

交互安装或更新:

```bash
curl -fsSL https://raw.githubusercontent.com/TuSinaWorld/vlan-connect/main/server/install.sh | sudo bash
```

首次安装会询问 TCP/UDP 共用端口和服务端鉴权密码。端口留空使用 `11510`；密码留空会从 `/dev/urandom` 生成 64 位十六进制密码，并仅在成功安装后显示一次。用户可以选择继续配置日志大小和全部容量限制。

脚本默认使用 `auto` 操作：未安装时执行安装，已有安装时更新到最新正式 tag，并保留现有端口、密码和容量配置。也可以显式指定操作:

```bash
# 只允许首次安装
curl -fsSL https://raw.githubusercontent.com/TuSinaWorld/vlan-connect/main/server/install.sh \
  | sudo bash -s -- install

# 要求已有安装并更新
curl -fsSL https://raw.githubusercontent.com/TuSinaWorld/vlan-connect/main/server/install.sh \
  | sudo bash -s -- update

# 安装指定正式 tag；显式指定旧 tag 时允许降级
curl -fsSL https://raw.githubusercontent.com/TuSinaWorld/vlan-connect/main/server/install.sh \
  | sudo bash -s -- update --version v0.3.0

# 更新程序并重新进入配置流程
curl -fsSL https://raw.githubusercontent.com/TuSinaWorld/vlan-connect/main/server/install.sh \
  | sudo bash -s -- update --reconfigure
```

### GitHub 不可访问时使用本地源码包

在可以访问 GitHub 的电脑上下载 `server/install.sh` 和同一正式版本的
`vlan-connect-source-vX.Y.Z.tar.gz` Release 资产，再通过 SCP、运维平台或可信的
内部文件服务传到目标服务器。例如文件已经放到 `/tmp`:

```bash
sudo bash /tmp/install.sh \
  --source-archive /tmp/vlan-connect-source-vX.Y.Z.tar.gz
```

安装器会从归档内唯一的 `vlan-connect-vX.Y.Z/` 顶层目录识别版本，不查询远程
tag，也不执行 `git clone`。归档必须不超过 256 MiB，只能包含普通文件和目录，
且必须具有 `server/Makefile`、systemd unit 和部署文档；多顶层目录、路径穿越、
软链接、硬链接及其它特殊条目都会被拒绝。

`--version` 在本地源码包模式下不是必需参数；如果同时提供，其值必须与归档
目录识别出的版本完全一致。本地源码包仍然需要 apt、dnf 或 yum 可用，以安装
编译依赖。没有指定 `--source-archive` 时，安装和更新继续使用原有的官方 GitHub
tag 查询及浅克隆流程。

无人值守安装建议通过 root 可读的临时密码文件传入秘密，避免把密码写在命令行历史中:

```bash
sudo install -o root -g root -m 0600 /path/to/password.txt /root/vlan-auth.password
curl -fsSL https://raw.githubusercontent.com/TuSinaWorld/vlan-connect/main/server/install.sh \
  | sudo bash -s -- --non-interactive --port 11510 \
      --password-file /root/vlan-auth.password
```

支持的配置参数:

```text
--port N
--password-file FILE
--log-max-mb N
--max-clients N
--max-pending N
--max-rooms N
--max-clients-per-ip N
--max-pending-per-ip N
--max-send-buffer-mb N
--version vX.Y.Z
--source-archive FILE
--non-interactive
--reconfigure
```

无人值守环境也可以使用对应的 `VLAN_INSTALL_*` 环境变量，包括 `VLAN_INSTALL_SOURCE_ARCHIVE`。密码优先使用 `VLAN_INSTALL_PASSWORD_FILE`；`VLAN_INSTALL_PASSWORD` 仅作为受控自动化环境的备用方式。参数优先级为命令行或密码文件、安装器环境变量、已有配置、交互输入、默认值。

安装器不会执行 `ufw` 或 `firewall-cmd`。结束时会显示需要开放的 TCP 和 UDP 端口，管理员仍需配置主机防火墙以及云厂商安全组。

### 更新安全和自动回滚

- 依赖安装、在线模式的 tag 查询、本地归档检查和编译全部在停止现有服务前完成。
- 二进制安装到 `/opt/vlan-server/releases/<tag>/`，通过 `/opt/vlan-server/current` 和 `/usr/local/bin/vlan-server` 软链接原子切换。
- 更新前会临时快照当前二进制链接、systemd unit、环境文件、密码、文档和版本状态。
- 新服务无法在 10 秒内进入 active 时，会恢复全部快照并重新启动旧版本。
- 普通在线自动更新拒绝降级；显式 `--version` 或主动提供的本地旧版本源码包可以选择旧版本，并会显示警告。
- 同一 tag 已安装、服务已启用且运行正常时不会重复编译；文件缺失或服务异常时会执行修复安装。
- 并发运行的安装器会通过 `/run/lock/vlan-server-installer.lock` 互斥。

如需先审查脚本再执行:

```bash
curl -fsSLo /tmp/vlan-server-install.sh \
  https://raw.githubusercontent.com/TuSinaWorld/vlan-connect/main/server/install.sh
less /tmp/vlan-server-install.sh
sudo bash /tmp/vlan-server-install.sh
```

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

推荐重复执行[一键源码安装和更新](#一键源码安装和更新)中的命令；安装器会保留现有配置，并在新服务启动失败时自动回滚。以下步骤仅用于手工部署。

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
- 无法连接 GitHub，或目标正式 tag 不存在；这种情况下可以改用正式 Release 的本地源码包安装模式。
- 当前系统不是 systemd，或发行版不在 apt/dnf/yum 支持范围内。
- 编译依赖安装失败。安装器会在停止旧服务前退出，不影响现有服务。

一键更新失败后可检查服务状态和回滚结果:

```bash
sudo systemctl status vlan-server --no-pager
sudo journalctl -u vlan-server -n 100 --no-pager
sudo cat /var/lib/vlan-server/installed-version
readlink -f /opt/vlan-server/current
```

## 卸载

```bash
sudo systemctl disable --now vlan-server
sudo rm -f /etc/systemd/system/vlan-server.service
sudo systemctl daemon-reload
sudo rm -f /usr/local/bin/vlan-server
sudo rm -f /usr/local/bin/vlan-server.env /usr/local/bin/auth.password
sudo rm -rf /opt/vlan-server
sudo rm -rf /usr/local/share/doc/vlan-server
sudo rm -f /run/lock/vlan-server-installer.lock
```

如果需要同时删除日志和状态目录:

```bash
sudo rm -rf /var/log/vlan-server /var/lib/vlan-server
```
