# 测试与验证

将证据分为三类：本仓库宿主自动化测试、真实第三方协议栈集成测试，以及目标 MCU/ESC/主站的现场验证。通过前一类不能推导后一类已通过。

## 仓库宿主测试

从仓库根目录执行：

```sh
make test
```

默认使用 `clang`，可通过 `CC` 覆盖。该目标编译并运行主机示例、EFW 包工具、核心与驱动 Mock、SSC 适配 Mock、异步核心及相关校验；还需要 Python 3 和 `pdftotext`。详细目标和依赖以 [`Makefile`](../Makefile) 为准。需要单独运行某组异步用例时，可用 `make test-core-async`、`make test-target-async` 或 `make test-ssc-async`。

宿主用例验证软件契约和模拟故障路径，不测量目标 Flash 擦写时间、总线挂起、掉电原子性、真实 SSC 交互或真实主站行为。

## SOES 集成测试

SOES 源码不随仓库分发。需要先获取项目使用的固定版本，并按 [`examples/bootloader_soes/README.md`](../examples/bootloader_soes/README.md) 选择其要求的文件目录，再运行：

```sh
make test-soes SOES_DIR=/path/to/soes-test-files
make test-bootloader SOES_DIR=/path/to/soes-test-files
```

目标会检查版本哈希并使用仓库中的宿主端口桩运行；哈希检查失败时应确认所取版本，不要直接对滚动分支忽略校验。该测试仍是宿主环境中的固定源码 Harness，不代表物理 ESC、完整板级端口或 TwinCAT 测试。

## SSC 与密码学后端

SSC 接口的 Mock 契约测试包含在宿主目标中，但仓库没有 Beckhoff SSC 源码，因此没有真实 SSC 构建或设备验证。可选 Monocypher 互操作测试需要仓库外的 Monocypher 4.0.3 源码目录：

```sh
make test-monocypher MONOCYPHER_DIR=/path/to/monocypher/src
```

测试具体依赖以 Makefile 检查为准。密码学库互操作测试不能替代设备密钥存储、目标板性能和启动策略验证。

## 产品硬件验证

使用具体 MCU、ESC、协议栈和主站复核分区边界、真实包分段、传输中止、擦写失败、回读校验、验签、防回滚、最终结果报告、掉电恢复、看门狗及启动交接。记录硬件修订、编译器、固件版本、协议栈版本和主站配置。当前仓库没有宣称完成完整真机升级闭环。
