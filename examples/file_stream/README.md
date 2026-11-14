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

1. 按本地设备身份和分区配置初始化核心、目标驱动及唯一的 ecat_upgrade_t。异步目标须先初始化异步引擎，再把其通用后端接到管理器。
2. 用 ecat_upgrade_file_init 建立文件入口并提供固定接收缓冲。异步模式下该缓冲容量不得超过引擎 packet buffer 容量。FILE 流用二进制读取方式打开，并定位在 EFW 头部。
3. 调用 ecat_upgrade_file_begin，传入目标与候选版本。返回 BUSY 时不会读取文件，也不会改变现有升级。元数据必须与 EFW 头一致；不能传入物理存储地址。
4. 每次应用调度调用一次 ecat_upgrade_file_step。返回 PENDING 时继续调度；等待存储操作期间只轮询、不读取下一块。COMPLETE 表示回读、启用时的验签以及提交均已成功。FAILED 时检查管理器诊断；io_error 单独表示读取文件失败。
5. 超时或用户取消时调用 ecat_upgrade_file_cancel。无论成功、失败还是取消，文件入口都不关闭或回绕 FILE，调用方负责 fclose。入口和缓冲必须活到操作终止，且不可移动或复制活跃入口对象。

与 FoE 共用同一个管理器；SOES/SSC 必须 bind_upgrade 后才能参与统一所有权控制。不要混用未绑定的旧事件 API 操作同一核心。

fread 可能阻塞，此入口仅作宿主参考；不能直接放进 ISR 或声称具有 MCU 实时时序保证。掉电安全与启动检查仍由目标驱动和 Bootloader 实现。

## 验证

make test 覆盖有效、CRC 损坏、截断文件，FoE/文件流相互拒绝抢占、验签失败的一致性，以及文件流驱动异步存储、取消和接收缓冲使用。make test-upgrade-build 验证默认关闭文件入口、显式启用且关闭 FoE 时的实际 archive 与链接结果。
