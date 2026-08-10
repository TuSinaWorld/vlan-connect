# 协议 v8 隔离回归测试

本目录包含协议 v8 回归测试。正式服务端 Makefile、GUI qmake 工程、CLI Makefile 和发布打包流程均不引用本目录；`.github/workflows/build.yml` 会在 Linux CI 中独立配置、构建并运行这些目标。

## 隔离规则

- 测试源码和 Fake 实现全部位于 `tests/` 下。
- 每个测试目标都是拥有独立 `main()` 的单独进程。
- 测试产物必须输出到 `build-tests/windows/<config>/` 或 `build-tests/linux/<config>/`，不得写入正式源码目录。
- 网络测试只绑定回环地址并使用临时端口。
- 自动测试不会创建真实 Wintun 适配器，也不会修改路由或防火墙。
- 临时文件统一写入操作系统临时目录，并由创建它的测试负责清理。
- 测试二进制和测试数据不会被复制到正式发布包。

## 测试目标

- `protocol_v8_tests`：ByteBuffer u64、v8 分页快照/delta 以及信令/数据 payload 严格校验。
- `server_auth_file_tests`：服务端鉴权密码文件边界和非法内容校验。
- `server_install_script_tests`：一键安装器的 Bash/ShellCheck 静态检查、参数边界、tag 选择、安全配置解析和回滚辅助逻辑。
- `server_session_tests`：状态转换和信令 FD 索引约束。
- `gui_signal_tests`：GUI 信令帧处理和回调行为。
- `cli_signal_tests`：CLI 信令帧处理和 v7 拒绝。
- `data_channel_tests`：GUI/CLI 数据通道畸形帧隔离。
- `data_plane_tests`：仅安全模式的数据面状态以及 IPv4 overlay 校验。
- `linux_tun_netlink_tests`：CLI Linux 原生 rtnetlink 消息、ACK 失败与回滚。
- `cli_raw_fec_tests`：生产 Raw UDP/FEC 重组元数据、活动项上限、内存预算与最旧项淘汰。
- `wintun_lifecycle_tests`：Fake Wintun 的 shutdown 和资源释放顺序。

正式行为通过正常的生产接口提供可测试性。测试不得使用 `#define private public`，也不得让正式二进制链接 Fake 实现。

这些目标不参与正常工程构建。按照仓库规则，执行任何构建或测试命令前必须获得用户明确许可。

`tests/CMakeLists.txt` 是唯一的测试构建入口；服务端、GUI 和 CLI 正式工程均不包含它。多配置生成器将每个测试程序输出到 `build-tests/<platform>/<config>/`。如果 CMake binary directory 不在仓库的 `build-tests/` 下，配置过程会直接失败。
