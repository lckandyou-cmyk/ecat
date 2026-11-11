# 快速开始

## 适用范围

ECAT 提供 C99 FoE 升级核心、EFW 包解析器和目标存储驱动接口。你需要自行提供 EtherCAT 从站协议栈、板级存储驱动、设备分区配置和 Bootloader 启动逻辑。仓库中的 SOES 与 SSC 材料是接入参考及宿主验证，不代表真实板卡已完成联调。

## 获取仓库并检查固件包

CLI 脚本 `ecat-efw` 使用 Python 3。签名和验签额外需要 PyNaCl/libsodium；未签名打包不需要该可选依赖。从仓库根目录开始：

```sh
./ecat-efw --version
./ecat-efw --help
```

准备已有的应用二进制映像 `app.bin` 后，将它封装为 `.efw`：

```sh
./ecat-efw pack \
  -i app.bin -n MY_PROJECT \
  -p 0x10010001 -d 1 -t MCU -v 1.0.0.0 \
  -o app.efw
```

`pack` 不编译固件。`-p` 是项目 ID，`-d` 是硬件 ID，`-t` 是目标类型，`-v` 是四段版本号；这些值应与设备本地兼容策略一致。输出路径默认不能覆盖已有文件。完整参数以 `./ecat-efw pack --help` 为准。

检查生成的包：

```sh
./ecat-efw inspect app.efw
./ecat-efw info app.efw
```

`inspect` 输出详细格式校验，`info` 显示主要元数据和 CRC 结果。CRC 用于检测意外损坏，不认证固件来源；检查包也不能证明目标设备兼容或映像可启动。格式字段和签名覆盖范围见 [EFW v2 格式规范](../docs/spec/efw-format-v0.2.md)。

## 从示例开始集成

建议按以下顺序阅读：

1. [架构概览](Architecture.md)，了解核心、适配层和驱动的边界。
2. [MCU 移植指南](Porting-Guide.md)，按本地身份、分区和驱动契约准备工程。
3. [协议栈接入](Stack-Integration.md)，选择 SOES 或 SSC 的接入路径。
4. [测试与验证](Testing.md)，区分宿主模拟与目标硬件验证。

SOES 示例和驱动接口详见 [`examples/bootloader_soes/README.md`](../examples/bootloader_soes/README.md)。示例中的地址和参数是参考配置，不能直接用于其他硬件。不要把 EFW 内部或 FoE 文件名中的远程信息用作物理写入地址；写入范围必须由设备本地配置决定。

## Windows CLI

仓库提供 Windows x64 独立可执行文件工作流。可从 [Windows CLI Actions](https://github.com/lckandyou-cmyk/ecat/actions/workflows/windows-cli.yml) 下载工作流产物；通过工作流的测试版标签可获取 [预发布 Release](https://github.com/lckandyou-cmyk/ecat/releases)。解压后在 PowerShell 运行 `.\ecat-efw.exe --help`。当前可执行文件未签名，且不支持 `--overwrite`。
