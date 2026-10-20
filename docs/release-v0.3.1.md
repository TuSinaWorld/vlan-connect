# v0.3.1 / 协议 v9

## 升级要求

服务端、Windows Qt GUI、Windows/Linux CLI 必须同步升级。协议 v9 明确拒绝 v8，不提供静默降级。软件版本与协议版本分别显示；CLI 和服务端的 `--version` 不触发密码读取、网卡配置或网络连接。

## 鉴权与连接

保留 X25519、会话加密及双向证明。握手增加随机盐及固定 KDF 配置编号，客户端拒绝未知编号；固定配置使用 Argon2id、19 MiB、2 次迭代、单路并行。每次握手只派生一次密码密钥，预先计算服务端证明，派生计算移至后台，取消或替换连接后丢弃过期结果。

会话派生绑定协议、双方公钥/随机数、盐及 KDF 配置；全零 X25519 共享值被拒绝。密码错误、协议不匹配和服务端证明失败停止自动重试，网络故障继续退避重连。日志记录连接阶段耗时而不记录秘密。

这些改动主要改善首次连接、重连、界面响应和故障定位；Argon2 不在逐包路径，增强派生成本后不承诺总握手耗时一定更短，也不承诺降低游戏 RTT。此协议不是 PAKE，主动伪装服务器仍可能收集离线口令猜测材料；请使用高熵鉴权密码。暂不引入 0-RTT、PAKE、TLS/QUIC 迁移或未经测量的 KCP/FEC/MTU 调参。

## Windows 数据面与体验

GUI 和 Windows CLI 共用网卡/路由/防火墙配置，使用实际网卡名、无默认网关和低接口 metric。关键配置失败会取消入房并清理资源。CLI 入房立即登记 UDP 端点，全 TCP Relay 房间允许 UDP 不可用；区分入房、中继验证及数据面就绪。

防火墙预清理、添加、重试、删除有明确结果、错误码、耗时和失败原因，命令执行有超时。CLI 支持轮转文件日志及 `--log-file`。GUI 保留启动时屏幕工作区/DPI 适配，以及双击后由 Windows 请求管理员权限。

## 发布资产与验证边界

同一正式 tag 的源码、Linux x86_64/arm64 服务端和 CLI、Windows x64 CLI、Windows x64 Qt GUI 包使用同一提交，并附 `SHA256SUMS.txt`。Linux 二进制保持 glibc 2.31 基线；GUI 保持 Qt 5.9.9 / MSVC 2015 x64。GUI 可在 Windows 本地构建后上传，发布保持草稿直至所有资产、版本、提交及校验和齐全。

自动安装器继续选择最新稳定 tag，校验源码及二进制版本，保留配置和密码，失败回滚。它不会直接追踪 main。真实多机 Wintun、路由、防火墙和游戏互通不由 CI 保证；排查方法见 [互通文档](connectivity-troubleshooting.md)。

## 手动 GUI 资产

从正式 tag 的干净检出构建，向 qmake 显式传入 `BUILD_COMMIT=<完整40位提交>`。运行 `windeployqt --release`，附官方 x64 Wintun DLL 和现有许可证材料，并确认内嵌 `requireAdministrator` 及 DPI 清单。ZIP 命名为 `vlan-gui-windows-x86_64-v0.3.1.zip`。根目录 `SOURCE.txt` 必须包含以下独立行：

```text
Release tag: v0.3.1
Build commit: <正式 tag 对应的完整40位提交>
Protocol version: 9
```

上传到草稿 Release 后重跑 Release 工作流。工作流验证所有包的对应源码元数据，生成统一校验和后发布；失败时保持草稿，不能把部分资产视为完成发布。
