# Windows 房间成员互通排查

连接中心服务器、看到房间成员或收到“中继已配置”只表示信令正常。成员 TCP/UDP 延迟探针收到回复后，才证明对应的成员中继链路双向可用；探针不经过 Wintun，也不能单独证明游戏或系统 ping 可用。

## 先判断故障在哪一层

| 现象 | 优先检查 |
| --- | --- |
| 服务器延迟正常，成员延迟持续 `--` | 实际生效的房间流量策略、成员中继建立、UDP 端口放行、TCP 数据通道/信令中继、服务端转发 |
| 成员延迟正常，ping 对方 `10.10.0.x` 失败 | VLan 地址状态、Windows 路由、源地址、防火墙及安全软件 |
| ping 和游戏直接连接正常，发现不到房间 | `255.255.255.255` / `10.10.0.255` 广播选路、游戏绑定的网卡和发现协议 |

ping 属于非 TCP 流量，使用房间的“UDP/非 TCP”策略。排除外层 UDP 被阻断时，创建新房间，把 **TCP 和 UDP/非 TCP 两项都设置为 TCP Relay**；只修改其中一项不够。用小包 ping 验证，不需要先反复更改 MTU 或 FEC。

服务端默认端口为 `11510`。TCP 信令可连接不代表同端口的 UDP 已在云安全组、防火墙、容器端口映射或网络出口放行。KCP 和 Raw UDP 都使用外层 UDP；TCP Relay 使用 TCP，并能在独立数据通道不可用时通过已鉴权的信令连接转发。

旧版客户端在单次函数调用中连续读取 transport/FEC/profile，依赖未规定的 C++ 参数求值顺序。逆序读取会把 transport 与 profile 对调，并将所选传输模式回退到默认 Raw UDP/KCP。修复后的客户端按线格式顺序读取每个字段；协议 v8 的线格式保持不变。部署修复时，房间成员都应升级，避免混用仍存在此缺陷的客户端。

## 修复后的自动保护

- 房间使用 UDP 中继时，入房即发送鉴权 UDP keepalive，登记中继端点；UDP 绑定失败会阻止需要 UDP 的数据面启动，发送失败会记录原因。两项策略均为 TCP Relay 的房间可在 UDP 绑定失败时继续使用 TCP。
- 中继配置完成后仍显示等待成员验证。连续约 10 秒没有有效成员探针回复时显示故障原因和检查方向；持续失联期间不重复刷屏，恢复后重新监测。3 秒采样周期会使首次提示稍晚于 10 秒。
- VLan 网卡不设置默认网关，接口 metric 为 5，限定广播使用 on-link 路由。Windows 按接口 metric 与路由 metric 的总和选路，原来的 9999 会使广播通常输给物理网卡。
- IP、metric、MTU、防火墙规则或广播路由配置失败会取消入房并清理本次资源。Wintun session 启动后核对广播实际选中的接口及源地址，允许有限的地址就绪等待；冲突会明确报错。
- 中心服务器地址落在 `10.10.0.0/24` 时拒绝入房，避免虚拟网段抢走中心服务器流量。
- 默认日志记录数据包校验、路由缺失、UDP 会话/解密拒绝、发送及网卡写入失败的计数；每 5 秒最多记录一条聚合丢包消息。预期的 IPv6 流量不会作为 IPv4 故障报错。

## 取得诊断信息

GUI 默认保存脱敏的普通/错误日志到 Windows 当前用户的应用数据目录，通常为 `%LOCALAPPDATA%\VLan\logs\client.log`，实际路径在客户端日志页启动消息中显示。日志轮转为 `client.log.1`、`client.log.2`，每个文件约 512 KiB。逐包详细日志仍仅在当前进程的详细日志视图中保留。日志写入需要该目录可写。

在故障仍发生、客户端仍在房间中时，用 PowerShell 读取：

```powershell
Get-NetAdapter | Format-Table Name, ifIndex, Status
Get-NetIPAddress -AddressFamily IPv4 | Format-Table InterfaceAlias, IPAddress, PrefixLength, AddressState
Get-NetIPInterface -AddressFamily IPv4 | Format-Table InterfaceAlias, InterfaceMetric, NlMtu, ConnectionState
Get-NetRoute -AddressFamily IPv4 | Where-Object { $_.DestinationPrefix -like '10.10.0.*' -or $_.DestinationPrefix -eq '255.255.255.255/32' } | Format-Table DestinationPrefix, InterfaceAlias, NextHop, RouteMetric
Get-NetFirewallRule -DisplayName 'VLan Virtual LAN' | Format-Table DisplayName, Enabled, Direction, Action, Profile
Find-NetRoute -RemoteIPAddress 10.10.0.3
ping -n 4 -l 32 10.10.0.3
```

把 `10.10.0.3` 替换为实际的另一位成员虚拟 IP。`Find-NetRoute` 用来确认该成员流量实际走 VLan、源地址为本机分配的虚拟 IP；特别检查其它 VPN、虚拟机或实体网络的相同网段，以及更具体的 `/32` 路由。

服务端可读取 `journalctl -u vlan-server --since '-10 minutes' --no-pager`，结合客户端的 peer ID 核对 UDP 目标未登记、会话解密拒绝及转发失败。客户端普通日志和服务端采样诊断不记录密码或数据包正文。

Windows 的显式阻止规则、域策略或第三方安全软件可能覆盖程序添加的允许规则。程序无法保证这些外部配置一定允许流量；应据诊断修正具体规则。游戏主动绑定物理网卡、使用 IPv6、组播或二层广播的情况，也不能仅凭 IPv4 ping 或中继延迟判定为支持。

## 验证范围与发布前验收

本次在 Linux/C++11 环境完成 11 项隔离测试，其中 Qt 5.15 的 GUI 测试使用生产 `SignalClient`，通过回环 TCP 完成加密鉴权，验证创建房间、三成员入房、房间列表快照和 delta 的策略回调。仅将 GUI 解析文件换回旧实现时，同一测试在流量策略一致性断言处失败。服务端和 CLI 修改文件的 C++11 语法检查也通过。

这些结果验证了解析缺陷和超时监测逻辑，不能代替 Windows 实机验收。当前环境未编译完整 Windows GUI，未运行真实 Wintun、Windows 路由或防火墙配置；Linux 下的 `wintun_lifecycle_tests` 是平台占位程序。

发布前用三个 Windows 客户端验收：

1. 所有成员升级修复后的客户端，先创建两项流量策略均为 TCP Relay 的房间。
2. 等待三个成员之间两类延迟均显示毫秒数，逐对 ping 虚拟 IP，再验证游戏直接连接和房间发现。
3. 确认服务端 UDP 已放行，再分别测试 Raw UDP、KCP、混合策略及需要的 FEC/MTU 组合。断开重连后重复成员延迟及 ping 验证。
4. 在隔离测试网络中阻断外层 UDP：UDP 房间应记录成员探针超时，两项均为 TCP Relay 的新房间应仍可互通。
5. 退出客户端后确认普通日志仍在；制造同网段路由冲突或配置失败时，确认入房失败能显示具体原因并清理本次资源。

没有现场日志，无法将历史故障唯一归因于解析缺陷；新日志和分层验收用于定位仍可能存在的外部网络及 Windows 配置问题。
