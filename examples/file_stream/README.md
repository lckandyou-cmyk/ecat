# 宿主文件流参考入口

此可选模块读取调用方提供的二进制 FILE 流，将 EFW 字节送入通用升级管理器。它用于验证第二种入口的架构契约，不是 UART/CAN/USB/网络协议实现，也不提供真实 Flash 驱动。

## 构建

```sh
# 文件流默认关闭。此处只构建通用引擎和文件入口，不含 FoE 适配器。
make build-upgrade ENABLE_FOE=0 ENABLE_FILE=1 UPGRADE_BUILD_DIR=build/file-upgrade
# 两入口同时启用，FoE 默认选择 SSC 适配器。
make build-upgrade ENABLE_FILE=1 UPGRADE_BUILD_DIR=build/shared-upgrade
```

需要重新构建时使用新目录，或先用 `make clean BUILD_DIR=build/file-upgrade` 清理已登记的构建产物。

## 接入次序

1. 初始化目标注册表与 `ecat_transfer_t`，固件目标还需配置固件核心；异步目标须配置对应异步后端和暂存区。
2. 用 `ecat_transfer_file_init` 建立文件入口并提供接收缓冲。文件流以二进制读取方式打开；固件导入时流应定位到 EFW 头部。
3. 构造 `ecat_transfer_request_t`，指定通道 ID、目标 ID 和 `ECAT_TRANSFER_DIR_IMPORT`，固件目标还需填写候选版本。`expected_size` 可为 0 表示长度未知；返回 BUSY 或 DENIED 时不会读取文件。
4. 每次调度调用 `ecat_transfer_file_step`。PENDING 表示继续调度；等待存储操作期间只轮询、不读取下一块。COMPLETE 表示提交成功；FAILED 时检查管理器诊断，`io_error` 表示文件读取失败。
5. 超时或用户取消时调用 `ecat_transfer_file_cancel`。文件入口不会关闭或回绕 FILE，调用方负责 `fclose`。入口、流和缓冲必须活到操作终止，活跃入口不可移动或复制。

FoE 和文件流应绑定同一个传输管理器，共用目标注册表、访问控制和会话仲裁。本参考适配器只实现文件流导入；数据导出可通过通用传输端点实现。

fread 可能阻塞，此入口仅作宿主参考；不能直接放进 ISR 或声称具有 MCU 实时时序保证。掉电安全与启动检查仍由目标驱动和 Bootloader 实现。

## 验证

make test 覆盖有效、CRC 损坏、截断文件，FoE/文件流相互拒绝抢占、验签失败的一致性，以及文件流驱动异步存储、取消和接收缓冲使用。make test-upgrade-build 验证默认关闭文件入口、显式启用且关闭 FoE 时的实际 archive 与链接结果。
