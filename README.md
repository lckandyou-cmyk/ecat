# ECAT — EtherCAT FoE 固件升级库

中文 | [English](README.en.md)

ECAT 是一个用于 EtherCAT 从站固件升级的可移植 C99 库，面向 32 位 MCU 上的独立 Bootloader。它提供 FoE 升级核心、固件容器解析和存储驱动接口，方便集成者接入自己的协议栈、硬件和分区配置。

> **项目状态：研发阶段。** 核心逻辑、SOES 适配和 Bootloader 示例已进行宿主测试；尚未在真实板卡和主站上完成联调。请在实际硬件上验证后再考虑生产使用。

## 项目提供什么

- 不依赖特定 MCU 的升级状态机和 `.efw` 固件容器解析，核心不使用动态内存分配。
- 项目和硬件标识校验、流式 CRC32 校验与存储回读校验；写入位置和容量由设备本地配置决定。
- SOES 适配层及 [Bootloader 接入示例](examples/bootloader_soes/README.md)，使用外部 SOES 源码和模拟硬件进行宿主集成测试。
- [固件打包工具](tools/efw_pack.py)、宿主测试、[格式规范](docs/spec/efw-format-v0.1.md)和[架构说明](docs/architecture/SAD-v0.1.md)。

本仓库不包含 SOES 或 Beckhoff SSC 源码，需要用户自行获取。SSC 目前只有抽象接口与模拟契约测试，尚未接入真实 SSC。

## 验证状态

| 内容 | 当前状态 |
| --- | --- |
| 升级核心和固件容器 | 已进行宿主单元测试，存储使用模拟驱动 |
| SOES 适配和 Bootloader 示例 | 已使用真实 SOES 源码及硬件端口桩进行宿主测试 |
| SSC | 现有同步适配有抽象契约测试；异步 BUSY/重试门控为阶段性组件，真实协议栈和异步核心尚未验证 |
| MCU 板卡、Flash 和 EtherCAT 主站 | 尚未进行真实硬件联调 |

宿主测试不能证明特定 MCU 的时序、存储可靠性或主站兼容性。SOES 测试使用的源码来源、文件哈希和具体限制见 [SOES 接入分析](docs/architecture/soes-integration-options.md)；已找到四个关键文件与测试基线匹配的上游提交。

## 快速开始

需要 C99 编译器、Python 3、`make` 和用于 PDF 引文检查的 `pdftotext`：

```sh
make test CC=clang
```

可选运行地址和未定义行为检查：

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

仓库自带 `ecat-efw` 命令行入口，需要 Python 3；在仓库根目录执行：

```sh
./ecat-efw pack -i app.bin -p 0x10010001 -n MY_PROJECT -d 1 -t MCU -v 1.0.0.0
./ecat-efw inspect MY_PROJECT_MCU_V1.0.0.0.efw
./ecat-efw --help
```

`-i` 是输入文件，`-p` 是项目 ID，`-d` 是硬件 ID，`-t` 是目标，`-v` 是固件版本，`-o` 是输出文件。也可使用 `--bin`、`--project-id`、`--hardware-id`、`--target`、`--fw-version`、`--out` 等长参数。项目 ID、硬件 ID 和目标须与设备配置匹配。`-n` 指定项目名，必须与从站配置一致；未指定时默认为 `PROJECT`。默认生成符合协议的文件名，若使用 `-o` 自定义名称，FoE 发送文件名仍须符合设备约定。工具将已有的 `.bin` 打包为 `.efw`，不编译固件；默认不覆盖已有输出文件。也可使用 `unpack --extract-bin` 提取载荷。

### Windows 可执行文件

GitHub Actions 在 Windows x64 上构建并测试独立的 `ecat-efw.exe`，无需在目标 Windows 电脑安装 Python。工作流在主分支推送、拉取请求或手动运行时生成构建产物；推送测试版 Git 标签（例如 `v0.1.0-alpha.1`）后，测试通过才会创建标记为预发布的 GitHub Release 并上传 ZIP 和 SHA-256 校验文件。首次可用版本请以实际工作流和 Release 结果为准。

下载 ZIP、校验并解压后，在 PowerShell 中运行 `.\ecat-efw.exe --help`，其 `pack`、`inspect` 和 `unpack` 参数与上面的命令相同。可执行文件目前未签名。Windows 上为避免在缺少原子防符号链接支持时误覆盖文件，`--overwrite` 不可用；请选择不存在的输出路径。

## 移植到 MCU

1. **准备设备工程**：将库加入独立 Bootloader，接入 EtherCAT 从站协议栈，并为目标 MCU 配置编译与链接。
2. **配置设备和分区**：设置项目标识、硬件标识、目标存储基地址及容量。存储边界必须来自设备本地配置。
3. **实现存储驱动**：根据目标介质实现初始化、失效标记、擦除、写入、回读、提交和异常清理。确认擦写粒度、地址边界与断电恢复行为。
4. **接入协议栈**：使用 SOES 时，参考示例接入前后置钩子，并确保协议处理和钩子在同一执行流中串行运行。其他协议栈需要自行实现适配。
5. **打包并验证**：使用固件打包工具生成 `.efw` 文件，在真实板卡和主站上验证正常升级、错误处理、断电恢复及启动行为。

具体接口、板级配置和示例步骤见 [Bootloader 移植示例](examples/bootloader_soes/README.md)；固件格式见 [EFW 格式规范](docs/spec/efw-format-v0.1.md)。示例中的配置和驱动需要按实际硬件修改，不能直接当作已验证的产品固件。

## 重要限制

- 在所测试的 SOES 源码中，零长度结束包路径可能忽略关闭回调的错误。主站收到最终 ACK 不能单独证明固件已成功提交；集成方需要提供并验证独立的升级结果状态。
- SOES 原生文件名匹配存在短前缀和空文件名问题；集成时应检查适配方案和配置，详见 [SOES 接入分析](docs/architecture/soes-integration-options.md)。无需读取功能时，`FOE_READ_SUPPORTED` 必须保持未定义，而非定义为 0。
- CRC32 只能检测偶发损坏，不能验证固件来源或防止恶意篡改。项目标识和硬件标识也不是身份认证；有安全需求的产品需自行设计签名校验和信任链。
- 断电后能否安全恢复取决于存储驱动、有效标记及 Bootloader 启动逻辑；Flash 擦写耗时、看门狗和主站超时需要在目标设备上验证。
- SSC 异步 BUSY/重试门控目前只是单包静态缓冲和回调契约组件，尚未接入核心升级状态机及非阻塞 Flash 驱动，不可视为可用的完整异步升级路径；范围见[阶段说明](docs/architecture/ssc-async-stage1.md)。

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
