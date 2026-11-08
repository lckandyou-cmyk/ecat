# EFW 固件容器格式与可选签名规范 v0.2

本文定义当前 EFW 容器格式：`format_version = 2`，固定 148 字节头部。签名扩展始终占用头部空间；`algorithm = 0` 表示未签名，`algorithm = 1` 表示 Ed25519ph。默认打包生成 v2 未签名包，设备默认也不强制验签。当前实现仅接受本格式；历史格式不再支持，须重新打包。

> 旧版或第三方解析器可能忽略未知字段；给包添加签名不能强制旧设备执行验签，也不能把传输到旧设备的签名包视为受到保护。

## 固定头部布局

所有整数为无符号小端序。文件必须恰为 `148 + payload_length` 字节；不允许尾随数据。

| 偏移 | 长度 | 字段 | 规则 |
| ---: | ---: | --- | --- |
| `0x00` | 4 | `magic` | ASCII `ECFW` |
| `0x04` | 2 | `format_version` | 固定 `2` |
| `0x06` | 2 | `header_length` | 固定 `148` (`0x0094`) |
| `0x08` | 4 | `project_id` | 与设备本地项目 ID 相符 |
| `0x0C` | 4 | `hardware_id` | 与设备本地硬件 ID 相符 |
| `0x10` | 2 | `hw_ver_min` | 支持的最低硬件版本 |
| `0x12` | 2 | `hw_ver_max` | 支持的最高硬件版本 |
| `0x14` | 2 | `target_id` | MCU=1、FPGA=2、SII=3、EEPROM=4 |
| `0x16` | 2 | `flags` | 当前必须为零 |
| `0x18` | 4 | `fw_version` | Major/Minor/Patch/Build，各占 8 位 |
| `0x1C` | 4 | `min_allow_version` | 防回滚下限元数据；开关独立配置 |
| `0x20` | 4 | `payload_length` | 包内有效载荷长度 |
| `0x24` | 4 | `payload_crc32` | IEEE CRC-32，覆盖完整 payload |
| `0x28` | 20 | `reserved` | 必须全零 |
| `0x3C` | 4 | `extension_magic` | 固定 ASCII `SIG2` |
| `0x40` | 2 | `extension_version` | 固定 `1` |
| `0x42` | 2 | `algorithm` | `0` 未签名；`1` Ed25519ph |
| `0x44` | 2 | `signature_length` | 未签名 `0`；Ed25519ph `64` |
| `0x46` | 2 | `key_id_length` | 未签名 `0`；Ed25519ph `8` |
| `0x48` | 8 | `key_id` | 已签名时为下述 Key ID；未签名全零 |
| `0x50` | 64 | `signature` | Ed25519ph 签名；未签名全零 |
| `0x90` | 4 | `header_crc32` | IEEE CRC-32，覆盖头部字节 `[0, 144)`，包括签名 |
| `0x94` | 可变 | `payload` | 精确覆盖 `payload_length` 字节 |

解析器对固定长度、扩展 magic/version、算法号、长度组合及未签名零填充做严格检查。未知算法、额外头部长度、截断、错误 CRC 或长度不一致均拒绝。

## 签名编码

不自行实现椭圆曲线或哈希算法。算法 1 使用 RFC 8032 Ed25519ph。唯一被签名的消息字节流为以下连续拼接：

```text
ASCII("ECAT-EFW-SIG-V2") || 0x00
|| header[0:80]
|| payload[0:payload_length]
```

第一段共 16 字节；头部切片包含项目/硬件/目标/版本/长度/CRC 元数据及签名扩展和 Key ID，但不包含签名槽本身和头部 CRC。Ed25519ph 按其标准再对整个消息流做 SHA-512 预哈希并使用 RFC 8032 域分离。不得将普通 Ed25519 对 SHA-512 摘要签名当作 Ed25519ph。

仓库固定了跨实现测试包 [`efw-v2-ed25519ph-monocypher.efw`](../../tests/fixtures/efw-v2-ed25519ph-monocypher.efw)。测试私钥种子为 `00 01 … 1f`（仅公开测试密钥，绝不可用于发布）；对应公钥为 `03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8`，Key ID 为 `4ecfdac260da4302`。测试载荷共 257 字节，`payload[i] = (13*i + 7) mod 256`。该包由 Monocypher 4.0.3 生成；有 PyNaCl/libsodium 的主机测试要求 CLI 生成结果与固定包逐字节相同，以覆盖签名实现间的互通。

FoE 空包规避填充若由打包器添加 `0xFF`，填充属于 payload，会计入 `payload_length`、CRC 和签名。尾部没有独立的未覆盖填充区。

Key ID 定义为：

```text
SHA-256(ASCII("ECAT-EFW-KEY-ID-v1") || 0x00 || raw_public_key)[0:8]
```

设备端使用本地可信密钥表将 8 字节 ID 映射到 32 字节公钥。Key ID 只用于查找，不是认证凭据。设备端的密钥表必须由固件构建/设备配置保护；缺失密钥或查找失败一律拒绝。

头部 CRC 不是签名的一部分。修改签名或头部后必须重算 CRC 才能通过结构校验，但仍无法伪造签名。修改 payload CRC/长度/任一安全元数据会改变签名消息并导致验签失败。

## 兼容矩阵

| 设备解析器 | 包 | 结果 |
| --- | --- | --- |
| 当前 v2，验签关闭 | v2 未签名 | 接受，执行结构/CRC/设备元数据校验 |
| 当前 v2，验签关闭 | v2 已签名 | 接受但不验证签名；不得将其报告为已认证 |
| 当前 v2，验签开启 | v2 有效签名且本地公钥可用 | 验证存储回读内容后才允许提交 |
| 当前 v2，验签开启 | 未签名、错误签名、未知算法或密钥不可用 | 拒绝，无降级到未验证模式 |
| 旧设备 / 第三方旧解析器 | v2 | 行为取决于其对版本与未知字段的处理；可能拒绝，也可能忽略签名字段。不能依赖其强制验签 |

验签与防回滚是独立设置。验签只证明包由受信密钥签发且内容完整；它不自动拒绝合法签名的旧版本。只有启用设备本地防回滚并正确配置版本下限/原子提交时，才限制版本回退。

FoE 传输密码独立于固件签名，不替代它；本功能不改变 TwinCAT 或 SOES 的密码协商/校验机制。

## 打包与检查 CLI

默认未签名工作流保持简单：

```sh
ecat-efw pack --bin app.bin --project-id 0x10010001 --hardware-id 1 \
  --target MCU --fw-version 1.2.3.4 --out app.efw
ecat-efw info app.efw
```

签名仅在可信构建环境显式提供 32 字节 Ed25519 seed 文件时启用；支持 32 字节原始值或 64 个 ASCII 十六进制字符：

```sh
ecat-efw pack --bin app.bin --project-id 0x10010001 --hardware-id 1 \
  --target MCU --fw-version 1.2.3.4 --signing-key signing-seed.bin \
  --public-key-out device-trust-key.bin --out app.efw
ecat-efw info app.efw --public-key device-trust-key.bin
```

签名/验签命令需要可选 Python 依赖 PyNaCl（使用 libsodium Ed25519ph）；无签名的 CLI 用法不需要它。Windows 发布包应包含该运行依赖。私钥只在可信打包主机使用：不要放进固件、仓库、Release、日志或普通设备文件系统。CLI 尽力清零可变种子缓冲，但 Python 运行时无法保证清除所有临时副本；高价值密钥应在隔离的签名服务/HSM 中使用。

`info` 不带公钥时只能报告“签名存在，未验证”；带公钥才执行密码学验证。主机端通过不代表设备兼容、固件可启动或对特定设备安全。

## 设备端顺序与失败语义

设备验签默认关闭；开启后配置必须提供完整 begin/update/finish/abort 回调，否则初始化失败。验签开启时拒绝未签名包，未配置/无法查找的公钥和所有算法/签名错误均失败关闭。

核心先解析头部并完成本地项目、硬件、目标及版本策略检查，再解析本地受信密钥；随后才调用存储驱动开始升级。收到数据后核心按有界块读取**实际目标存储中的 payload 回读字节**，将它们与规范域分隔符及头部签名切片一起流式更新验签器。回读 CRC 通过后，验签器必须成功完成，之后才能调用 `finalize()` 或防回滚联合提交。任何失败走现有 abort 清理，不得设置有效标记或切换活动分区。端口实现须保证 `read()` 读取的确是最终准备启动的存储区域。

签名验证不会撤销已经发生的擦除：单分区升级可能在签名最终失败前擦除旧镜像，导致设备暂时不可用。验签不提供断电恢复、A/B 回滚、镜像启动格式校验、启动向量/签名策略隔离或安全启动根；这些需由板级启动设计单独实现。签名通过也不代替镜像结构和硬件可启动性检查。

## 算法选择与资源测量

本项目提供可替换的 Ed25519ph 后端接口和可选 Monocypher 适配示例，不在仓库内复制第三方密码学实现。Monocypher 4.0.3 提供可选 SHA-512/Ed25519 源文件；项目没有集成或编译该依赖。具体 MCU 的 Flash、峰值 RAM、栈和验签耗时依目标、编译器、优化选项和所选库而异，本仓库没有硬件测量数据，均标为**未验证**。

移植时可在相同编译设置下对比 `ECAT_FOE_SIGNATURE_VERIFY_ENABLE=0/1` 的链接 map（启用 section GC），报告 `.text/.rodata` 差值；记录 verifier 上下文、SHA-512 上下文和核心实例的静态 RAM；用栈填充水位/RTOS stack high-water mark 测峰值栈；用目标 cycle counter/GPIO 包围完整回读验签路径并报告 payload 大小、频率、编译参数、读取时间和 Ed25519ph 完成时间。宿主模拟、代码体积估算或理论周期数不得写成 MCU 实测。
