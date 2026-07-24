# 协议 v7 隔离回归测试

本目录包含需要主动启用的协议 v7 回归测试。正式服务端 Makefile、GUI qmake 工程、CLI Makefile 和发布打包流程均不引用本目录。

## 隔离规则

- 测试源码和 Fake 实现全部位于 `tests/` 下。
- 每个测试目标都是拥有独立 `main()` 的单独进程。
- 测试产物必须输出到 `build-tests/windows/<config>/` 或 `build-tests/linux/<config>/`，不得将目标文件写入正式源码目录。
- 网络测试只绑定回环地址并使用临时端口。
- 自动测试不会创建真实 Wintun 适配器，也不会修改路由或防火墙。
- 临时文件统一写入操作系统临时目录，并由创建它的测试负责清理。
- 测试二进制和测试数据不会被复制到正式发布包。

## 测试目标

- `protocol_v7_tests`：ByteBuffer 和信令/数据 payload 严格校验。
- `server_session_tests`：状态转换和信令 FD 索引约束。
- `gui_signal_tests`：GUI 信令客户端的帧处理及回调行为。
- `cli_signal_tests`：CLI 信令客户端的帧处理及回调行为。
- `data_channel_tests`：GUI/CLI 数据通道畸形帧隔离。
- `data_plane_tests`：停止、明文和安全模式的数据面输出规则。
- `wintun_lifecycle_tests`：Fake Wintun 的 shutdown 和资源释放顺序。

正式行为通过正常的内部接口提供可测试性。测试不得使用 `#define private public`、隐藏的正式程序测试命令，也不得让正式二进制链接 Fake 实现。

这些目标不会参与正常工程构建。按照仓库规则，执行任何构建或测试命令前都必须获得用户明确许可。

`tests/CMakeLists.txt` 是唯一的测试构建入口，服务端、GUI 和 CLI 正式工程均不包含它。

多配置生成器将每个测试程序输出到 `build-tests/<platform>/<config>/`。单配置生成器默认使用隔离的 `Debug` 目录，除非显式指定其它配置。

如果 CMake binary directory 不在仓库的 `build-tests/` 目录下，配置过程会直接失败，从而避免目标文件和生成的工程文件进入 `tests/` 或正式源码目录。
