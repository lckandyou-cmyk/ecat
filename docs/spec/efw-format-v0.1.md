# EtherCAT 固件容器格式规范说明书 (.efw Format Specification v0.1)

---

## 1. 概述与核心安全原则

`.efw` (EtherCAT Firmware) 是专为本通用 EtherCAT 从站 FoE 升级库设计的轻量级、自包含固件容器格式。它封装了固件有效负载（Payload）及其元数据（Metadata），为 MCU Bootloader 提供固件完整性校验的边界。

### 核心安全准则【已确定】
1. **绝对物理基地址隔离**：
   **`.efw` 容器内绝对不包含任何物理写入基地址或硬件扇区指针**。写入的物理基地址与分区最大尺寸上限完全由 Bootloader 本地的编译期配置文件（`ecat_foe_conf.h`）严格硬编码裁决，从物理机制上彻底杜绝固件篡改地址踩踏 Bootloader 自身。
2. **校验属性与安全边界说明**：
   - 容器内置的 CRC32 校验机制**仅用于检测数据在传输或非易失存储过程中的意外损坏，不具备防恶意伪造的密码学认证能力**。
   - 密码学认证（如非对称数字签名 Ed25519/ECDSA 或 HMAC 哈希）作为后续扩展特性，预留置于扩展区中，**当前 v1 格式不包含任何密码学签名或认证字段**。
3. **字节序规范**：
   所有多字节整型字段（uint16_t、uint32_t）一律采用 **小端序（Little-Endian）** 存储与解析（低字节位于低地址）。

---

## 2. 头部结构与字段定义

当前标准头部总长度 `header_length` 定为 **64 字节**。

### 2.1 头部字段排布表

| 偏移 (Offset) | 字段名称 (Field) | 类型与位宽 | 规范状态 | 详细说明与取值约定 |
| :--- | :--- | :--- | :--- | :--- |
| `0x00` - `0x03` | `magic` | uint32_t (4B) | 【已确定 v0.1】 | 容器魔数。固定为 `0x57464345`（ASCII 字符串 `"ECFW"` 的小端序表示）。 |
| `0x04` - `0x05` | `format_version` | uint16_t (2B) | 【已确定 v0.1】 | 格式规范版本号。定义为单个整数，当前为 `1`。只有破坏兼容性的改动才递增；常规扩展靠 `header_length` 增长。 |
| `0x06` - `0x07` | `header_length` | uint16_t (2B) | 【已确定 v0.1】 | 头部总字节数（包含保留区与末尾 CRC）。当前定为 `64` (`0x0040`)。 |
| `0x08` - `0x0B` | `project_id` | uint32_t (4B) | 【已确定 v0.1】 | 目标工程唯一标识 ID。用户完全自主配置，库本身不做强制约束。解析时必须与从站本地配置完全一致。 |
| `0x0C` - `0x0F` | `hardware_id` | uint32_t (4B) | 【已确定 v0.1】 | 目标硬件板型标识 ID。用户完全自主配置，库本身不做强制约束。防止误刷入不同硬件平台。解析时必须与从站本地配置完全一致。 |
| `0x10` - `0x11` | `hw_ver_min` | uint16_t (2B) | 【已确定 v0.1】 | 支持的最低硬件版本号。 |
| `0x12` - `0x13` | `hw_ver_max` | uint16_t (2B) | 【已确定 v0.1】 | 支持的最高硬件版本号。 |
| `0x14` - `0x15` | `target_id` | uint16_t (2B) | 【已确定 v0.1】 | 升级目标标识。区分固件刷入的目标部件类型（详见第 5 节）。 |
| `0x16` - `0x17` | `flags` | uint16_t (2B) | 【已确定 v0.1】 | 升级指示标志位。**v1 规范全部保留，必须为 0**；若解析器收到非零值则直接拒绝。 |
| `0x18` - `0x1B` | `fw_version` | uint32_t (4B) | 【已确定 v0.1】 | 固件数值型版本号。按 `(Major<<24)|(Minor<<16)|(Patch<<8)|Build` 编码（详见第 7 节）。 |
| `0x1C` - `0x1F` | `min_allow_version` | uint32_t (4B) | 【已确定 v0.1】 | 允许运行的最低版本号。防回滚时使用（详见第 6 节）；开关关闭时忽略。 |
| `0x20` - `0x23` | `payload_length` | uint32_t (4B) | 【已确定 v0.1】 | 有效负载（Payload）纯数据长度（字节数），不包含头部。 |
| `0x24` - `0x27` | `payload_crc32` | uint32_t (4B) | 【已确定 v0.1】 | 有效负载的完整 IEEE 802.3 CRC32 校验和。 |
| `0x28` - `0x3B` | `reserved` | uint8_t[20] (20B) | 【已确定 v0.1】 | 扩展保留区。**必须全为 0x00**，供未来扩展新元数据字段。 |
| `0x3C` - `0x3F` | `header_crc32` | uint32_t (4B) | 【已确定 v0.1】 | 文件头校验和。**固定位于头部最后 4 字节**（偏移 `header_length - 4` 到 `header_length - 1`）。 |

### 2.2 尾部填充（可选）【已确定】

为了在主机打包时主动规避底层 FoE 传输中可能出现的独立 0 长度包路径（即避免容器总字节数 `header_length + payload_length` 恰好等于底层 FoE 数据包单包容量的整数倍），打包工具（Host Packaging Tool）可在有效负载（Payload）末尾填充适量的 `0xFF` 字节：

* **归属与校验范围**：
  填充的字节**完全属于 Payload 的一部分**。其字节数直接计入文件头的 `payload_length`，填充的内容直接计入 `payload_crc32` 的计算范围。
* **写入存储与运行无影响**：
  从站核心协议机与目标驱动将填充字节作为正常 Payload 数据流同步写入底层物理介质。对于 Flash 类介质，写入未使用的 `0xFF` 字节为常规擦除态，不改变 Flash 空闲扇区属性，不影响应用程序执行。
* **不影响总长规则**：
  尾部填充不改变第 4.2 节第 12 条关于期望总长度的计算与校验规则：
  $$\text{Expected\_Total} = (\text{uint64\_t})\text{header\_length} + (\text{uint64\_t})\text{payload\_length}$$
  从站核心依然严格按照包含填充后的 `payload_length` 执行边界检查、按需擦除与多收即时熔断。
* **填充量与对齐规则**：
  打包工具在规避空包时固定补 1 字节（对于 ESC SII EEPROM 目标 `target_id == 0x0003` 硬件要求按 16 位 Word 寻址，固定补 2 字节以保持偶数长）。有效负载对齐到目标硬件的 `write_unit` 由从站核心协议机写入时在残留缓冲中处理，打包工具不管对齐。

---

## 3. CRC32 算法定义、计算范围与标准测试向量

### 3.1 算法参数写死规范【已确定】
本升级库中所有涉及 CRC32 的计算（Payload CRC32 与 Header CRC32），一律严格遵循标准 **IEEE 802.3 (CRC-32/ISO-HDLC)** 算法模型：

* **多项式 (Polynomial)**：`0x04C11DB7`（正向）/ `0xEDB88320`（反转）
* **初始值 (Initial Value)**：`0xFFFFFFFF`
* **输入数据反转 (Reflect Input)**：`True`（LSB 优先输入）
* **输出数据反转 (Reflect Output)**：`True`（LSB 优先输出）
* **异或输出值 (XOR Output)**：`0xFFFFFFFF`

### 3.2 标准测试向量【已确定】
针对标准 ASCII 测试字符串（9 字节）：
`"123456789"`（十六进制字节流：`0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39`）
按照上述算法参数计算，输出校验码必须严格等于：
`0xCBF43926`

### 3.3 Header CRC32 存放位置与计算范围规则【已确定】
* **存放位置**：固定放置于文件头的**最后 4 个字节**（即字节偏移从 `header_length - 4` 到 `header_length - 1`）。
* **计算覆盖范围**：严格覆盖从文件头起始偏移 `0x00` 一直到 `header_length - 4` 的**全部字节**（包含所有基础元数据字段与扩展保留区字段）。计算时不包含 `header_crc32` 自身这最后 4 字节。

---

## 4. 解析顺序与“擦除前必须全过”校验规则【已确定】

### 4.1 严格分阶段解析顺序【已确定】
为了以最小 RAM 开销防御畸变包，Bootloader 对 `.efw` 的解析必须按顺序分为三个阶段：

1. **阶段 1：前 8 字节轻量探测**：
   - 接收到数据流起始的前 8 字节后，立即探测并校验：
     - `magic == 0x57464345`；
     - `format_version == 1`；
     - `header_length >= 64`、`header_length <= 本地配置上限`、`header_length % 4 == 0`。
   - 只要任何一项不符，立即返回 FoE ERR 并中止，后续数据一律丢弃。
2. **阶段 2：头部整体装配与 CRC 校验**：
   - 维持流式缓冲，攒够完整的 `header_length` 字节；
   - 定位末尾 4 字节 `header_crc32`，计算前 `header_length - 4` 字节的 CRC32 并比对。不匹配则立即回复 FoE ERR 并中止。
3. **阶段 3：全量元数据安全核验**：
   - 校验全部元数据字段，严格执行下方 4.2 节清单。

### 4.2 “擦除前必须全过”清单【已确定】
在调用底层目标驱动的 `erase` 或 `write` 接口前，以下所有条件必须**全部通过**。只要有任意一项不满足，必须立即返回 FoE ERR 且**绝对禁止触碰底层物理存储**：

1. `magic` 严格等于 `0x57464345`；
2. `format_version` 严格等于 `1`；
3. `header_length` 位于 `[64, 本地上限]` 且为 4 的倍数；
4. `header_crc32` 计算比对一致；
5. `flags` 严格等于 `0`；
6. `reserved` 区域（20 字节）全为 `0x00`；
7. `project_id` 与本地硬编码配置完全一致；
8. `hardware_id` 与本地硬编码配置完全一致；
9. `hw_ver_min <= hw_ver_max`，且当前设备的本地硬件版本必须落在 `[hw_ver_min, hw_ver_max]` 区间内（两端均包含）；
10. `target_id` 为已知合法目标，且该目标在本地使能宏中已开启（未开启的目标一律拒绝）；
11. `payload_length` 未超过该目标在本地静态配置 `ECAT_FOE_xxx_MAX_SIZE` 中分配的分区容量上限（分区容量只认本地静态配置，驱动不暴露容量）；
12. **64 位总长度严格约束与即时熔断**：头部校验通过后，立即以 64 位整型计算出期望总长度：
    $$\text{Expected\_Total} = (\text{uint64\_t})\text{header\_length} + (\text{uint64\_t})\text{payload\_length}$$
    在流式接收过程中，以回调块为检测粒度：累计接收偏移加上当前回调块长度若超过 Expected_Total，解析器必须整块拒绝，立即返回 FoE ERR 熔断退出，不等传输结束，且绝对不再向物理存储写入任何数据；在传输正常结束时，累计接收总字节数必须严格等于 Expected_Total。
13. `min_allow_version <= fw_version`；若本地开启了防回滚开关，还必须满足 `fw_version >= version_floor(target_id)`；
14. 文件名与文件头中的 Project Name、Target、Version 双向强交叉一致（详见第 7 节）。

---

## 5. 升级目标 (Target ID) 与目标字样映射【已确定 v0.1】

Target ID 用于通知 Bootloader 调度何种驱动以及检验何种配置配额：

| Target ID | 目标名称 | 规范状态 | 目标字样（禁止含下划线） | 示例，非规范说明 |
| :--- | :--- | :--- | :--- | :--- |
| `0x0001` | **MCU 固件** | 【已确定 v0.1】 | `MCU` | 示例：MCU 片内 Flash 主应用程序。 |
| `0x0002` | **FPGA 镜像** | 【已确定 v0.1】 | `FPGA` | 示例：FPGA 启动 Bitstream 镜像。 |
| `0x0003` | **ESC SII EEPROM** | 【已确定 v0.1】 | `SII` | 示例：挂载于 ESC 专用总线的 ESI 配置芯片（高危目标）。 |
| `0x0004` | **外部用户数据 EEPROM** | 【已确定 v0.1】 | `EEPROM` | 示例：挂载于 MCU I2C/SPI 外设的参数与配置存储器。 |
| `0x0005` - `0xFFFF` | **预留 (Reserved)** | 【草案】 | - | 供后续扩展专用目标使用。 |

> **规则约束**：目标字样来自上述固定集合，禁止包含下划线。各目标字样与 Target ID 映射固定。

---

## 6. 向前兼容性与防回滚语义

### 6.1 向前兼容性规则【已确定】
1. **格式版本判定**：
   - `format_version` 定义为单个整数。若 `format_version != 1`，直接拒绝刷写并回复 FoE ERR，杜绝解析未知格式结构；
   - 常规兼容性扩展一律通过 `header_length` 增长实现，不递增 `format_version`。
2. **末尾 CRC 定位与扩展头部跳过**：
   - Bootloader 根据 `header_length`，从 `(header_length - 4)` 处读取 `header_crc32`；
   - 对前 `(header_length - 4)` 字节全部数据计算并验证 CRC32；
   - 有效负载物理起始偏移定义为：
     $$\text{Payload\_Offset} = \text{header\_length}$$
   - Bootloader 安全跳过 `(header_length - 64)` 字节的未知扩展字段，直接从 `Payload_Offset` 处流式读取有效负载。

### 6.2 防版本回滚语义与平台层要求【已确定】
1. **独立版本下限管理**：
   - 版本下限必须按 `target_id` 分开独立存储（每个使能的目标拥有各自互不影响的 `version_floor(target_id)`）。
2. **校验与更新时机**：
   - 校验：收到固件时，必须满足 `fw_version >= version_floor(target_id)`；
   - 更新时机：**版本下限的持久化更新必须在固件升级成功后执行**；公式如下：
     $$\text{version\_floor}_{\text{new}}(T) = \max(\text{version\_floor}_{\text{old}}(T),\ \text{min\_allow\_version})$$
   - 版本下限更新与“标记有效”的执行先后顺序由架构设计说明书（SAD）规范，本文仅作约束引用；
   - 当防回滚编译宏开关（如 `ECAT_FOE_ANTI_ROLLBACK_ENABLE`，示例名）关闭时，Bootloader 对版本下限不读、不写、不校验。
3. **对平台层的硬性要求**：
   - 若开启防回滚，平台层（Port/Platform）必须为每个使能的 Target 提供独立的、抗突发掉电损坏的非易失存储下限读写接口。

---

## 7. 文件名约定与双阶段强交叉校验规则

> **范围说明**：本节【已确定】状态仅覆盖双阶段交叉规则与切分算法本身，不覆盖依赖待定项（如目标具体命名、ID 分配）的部分。

### 7.1 文件名基础属性
* **长度限制【已核实 SOES】**：SOES 协议栈源码 `soes/esc_foe.h` 第 17 行定义 `#define FOE_FN_MAX      31`，即原生 SOES 协议栈最大支持 31 字符的文件名长度（包含扩展名）。当前规范定义的文件名命名格式（典型长度如 `SERVO_DRV_MCU_V1.2.3.4.efw` 为 26 字符）严格处于 31 字符限制内。从站工程自定义命名时总长度严禁超过 31 字符。
* **字符编码**：标为 **【未核实】**。
* **协议前缀限制**：FoE 协议官方是否限制特定前缀标为 **【未核实】**。

### 7.2 文件名命名格式与逆向切分算法【已确定】
文件名格式固定定义为：
```text
<项目名>_<目标>_V<版本>.efw
```

由于项目名本身可能包含下划线（例如 `SERVO_DRV`），文件名解析必须**从字符串末尾向前逆向切分**：
1. 剥离末尾扩展名 `.efw`；
2. 从右向左查找第一个下划线 `_`，该下划线之后的部分为版本字符串（剥离前缀字母 `V`/`v`）；
3. 从上一步的位置继续向左查找下一个下划线 `_`，这两个下划线之间的部分即为**目标字样**（目标字样来自固定集合，禁止包含下划线）；
4. 剩余的前半部分全部字符串（无论包含多少个下划线）即为**项目名**。

### 7.3 版本数值映射规则【已确定】
文件名中的版本字符串与文件头中的 `fw_version` 数值采用定宽映射：
$$\text{fw\_version} = (\text{Major} \ll 24) \mid (\text{Minor} \ll 16) \mid (\text{Patch} \ll 8) \mid \text{Build}$$
* 每个分量取值范围为 `0` 到 `255`；
* **文件名缺段补 0**：若文件名版本写为 `V1.2`，则等价于 `1.2.0.0`，对应数值 `0x01020000`；若为 `V2` 则等价于 `2.0.0.0`，对应 `0x02000000`。

### 7.4 双阶段强交叉校验规则【已确定】
1. **WRQ 阶段（首阶段快速拒绝）**：
   - Bootloader 拦截到 FoE WRQ 报文后，逆向切分解析文件名；
   - 校验提取的项目名是否与从站本地配置完全一致，提取的目标是否在从站本地已开启使能；若不符直接回复 FoE ERR 拒绝，中止握手。
2. **首包到达阶段（严格双向一致性校验）**：
   - 第一包数据解析出 `.efw` 头部后，强制执行三方双向一致性比对：
     - 文件名里的项目字符串比对本地配置 `ECAT_FOE_PROJECT_NAME`；文件头里的 `project_id` 比对本地配置 `ECAT_FOE_PROJECT_ID`；两者各自对本地配置，互不比较；
     - 文件头 `target_id` 必须与文件名解析出的目标字样完全匹配；
     - 文件头 `fw_version` 必须与文件名提取的版本数值完全一致；
   - 若出现任何一项不符，立即回复 FoE ERR 中断流程，**绝对不触碰底层物理存储**。

---

## 8. 二进制头部样例与反例测试向量

### 8.1 正例配置参数与计算输出
* `magic`: `0x57464345` ("ECFW")
* `format_version`: `1` (`0x0001`)
* `header_length`: `64` (`0x0040`)
* `project_id`: `0x10010001`
* `hardware_id`: `0x00000001`
* `hw_ver_min`: `1` (`0x0001`)
* `hw_ver_max`: `1` (`0x0001`)
* `target_id`: `1` (`0x0001` - APP)
* `flags`: `0` (`0x0000`)
* `fw_version`: `0x01020304` (V1.2.3.4)
* `min_allow_version`: `0x01000000` (V1.0.0.0)
* `payload`: 16 字节线性测试序列（`0x00, 0x01, ..., 0x0F`）
* `payload_length`: `16` (`0x00000010`)
* `reserved`: 20 字节全 `0x00`
* **计算校验码**：
  - `payload_crc32`: `0xCECEE288`
  - `header_crc32`（覆盖前 60 字节）: `0x61C252E3`

#### 正例十六进制转储 (Hex Dump)
```text
正例头部 64 字节转储 (Valid Header Hex Dump):
0x0000:  45 43 46 57 01 00 40 00 01 00 01 10 01 00 00 00   |ECFW..@.........|
0x0010:  01 00 01 00 01 00 00 00 04 03 02 01 00 00 00 01   |................|
0x0020:  10 00 00 00 88 E2 CE CE 00 00 00 00 00 00 00 00   |................|
0x0030:  00 00 00 00 00 00 00 00 00 00 00 00 E3 52 C2 61   |.............R.a|

正例有效负载 16 字节转储 (Valid Payload Hex Dump):
0x0000:  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F   |................|
```

### 8.2 四个典型反例测试向量 (Negative Test Vectors)

#### 反例 1：Header CRC 损坏或被篡改
* **故障现象**：前 60 字节元数据完全正常，但末尾 4 字节 Header CRC 被篡改为 `0x9E3DAD1C`（真实期望值为 `0x61C252E3`）。
* **末尾十六进制**：`... 00 00 00 00 1C AD 3D 9E`
* **预期拒绝原因**：解析阶段 2 检出 `Header CRC32 mismatch`，直接回复 FoE ERR，拒绝进入阶段 3，不触碰底层存储。

#### 反例 2：流总长度少收（传输结束时总长不符）
* **故障现象**：头部合法且声明 `header_length = 64`、`payload_length = 16`（标称总字节数 80 字节）。但在 FoE 写入结束时（依据 SOES `esc_foe.c` 源码第 479/493 行，从站接收到数据包长小于最大邮箱容量 `data_len < ESC_FOE_DATA_SIZE` 判定为最后一包并触发 `FOE_fclose`），主站实际传输的 Payload 累计只有 12 字节（总共 76 字节）。
* **预期拒绝原因**：固件传输结束时检出 `Actual stream length (76) != header_length + payload_length (80)`，拒绝提交固件，回复 FoE ERR，中止流程。

#### 反例 3：Flags 包含未定义的非零标志位
* **故障现象**：头部注入了 `flags = 0x0001`（偏移 `0x16`-`0x17` 为 `01 00`），其余字段与正例相同，重新计算该头部的合法 `header_crc32` 为 `0xEC4AAF01`（Header CRC 自洽通过）。
* **完整 64 字节十六进制转储**：
```text
反例 3 头部转储 (Non-zero Flags Hex Dump):
0x0000:  45 43 46 57 01 00 40 00 01 00 01 10 01 00 00 00   |ECFW..@.........|
0x0010:  01 00 01 00 01 00 01 00 04 03 02 01 00 00 00 01   |................|
0x0020:  10 00 00 00 88 E2 CE CE 00 00 00 00 00 00 00 00   |................|
0x0030:  00 00 00 00 00 00 00 00 00 00 00 00 01 AF 4A EC   |..............J.|
```
* **预期拒绝原因**：解析阶段 3 在“擦除前必须全过”检查中检出 `flags != 0`，判定为非法版本特性，回复 FoE ERR 拒绝，不触碰底层存储。

#### 反例 4：流总长度多收（某一次回调累计超出期望总长即时熔断）
* **故障现象**：头部声明 `header_length = 64`、`payload_length = 16`（期望总长 80 字节）。主站实际发送了 84 字节（Payload 发送了 20 字节，前 16 字节正常，后续附加了 `0x10, 0x11, 0x12, 0x13`）。
* **预期行为与拒绝原因**：
  Bootloader 在流式接收过程中以回调块为粒度持续校验累计偏移；当某一次回调累计超出期望总长（例如某次块写入导致累计偏移达到 84 字节，突破 Expected_Total 的 80 字节），**整块拒绝该数据，立即返回 FoE ERR 熔断拒绝，不等传输结束，且绝对不再向存储写入任何数据**。
* **反例 4 数据流转储（后部多出数据）**：
```text
反例 4 有效负载 20 字节转储 (Payload 20 Bytes Hex Dump):
0x0000:  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F   |................|
0x0010:  10 11 12 13                                       |....|
         ^^ (第 81 字节，在此处即刻触发 FoE ERR 熔断退出)
```

### 8.3 测试向量生成与独立交叉验证脚本
```python
import struct
import binascii

magic = 0x57464345         # 'ECFW' little endian
format_version = 1
header_length = 64
project_id = 0x10010001
hardware_id = 0x00000001
hw_ver_min = 1
hw_ver_max = 1
target_id = 1
flags = 0
fw_version = 0x01020304
min_allow_version = 0x01000000

payload = bytes([i for i in range(16)])
payload_length = len(payload)

def crc32_bitwise(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc = crc >> 1
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF

# 交叉校验测试向量
assert binascii.crc32(b'123456789') == 0xCBF43926
assert crc32_bitwise(b'123456789') == 0xCBF43926

# 正例生成
p_crc = binascii.crc32(payload) & 0xFFFFFFFF
assert p_crc == crc32_bitwise(payload) == 0xCECEE288

header_prefix = struct.pack(
    '<IHHIIHHHHIIII',
    magic, format_version, header_length, project_id,
    hardware_id, hw_ver_min, hw_ver_max, target_id,
    flags, fw_version, min_allow_version,
    payload_length, p_crc
) + bytes(20)

h_crc = binascii.crc32(header_prefix) & 0xFFFFFFFF
assert h_crc == crc32_bitwise(header_prefix) == 0x61C252E3
valid_header = header_prefix + struct.pack('<I', h_crc)

# 反例 1: 故意注入错误的 Header CRC
bad_header_crc = h_crc ^ 0xFFFFFFFF  # 0x9E3DAD1C
case1_header = header_prefix + struct.pack('<I', bad_header_crc)

# 反例 3: flags 注入非零值 0x0001 并自洽计算 CRC
header_prefix_bad_flags = struct.pack(
    '<IHHIIHHHHIIII',
    magic, format_version, header_length, project_id,
    hardware_id, hw_ver_min, hw_ver_max, target_id,
    0x0001, fw_version, min_allow_version,
    payload_length, p_crc
) + bytes(20)
h_crc_case3 = binascii.crc32(header_prefix_bad_flags) & 0xFFFFFFFF
assert h_crc_case3 == 0xEC4AAF01
case3_header = header_prefix_bad_flags + struct.pack('<I', h_crc_case3)

# 反例 4: 流总长度多收 (期望 80 字节，实际发送 84 字节)
bad_payload_20 = bytes([i for i in range(20)])
stream_case4 = valid_header + bad_payload_20  # 共 84 字节
expected_total = header_length + payload_length  # 80 字节
received_count = 0
for idx, b in enumerate(stream_case4):
    received_count += 1
    if received_count > expected_total:
        # 在第 81 字节即时熔断
        assert received_count == 81
        assert b == 0x10
        break
```

---

## 9. 待决策清单（已清空 / 转入 SAD 决策）

当前 .efw 容器格式规范核心要素均已确定：
* **目标字样与映射**：已确定为 `MCU` (`0x0001`)、`FPGA` (`0x0002`)、`SII` (`0x0003`)、`EEPROM` (`0x0004`)。
* **Project ID 与 Hardware ID 分配**：已确定为用户在本地配置中完全自主配置，库本身不强加中心化约束，README 中提供分配建议。
* **版本映射格式**：已确定为数值型 32 位定宽映射，文件名缺段补 0。

---

## 10. 跨文档架构备忘 (Cross-Document Notes)

> **关于分包完整性校验、防变砖时序与下限更新的约束引用**：
> 1. `payload_crc32` 是对完整固件流的校验码，**只能在所有分包全部接收完毕时方可完成累计校验并执行比对**；
> 2. 在单分区直接覆盖模式下，MCU 片内 Flash 执行擦除后即处于未决损坏状态。为防止传输意外中断变砖，系统必须严格执行“写前将本地 APP 状态标记为无效、全部数据包接收且 CRC32 校验通过后方才标记为有效”的状态机控制；
> 3. 防回滚版本下限持久化更新的时机，以及其与“标记有效”的绝对执行先后顺序，由架构设计说明书（SAD）具体规范，本文仅作数据格式层面的约束引用。
