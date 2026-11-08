# ECAT：EtherCAT FoE 固件升级库

中文 | [English](README.en.md)

ECAT 是面向 32 位 MCU 独立 Bootloader 的 EtherCAT FoE 固件升级库，采用 C99 编写。项目提供升级状态机、`.efw` 固件容器解析与校验，以及目标存储驱动接口，供集成者接入自己的 EtherCAT 从站协议栈、硬件平台和本地分区配置。

> **项目状态：研发中，尚未完成产品级验证。** 核心逻辑、SOES 接入路径和 Bootloader 示例已通过所列宿主测试；尚未在真实 MCU 板卡与 EtherCAT 主站上完成联调。将项目用于产品前，必须针对目标硬件、协议栈和启动流程完成验证。

## 项目范围

- **升级核心：** 与特定 MCU 和协议栈解耦的 C99 状态机；核心不使用动态内存分配。
- **固件校验：** 校验项目标识、硬件标识、目标类型、版本和数据完整性，并支持写入后回读校验。写入地址和容量仅由设备端本地配置决定。
- **协议栈接入：** 提供 SOES 适配代码和 [Bootloader 接入示例](examples/bootloader_soes/README.md)。协议栈源码须由集成者自行获取。
- **工具与文档：** 提供 [固件打包工具](tools/efw_pack.py)、宿主测试、[固件格式规范](docs/spec/efw-format-v0.1.md)及[架构说明](docs/architecture/SAD-v0.1.md)。

本仓库不分发 SOES 或 Beckhoff SSC 的第三方源码。SSC 同步适配器目前通过 Mock 契约测试；异步回调门控尚未接入核心升级状态机和非阻塞存储驱动。两者均未在真实 SSC 上验证。

## 验证状态

| 内容 | 当前状态 |
| --- | --- |
| 升级核心与 `.efw` 容器 | 宿主单元测试通过；存储使用模拟驱动 |
| SOES 适配与 Bootloader 示例 | 使用固定版本的 SOES 源文件及硬件端口桩通过宿主测试 |
| Beckhoff SSC | 同步适配器 Mock 契约测试及异步门控测试通过；真实 SSC 集成和异步核心路径未验证 |
| MCU 板卡、Flash 和 EtherCAT 主站 | 尚未进行真实硬件联调 |

宿主测试不能证明目标 MCU 的时序、存储可靠性、断电恢复能力或主站兼容性。SOES 测试的源码基线、文件哈希和限制见 [SOES 接入分析](docs/architecture/soes-integration-options.md)。测试使用四个上游源码文件及本项目的宿主配置和硬件端口桩，不等同于真实协议栈或硬件联调。

## 宿主开发与测试

需要 C99 编译器、Python 3、`make` 和用于 PDF 引文检查的 `pdftotext`：

```sh
make test CC=clang
```

可选运行 AddressSanitizer 与 UndefinedBehaviorSanitizer 检查：

```sh
make test-asan CC=clang
```

要运行 SOES 宿主集成测试，请自行获取 [SOES](https://github.com/OpenEtherCATsociety/SOES)，检出固定版本，并将四个测试文件复制到独立目录：

```sh
git clone https://github.com/OpenEtherCATsociety/SOES.git /path/to/SOES
git -C /path/to/SOES checkout --detach 6ef7b9415e936c061ad30626ca00004bdde2ac62
soes_test_dir=$(mktemp -d)
cp /path/to/SOES/soes/esc.c /path/to/SOES/soes/esc.h \
  /path/to/SOES/soes/esc_foe.c /path/to/SOES/soes/esc_foe.h "$soes_test_dir/"
make test-soes SOES_DIR="$soes_test_dir"
make test-bootloader SOES_DIR="$soes_test_dir"
```

SOES 版本会影响结果；请按[示例文档中的固定提交和源码哈希](examples/bootloader_soes/README.md)检出并校验。测试使用这四个上游文件及本仓库的宿主配置和端口桩；完整 SOES 目录的其他头文件会覆盖测试桩，不能直接用于此测试。测试结束后可自行删除临时目录。这不是完整协议栈集成或上板验证。

## 固件打包 CLI

仓库提供 `ecat-efw` 命令行工具，用于打包和检查 `.efw` 文件。源码入口需要 Python 3；在仓库根目录执行：

```sh
./ecat-efw pack -i app.bin -p 0x10010001 -n MY_PROJECT -d 1 -t MCU -v 1.0.0.0
./ecat-efw inspect MY_PROJECT_MCU_V1.0.0.0.efw
./ecat-efw --help
```

`-i`、`-p`、`-d`、`-t` 和 `-v` 分别指定输入文件、项目 ID、硬件 ID、升级目标和固件版本；`-o` 用于指定输出路径。工具也支持 `--bin`、`--project-id`、`--hardware-id`、`--target`、`--fw-version` 和 `--out` 等长参数。相关标识和目标必须与设备端配置一致。`-n` 指定项目名，须与从站配置一致；默认值为 `PROJECT`。工具只负责将现有 `.bin` 打包为 `.efw`，不负责编译固件，且默认拒绝覆盖已有输出。自定义输出路径时，FoE 传输使用的文件名仍须符合设备端约定。可通过 `unpack --extract-bin` 提取固件载荷。

### Windows 可执行文件

GitHub Actions 会在 Windows x64 环境构建并测试独立的 `ecat-efw.exe`，运行该可执行文件的目标电脑无需安装 Python。主分支推送、拉取请求和手动触发会生成工作流构建产物。推送测试版 Git 标签（例如 `v0.1.0-alpha.1`）后，工作流仅在测试通过时创建预发布版 Release，并上传 ZIP 和 SHA-256 校验文件。请以 GitHub Actions 和 Release 页面上的实际状态为准。

下载并校验 ZIP 后解压，在 PowerShell 中运行 `.\ecat-efw.exe --help`。其 `pack`、`inspect` 和 `unpack` 子命令与上文相同。该可执行文件目前未签名。为避免无法可靠实施防符号链接覆盖保护，Windows 版本不支持 `--overwrite`；请指定尚不存在的输出路径。

## 移植到 MCU

1. **准备设备工程：** 将库集成到独立 Bootloader，接入 EtherCAT 从站协议栈，并配置目标 MCU 的编译和链接。
2. **配置设备与分区：** 设置项目标识、硬件标识、升级目标、存储基地址和容量。地址与容量边界必须由设备端本地配置提供，固件包不得指定写入地址。
3. **实现存储驱动：** 按目标介质实现初始化、镜像失效标记、擦除、写入、回读校验、提交和失败清理；验证擦写粒度、地址边界和断电恢复行为。
4. **接入协议栈：** 使用 SOES 时按示例集成前置和后置处理钩子，并保证协议处理、异步门控及驱动操作满足对应的执行上下文和串行化要求。其他协议栈需实现并验证独立适配层。
5. **打包与验证：** 使用 CLI 将固件映像打包为 `.efw`，在目标板卡和 EtherCAT 主站上验证成功升级、失败处理、断电恢复及启动行为。

接口定义、板级配置和接入步骤见 [Bootloader 移植示例](examples/bootloader_soes/README.md)；容器字段和校验规则见 [EFW 格式规范](docs/spec/efw-format-v0.1.md)。示例配置与驱动仅用于说明接入方式，必须针对实际硬件实现和验证。

## 重要限制

- 在已测试的 SOES 源码中，零长度结束包路径可能忽略关闭回调错误。因此，主站收到最终 ACK 不能单独证明镜像已成功提交；集成方应提供并验证独立的升级结果状态。
- SOES 原生文件名匹配存在短前缀及空文件名边界问题。请按 [SOES 接入分析](docs/architecture/soes-integration-options.md)检查适配方案和配置。若设备不需要读取功能，应保持 `FOE_READ_SUPPORTED` 未定义；不要将其定义为 `0`。
- CRC32 仅用于检测偶发损坏，不能验证固件来源或防止恶意篡改；项目标识和硬件标识也不构成身份认证。具有安全要求的产品必须自行实现签名验证和信任链。
- 断电恢复能力取决于存储驱动、镜像有效标记和 Bootloader 启动逻辑。Flash 擦写时序、看门狗行为和主站超时必须在目标设备上验证。
- SSC 适配器可校验 FoE 访问密码；默认预期密码为 `0`，使用非零密码时必须在接受 WRQ 前配置。FoE 密码属于协议访问控制，不是密码学认证，也不能替代固件签名与信任链。
- SSC 异步回调目前提供 `bDataFollowing` 包装和 BUSY/重试门控，但 DATA 处理尚未接入核心升级状态机或非阻塞 Flash 驱动，不构成完整的异步升级路径。详见[阶段说明](docs/architecture/ssc-async-stage1.md)。

## 目录

| 路径 | 内容 |
| --- | --- |
| `include/`、`src/` | 核心接口、升级逻辑和协议栈适配 |
| `examples/bootloader_soes/` | SOES Bootloader 接入参考与宿主集成测试 |
| `tests/` | 单元测试、模拟驱动和协议栈测试 |
| `tools/` | 固件打包及辅助检查工具 |
| `docs/` | 架构、固件格式和接入分析 |

## 反馈问题

欢迎通过 [Bug 报告模板](.github/ISSUE_TEMPLATE/bug_report.md)或[功能需求模板](.github/ISSUE_TEMPLATE/feature_request.md)提交问题。请尽可能提供仓库版本、MCU 和板卡型号、协议栈来源及版本、主站与编译器版本、复现步骤和脱敏日志。请勿公开密码、密钥、凭据或专有固件。

## 许可证

本项目采用 [MIT 许可证](LICENSE)。第三方协议栈的使用与许可请遵循其各自的规定。
