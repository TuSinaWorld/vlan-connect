# VLan Server 部署指南

本文面向新版本服务端。新版本不兼容旧客户端/旧服务端，升级时请确保 GUI、CLI、server 同步更新。

服务端默认监听同一个端口的 TCP 和 UDP，默认端口为 `11510`。TCP 用于信令和 TCP Relay data channel，UDP 用于 KCP/Raw UDP relay。

## 运行模式

- 无服务端鉴权：不启用传输层 SecureFrame 加密，但登录、房间、TCP/UDP 分流、FEC、Relay、断线重连等功能都应完整正常。
- 有服务端鉴权：客户端必须先完成服务端密码鉴权。鉴权成功后，业务信令、TCP Relay data channel、UDP relay 和 TUN payload 都使用会话密钥加密传输。
- 房间密码只用于房间加入控制，不参与传输加密派生。

本指南提供的 `vlan-server.service` 默认启用服务端鉴权，并固定带 `--auth-file /usr/local/bin/auth.password`。也就是说，按本文部署后默认就是鉴权加密模式。

## 路径约定

推荐按以下路径部署：

- 服务端程序：`/usr/local/bin/vlan-server`
- systemd unit：`/etc/systemd/system/vlan-server.service`
- 配置文件：`/usr/local/bin/vlan-server.env`
- 鉴权密码文件：`/usr/local/bin/auth.password`
- 工作目录：`/var/lib/vlan-server`
- 应用日志：`/var/log/vlan-server/server.log`

## 构建

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

构建服务端：

```bash
cd /path/to/network
make -C server clean all
```

成功后会生成：

```bash
server/vlan-server
```

## 安装

创建运行目录：

```bash
sudo install -d -o root -g root -m 0750 /var/lib/vlan-server
sudo install -d -o root -g root -m 0750 /var/log/vlan-server
sudo install -d -o root -g root -m 0755 /usr/local/share/doc/vlan-server
```

安装二进制、配置示例、模拟鉴权文件、文档和 systemd unit：

```bash
cd /path/to/network
sudo install -o root -g root -m 0755 server/vlan-server /usr/local/bin/vlan-server
sudo install -o root -g root -m 0644 server/vlan-server.service /etc/systemd/system/vlan-server.service
sudo install -o root -g root -m 0644 server/DEPLOY.md /usr/local/share/doc/vlan-server/DEPLOY.md

sudo install -o root -g root -m 0600 server/vlan-server.env.example \
  /usr/local/bin/vlan-server.env
sudo install -o root -g root -m 0600 server/auth.password.example \
  /usr/local/bin/auth.password
```

`server/auth.password.example` 只是用于模拟鉴权文件和首次启动验证。正式部署前必须替换为自己的强密码。

## 配置

编辑环境配置：

```bash
sudoedit /usr/local/bin/vlan-server.env
```

默认鉴权配置：

```ini
VLAN_SERVER_PORT=11510
VLAN_SERVER_LOG=/var/log/vlan-server/server.log
VLAN_SERVER_LOG_MAX_MB=10
VLAN_SERVER_AUTH_FILE=/usr/local/bin/auth.password
VLAN_SERVER_EXTRA_ARGS=
```

编辑鉴权密码文件：

```bash
sudoedit /usr/local/bin/auth.password
```

`/usr/local/bin/auth.password` 第一行写入服务端鉴权密码。不要把正式密码提交进仓库或写进 systemd unit 文件。

服务端也支持 `VLAN_SERVER_AUTH_PASSWORD=...`，但本 service 默认使用 `--auth-file`，不推荐长期用环境变量保存密码。

## 防火墙

必须同时放行 TCP 和 UDP。

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

云服务器还需要在安全组中放行同样的 TCP/UDP 端口。

## 启动

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now vlan-server
sudo systemctl status vlan-server
```

查看日志：

```bash
journalctl -u vlan-server -f
sudo tail -f /var/log/vlan-server/server.log
```

确认端口监听：

```bash
ss -lntup | grep 11510
ss -lnu | grep 11510
```

## 修改配置

修改 `/usr/local/bin/vlan-server.env` 或 `/usr/local/bin/auth.password` 后重启：

```bash
sudo systemctl restart vlan-server
```

本 service 默认必须读取鉴权文件；如果鉴权文件不存在、为空或权限错误，服务会启动失败。

## 升级

```bash
cd /path/to/network
make -C server clean all
sudo systemctl stop vlan-server
sudo install -o root -g root -m 0755 server/vlan-server /usr/local/bin/vlan-server
sudo systemctl start vlan-server
sudo systemctl status vlan-server
```

本项目新版本不做旧 wire format 兼容。升级时请同时更新服务端、GUI 客户端和 CLI 客户端。

## 排错

服务启动失败：

```bash
journalctl -u vlan-server -n 100 --no-pager
```

常见原因：

- `/usr/local/bin/vlan-server` 不存在或不可执行。
- `/usr/local/bin/auth.password` 不存在、为空，或权限导致服务进程无法读取。
- TCP/UDP `11510` 被其他进程占用。
- 云安全组或系统防火墙只放行了 TCP，没有放行 UDP。
- 客户端和服务端版本不一致，协议版本会被拒绝。

临时前台运行验证：

```bash
/usr/local/bin/vlan-server -p 11510 -l /tmp/vlan-server.log -L 10 \
  --auth-file /usr/local/bin/auth.password
```

## 卸载

```bash
sudo systemctl disable --now vlan-server
sudo rm -f /etc/systemd/system/vlan-server.service
sudo systemctl daemon-reload
sudo rm -f /usr/local/bin/vlan-server
sudo rm -f /usr/local/bin/vlan-server.env /usr/local/bin/auth.password
```

如需连配置和日志一起删除：

```bash
sudo rm -rf /var/log/vlan-server /var/lib/vlan-server
```
