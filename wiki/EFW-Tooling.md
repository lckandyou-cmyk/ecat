# EFW 打包与签名工具

`ecat-efw` 将已有应用二进制映像封装为 EFW 包。它不调用编译器，也不会替你确认该映像适用于目标设备。

## 打包与检查

未签名包是默认模式：

```sh
./ecat-efw pack \
  -i app.bin -n MY_PROJECT \
  -p 0x10010001 -d 1 -t MCU -v 1.2.3.4 \
  -o app.efw

./ecat-efw inspect app.efw
./ecat-efw info app.efw
```

命令格式以 `./ecat-efw --help` 和 `./ecat-efw pack --help` 为准。包名、项目 ID、硬件 ID、目标类型和四段版本应与接收设备本地配置匹配。CLI 默认拒绝覆盖现有输出。`inspect` 提供详细结构检查，`info` 适合快速查看元数据和 CRC；没有公钥时，签名只会被显示为存在，不能据此判断签名有效。

EFW v2 字段布局、版本规则和签名覆盖内容以[格式规范](../docs/spec/efw-format-v0.2.md)为准。历史 EFW v1 文档保留在 [`docs/spec/efw-format-v0.1.md`](../docs/spec/efw-format-v0.1.md)，当前解析器只接受统一格式；旧包应按当前规范重新打包。

## 可选 Ed25519ph 签名

签名需要可信发布环境中的 32 字节私钥种子文件，以及可选 PyNaCl/libsodium 依赖：

```sh
./ecat-efw pack -i app.bin -p 0x10010001 -d 1 -t MCU -v 1.2.3.4 \
  --signing-key signing-seed.bin \
  --public-key-out trusted-key.bin \
  -o app-signed.efw

./ecat-efw info app-signed.efw --public-key trusted-key.bin
```

将导出的公钥按产品流程安全配置到设备。设备端还必须在本地启用验签选项并接入受维护的密码学后端；否则签名包不会因此自动获得认证。启用后，缺少公钥、后端或有效签名时应拒绝升级。验签独立于防回滚、启动映像检查和断电恢复。不要提交、分发或记录私钥。

旧设备可能忽略未知签名扩展，因此不能靠发布签名包强制旧解析器验签。升级策略应纳入设备软件版本兼容矩阵。

## Windows CLI

仓库的 Windows x64 工作流会构建独立 CLI 包；可在 [GitHub Actions 工作流](https://github.com/lckandyou-cmyk/ecat/actions/workflows/windows-cli.yml)下载产物，通过测试的预发布标签会生成 [GitHub Release](https://github.com/lckandyou-cmyk/ecat/releases)。目标电脑不需要安装 Python。解压后在 PowerShell 执行：

```powershell
.\ecat-efw.exe --help
```

当前发布的可执行文件未签名，且不支持 `--overwrite`；使用不存在的输出路径。打包物可被检查，不代表已通过目标设备兼容性或启动验证。
