# ECAT

<p align="center"><strong>嵌入式传输与固件升级框架 · C99 · 面向 32 位 MCU</strong></p>

<p align="center">
  <a href="https://github.com/lckandyou-cmyk/ecat/actions/workflows/windows-cli.yml"><img src="https://github.com/lckandyou-cmyk/ecat/actions/workflows/windows-cli.yml/badge.svg?branch=main" alt="Windows CLI workflow"></a>
  <img src="https://img.shields.io/badge/language-C99-blue" alt="C99">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License"></a>
</p>

<p align="center">
  <a href="#快速体验">快速体验</a> ·
  <a href="#移植到-mcu">移植指南</a> ·
  <a href="#验证状态">验证状态</a> ·
  <a href="#反馈问题">反馈问题</a>
</p>

> **项目状态。** 统一传输管理、单一目标注册表（`1..65535`）、同步/异步双向流、`.efw` 校验与 Ed25519ph 验签、固定上游 SOES 基线及 SSC 同步/异步契约桩均已通过宿主机全量测试；目标 32 位 MCU Flash 掉电恢复、商业 Beckhoff SSC 官方源码栈与 EtherCAT 主站现场联调仍须在产品硬件上实测确认。本文档严格区分宿主已验证范围与待实机验证范围。

ECAT 提供统一的传输管理器、目标注册与访问控制、双向数据传输，以及基于 `.efw` 的固件升级能力。FoE 是默认内置入口；集成者也可以关闭内置入口并接入自己的通信方式。设备协议栈、板级存储驱动与本地分区配置由集成者提供。

## 升级流程

```mermaid
flowchart TD
    BIN["应用固件映像 .bin"] --> CLI["ecat-efw 打包"]
    CLI --> EFW[".efw 固件包"]
    EFW --> FOE["FoE / 文件流 / 自定义通道"]
    DATA["普通数据导入 / 导出"] --> CHANNEL["自定义通道"]
    FOE --> MANAGER["统一传输管理器"]
    CHANNEL --> MANAGER
    REGISTRY["本地目标注册与访问控制"] --> MANAGER
    MANAGER --> CORE["固件校验与升级核心"]
    MANAGER --> DATA_DRIVER["普通数据目标驱动"]
    CORE --> FW_DRIVER["固件目标驱动"]
    PARTITION["设备本地分区配置"] --> FW_DRIVER
```

固件包携带项目、硬件、目标和版本信息；**实际写入地址与容量只由设备本地配置决定**。ECAT 不包含 EtherCAT 协议栈，也不替代 MCU 厂商的 Flash 驱动或 Bootloader 启动逻辑。

## 快速体验

### 打包并检查固件

在仓库根目录使用 Python 3 运行 `ecat-efw`：

```sh
./ecat-efw pack \
  -i app.bin -n MY_PROJECT \
  -p 0x10010001 -d 1 -t MCU -v 1.0.0.0

./ecat-efw inspect MY_PROJECT_MCU_V1.0.0.0.efw
./ecat-efw info MY_PROJECT_MCU_V1.0.0.0.efw
./ecat-efw --version
```

`pack` 将已有 `.bin` 封装为 `.efw`，不会编译固件；`-t` 支持 `1..4`（`MCU`/`FPGA`/`SII`/`EEPROM`）与 `5..65535`（唯一大写四位十六进制 `T0005`～`TFFFF` 或对应正整数）。`inspect` 输出详细校验信息，`info` 简洁显示包内版本、项目/硬件 ID、升级目标、签名状态、Key ID、固件大小和 CRC 结果。带 `--public-key` 时还会验证签名；否则只报告签名存在但未验证。命令按文件内容识别，不依赖扩展名。EFW 格式未记录打包日期；`--version` 显示 CLI 版本和构建日期。包校验不等同于目标设备兼容性或可启动性验证。默认拒绝覆盖已有输出。运行 `./ecat-efw --help` 查看全部命令和参数。

### 可选签名

默认仍生成未签名包。需要签名时，在可信打包环境准备 32 字节 Ed25519 私钥种子文件，并显式传入 `--signing-key`；签名/验签需要 PyNaCl 与 libsodium。CLI 不会把私钥写进 EFW，`--public-key-out` 可导出设备信任配置所需公钥。检查已签名包时，`info` 不带公钥只会显示“存在，未验证”；带 `--public-key` 才验证签名。Windows 发布 CLI 包含签名运行依赖。设备端验签接口默认关闭；启用时必须接入受保护公钥和验签后端，否则初始化失败，不会回退到未验证模式。

```sh
./ecat-efw pack -i app.bin -p 0x10010001 -d 1 -t MCU -v 1.2.3.4 \
  --signing-key signing-seed.bin --public-key-out trusted-key.bin -o app.efw
./ecat-efw info app.efw --public-key trusted-key.bin
```

不要提交、分发或记录私钥。签名功能默认关闭；设备要强制验签，必须在本地启用 `ECAT_FOE_SIGNATURE_VERIFY_ENABLE` 并接入受保护公钥及密码学后端。验签失败会拒绝升级，不会降级到未验证模式。当前解析器仅接受统一格式；旧格式固件需要重新打包。旧设备可能忽略未知扩展，不能靠签名包强制旧设备验签。

### Windows

项目提供 Windows x64 独立 CLI。可在 [GitHub Actions](https://github.com/lckandyou-cmyk/ecat/actions/workflows/windows-cli.yml) 下载工作流产物；通过测试的 `v0.1.0-alpha.N` 等测试版标签会生成 [预发布 Release](https://github.com/lckandyou-cmyk/ecat/releases)。目标电脑无需安装 Python。

解压后在 PowerShell 中运行：

```powershell
.\ecat-efw.exe --help
```

Windows 可执行文件目前未签名，且不支持 `--overwrite`；请指定尚不存在的输出路径。

## 移植到 MCU

1. **接入工程与协议栈。** 将核心加入独立 Bootloader，并实现所用 EtherCAT 从站协议栈的适配。仓库包含 SOES 接入示例；SOES/SSC 第三方源码需自行获取。
2. **配置设备身份与分区。** 设置项目 ID、硬件 ID、目标类型、存储基地址和容量。分区边界必须来自设备本地配置，不能由收到的固件包指定。
3. **实现目标存储驱动。** 按介质实现初始化、失效标记、擦除、写入、回读校验、提交和失败清理，并验证擦写粒度与地址边界。
4. **在目标设备验证。** 使用真实 MCU 和主站验证升级成功与失败路径、断电恢复、启动交接、看门狗和超时行为。

从 [SOES Bootloader 示例](examples/bootloader_soes/README.md) 开始查看接口、配置和移植步骤。示例驱动用于展示接口，不是可直接用于产品的板级实现。

需要调整核心静态缓冲区时，将 `include/ecat_foe_conf_template.h` 复制为工程自己的 `ecat_foe_conf.h` 并加入包含路径；所有使用核心头文件的编译单元以及核心本身都要使用相同配置。Shell 中可用单引号保留宏值里的双引号，例如：

```sh
cc -Iinclude -I/path/to/config '-DECAT_FOE_CONFIG_HEADER="ecat_foe_conf.h"' -c src/ecat_foe_core.c
```

同样的选项必须用于应用和适配器的编译。`ECAT_FOE_HEADER_BUF_MAX` 和 `ECAT_FOE_MAX_WRITE_UNIT` 支持预处理整数常量表达式（例如 `(144U + 4U)`、`(1U << 8)`），各编译单元求值后的数值必须一致；括号、进制和算术写法不同不会造成假错配。若不一致，链接器会报告未定义的 `ecat_foe_config_abi_...` 符号；请重新使用一致配置编译所有对象。此检查要求编译器支持 `used` 和 `retain` 属性，使用核心公开头的目标也须链接核心对象。

本版本统一以 `ecat_target_registry_t` 承载全部固件与数据目标，并同步升级了核心配置与注册表的 ABI 标识（`ecat_foe_config_abi_v3_...`、`ecat_target_registry_abi_v2_...`）。升级时必须重新编译使用公开头的所有对象和库；旧对象混用时会在链接阶段报未定义的 ABI 符号。

## 可选升级入口

`ecat_transfer_t` 提供统一的会话、导入、导出和异步轮询接口。FoE 默认启用，SOES/SSC 必须绑定该管理器；可选宿主文件流入口默认关闭。不同入口不能抢占或取消其他所有者的会话，固件导入共用 EFW 校验、验签与存储驱动。

```sh
make build-upgrade
make build-upgrade ENABLE_FOE=0 ENABLE_FILE=1 UPGRADE_BUILD_DIR=build/file-upgrade
make build-upgrade ENABLE_FOE=0 ENABLE_FILE=0 UPGRADE_BUILD_DIR=build/custom-only
```

默认静态库选择 SSC 适配器；SOES 需显式选择并提供协议栈/BSP 头。以上构建仍要求 POSIX Make 工具链。文件流是架构参考，UART/CAN/USB/网络协议尚未实现。接口、构建矩阵及接入顺序见[统一传输架构](docs/architecture/unified-transfer-architecture.md)和[文件流接入](examples/file_stream/README.md)。目标注册、可选通道与双向导入/导出接口见[多通道与目标区块指南](docs/multichannel-bidirectional-framework.md)与[可注册固件目标设计](docs/architecture/custom-firmware-targets.md)。固件与数据目标统一由 `ecat_target_registry_t` 管理，支持 `1..65535` 非零目标 ID（`1..4` 规范名为 `MCU`/`FPGA`/`SII`/`EEPROM`，`5..65535` 规范名为 `T0005`～`TFFFF`），注册槽位数受 `ECAT_TARGET_REGISTRY_MAX_BLOCKS` 限制；ACL 和签名验证须按设备配置启用。

此次架构收敛已移除旧升级管理器、兼容入口与双套四元目标数组，使用旧接口的工程须迁移至 `ecat_transfer_t` + `ecat_target_registry_t` 并全量重新编译。固件核心的内部事件不再作为应用传输入口。

## 验证状态

| 范围 | 已验证（宿主机自动化） | 尚未验证（待目标实机） |
| --- | --- | --- |
| 统一传输与单一目标注册表 | 同步/异步双向导入导出、`1..65535` 稀疏目标、ACL 权限、独占会话租约与 ABI 防混链测试 | 目标 32 位 MCU 外设中断/DMA 驱动与跨实例物理存储互斥 |
| 固件升级核心、`.efw` 与验签 | 同步/异步全路径模拟存储驱动测试；Monocypher 4.0.3 Ed25519ph 真实签名互操作测试 | 目标 MCU Flash 擦写时序、安全启动、防回滚掉电事务与断电恢复 |
| SOES 接入 | 固定上游四文件源码基线与宿主硬件端口桩集成测试 | 完整协议栈部署、真实 ESC 从站芯片与主站现场联调 |
| Beckhoff SSC 接入 | 同步适配器、单包 BUSY/重试门控及异步固件升级全链路宿主契约桩测试 | 商业 Beckhoff SSC 官方源码接入与 TwinCAT 实机时序联调 |
| `ecat-efw` CLI 工具 | macOS/Linux Python 全套回归及 Windows x64 独立可执行包测试 | 可执行文件代码签名与更多目标主机环境覆盖 |

宿主测试通过不等于硬件或商业协议栈实机验证。SOES 测试基线、文件哈希和限制见 [SOES 接入分析](docs/architecture/soes-integration-options.md)。

## 安全边界

- `.efw` 中的 CRC32 用于检测意外损坏，不提供固件来源认证。项目 ID 和硬件 ID 也不是认证凭据。当前 EFW v2 提供默认关闭的 Ed25519ph 签名选项，准确覆盖范围见[格式规范](docs/spec/efw-format-v0.2.md)。
- 设备端验签只在本地显式启用并配置受信公钥及密码学后端时强制执行；签名错误、未签名包、算法或密钥不可用均拒绝，不降级。验签与防回滚独立，是否接受旧版本由本地防回滚策略决定。签名不能替代安全启动和镜像结构检查；单分区升级在最终验签前可能已擦除旧镜像，验签本身不保证断电恢复或设备可用。
- 启用防回滚时，目标驱动必须实现 `finalize_with_version_floor`，以断电可恢复事务联合提交镜像激活和版本下限；核心会拒绝缺少此回调的配置。宿主模拟不能证明具体板级事务的掉电安全。
- SSC 适配器会校验 FoE 访问密码。默认预期值为 `0`；如使用非零密码，须在接受 WRQ 前配置。FoE 密码不等同于密码学认证。
- 已测试的 SOES 版本在零长度结束包路径上可能忽略关闭回调错误，因此主站最终 ACK 不能单独作为镜像提交成功的证据。集成端应提供独立的升级结果状态。
- SOES 原生文件名匹配存在短前缀和空文件名边界问题。若不需要 FoE 读取功能，应保持 `FOE_READ_SUPPORTED` 未定义，而不是定义为 `0`。具体限制见 [SOES 接入分析](docs/architecture/soes-integration-options.md)。

## 规范与反馈

- [ECAT Wiki：快速开始、架构、移植与故障排查](https://github.com/lckandyou-cmyk/ecat/wiki)
- [EFW v2 固件包格式与签名规范](docs/spec/efw-format-v0.2.md)
- [多通道双向传输与目标区块指南](docs/multichannel-bidirectional-framework.md)
- [统一传输架构说明](docs/architecture/unified-transfer-architecture.md)
- [可注册固件目标设计](docs/architecture/custom-firmware-targets.md)
- [可选 Monocypher 设备端验签适配器](examples/crypto/README.md)
- [SOES 接入分析](docs/architecture/soes-integration-options.md)
- 历史与阶段记录：[旧 EFW v1 格式说明](docs/spec/efw-format-v0.1.md) · [传输解耦迁移记录](docs/architecture/transport-independent-upgrade.md) · [SSC 异步第一阶段说明](docs/architecture/ssc-async-stage1.md)

请通过 [Bug 报告模板](.github/ISSUE_TEMPLATE/bug_report.md)或[功能需求模板](.github/ISSUE_TEMPLATE/feature_request.md)提交反馈，并注明仓库版本、MCU/板卡、协议栈版本、主站、编译器和复现步骤。请勿公开密码、密钥、凭据或专有固件。

## 许可证

本项目采用 [MIT 许可证](LICENSE)。第三方协议栈须遵循其各自的许可条款。
