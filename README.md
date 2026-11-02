# ecat (Universal EtherCAT FoE Upgrade Library)

面向嵌入式从站设备的轻量、通用、高安全 **EtherCAT FoE (File over EtherCAT)** 固件升级核心库。

---

## 🌟 设计目标与核心原则

1. **即插即用**：只需将核心文件加入现有工程即可编译使用。
2. **标准合规**：所有升级流程严格走 EtherCAT 标准 `BOOT` 状态机与 `FoE` 协议（ETG.1000.6），**不自造私有协议，不改动 EtherCAT 标准状态机**。
3. **独立 Bootloader 架构**：由独立 MCU Bootloader 执行固件接收与烧录，即使应用固件损坏依然可无缝恢复。
4. **统一 `.efw` 容器格式**：文件名与 84 字节二进制文件头双重识别匹配，校验工程 ID、硬件 ID、目标 ID 与版本。
5. **绝对物理分区信任**：写入地址仅采信 Bootloader 本地静态配置，绝不信任文件中携带的任意地址，彻底杜绝越界写与 Bootloader 踩踏。
6. **多目标灵活支持**：APP、FPGA、EEPROM 升级全部可选，大多数场景可仅开启 APP。
7. **芯片与协议栈完全解耦**：核心为纯 C99 实现（无动态内存分配 `malloc`），提供两层极简接口：
   - **FoE 协议栈适配层**：已包含针对 Beckhoff SSC 与 SOES 的薄转接层。
   - **目标驱动层**：擦除、写入、校验、提交抽象为函数指针，用户按自己芯片的 Flash/SPI 控制器注册即可。

---

## 📂 目录结构

```text
ecat/
├── include/
│   ├── ecat_foe.h               # 核心库主头文件与 API 声明
│   ├── ecat_foe_def.h           # 标准 FoE 错误码、目标 ID 与 .efw 头部结构
│   ├── ecat_foe_target.h        # 目标存储驱动抽象操作接口
│   └── ecat_foe_conf_template.h # 全局工程与分区配置头模板
├── src/
│   └── ecat_foe_core.c          # 纯 C99 核心状态机、头部解析与流式校验
├── adapters/
│   ├── ecat_foe_ssc.h / .c      # Beckhoff Slave Stack Code (SSC) 薄适配层
│   └── ecat_foe_soes.h / .c     # Simple Open EtherCAT Slave (SOES) 薄适配层
├── tools/
│   └── efw_pack.py              # .efw 标准固件容器打包脚本 (Python)
├── tests/
│   └── test_ecat_foe.c          # 全覆盖单元测试与模拟用例
├── Makefile                     # 编译构建与测试驱动
└── README.md
```

---

## 📦 `.efw` 固件容器格式规范

升级文件统一部署为 `.efw` 格式，由 **84 字节固定头部** 与 **原始固件 Payload** 组成：

| 偏移 (Byte) | 字段 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| `0x00 ~ 0x03` | `magic` | `uint32_t` | 固定为 `0x31574645` ("EFW1") |
| `0x04 ~ 0x05` | `header_version` | `uint16_t` | 头部版本，当前为 `1` |
| `0x06 ~ 0x07` | `header_size` | `uint16_t` | 头部尺寸 = `84` |
| `0x08 ~ 0x0B` | `project_id` | `uint32_t` | 唯一数字工程 ID (如 `0x10010001`) |
| `0x0C ~ 0x0F` | `hardware_id` | `uint32_t` | 目标硬件版本 ID (如 `0x00000001`) |
| `0x10 ~ 0x11` | `target_id` | `uint16_t` | 1: APP, 2: FPGA, 3: EEPROM |
| `0x12 ~ 0x13` | `flags` | `uint16_t` | 扩展标志位 (保留置 0) |
| `0x14 ~ 0x17` | `version_raw` | `uint32_t` | 32 位原始版本号 |
| `0x18 ~ 0x37` | `project_name` | `char[32]` | 工程名称字符串 (如 `"SERVO_DRV"`) |
| `0x38 ~ 0x47` | `version_str` | `char[16]` | 版本号字符串 (如 `"1.0.0"`) |
| `0x48 ~ 0x4B` | `raw_payload_size`| `uint32_t` | 原始固件二进制字节数 |
| `0x4C ~ 0x4F` | `raw_payload_crc` | `uint32_t` | 原始固件 IEEE 802.3 CRC32 校验和 |
| `0x50 ~ 0x53` | `header_crc` | `uint32_t` | 头部自身的 CRC32 (计算时此字段填 0) |

### 双重识别规则
1. **文件名规则**：必须形如 `<PROJECT>_<TARGET>_<VERSION>.efw`（例如 `SERVO_DRV_APP_1.0.0.efw`）。
2. **内容校验**：文件首包解析后，核心库核对文件头中的 `project_id`、`hardware_id`、`target_id` 及工程名称，必须与从站本地配置和下发的文件名完全吻合，否则在写入前立即以 FoE 错误码终止流程。

---

## 🛠️ 移植与快速上手

### 1. 复制与定制配置文件
复制 `include/ecat_foe_conf_template.h` 为 `ecat_foe_conf.h`，按需配置：
```c
#define ECAT_FOE_CONF_PROJECT_ID      0x10010001U
#define ECAT_FOE_CONF_PROJECT_NAME    "SERVO_DRV"
#define ECAT_FOE_CONF_HARDWARE_ID     0x00000001U

#define ECAT_FOE_ENABLE_TARGET_APP    1
#define ECAT_FOE_APP_BASE_ADDR        0x08020000U
#define ECAT_FOE_APP_MAX_SIZE         (384U * 1024U)
```

### 2. 注册硬件目标驱动
以内部 Flash 的 MCU APP 升级为例：
```c
static const ecat_foe_target_ops_t s_app_ops = {
    .target_id = ECAT_FOE_TARGET_APP,
    .name      = "APP",
    .init      = bsp_flash_unlock,
    .erase     = bsp_flash_erase,
    .write     = bsp_flash_write,
    .verify    = bsp_flash_verify_crc,
    .finalize  = bsp_flash_mark_boot_valid,
    .abort     = bsp_flash_lock,
};

ecat_foe_register_target(&s_app_ops, NULL);
```

### 3. 转接协议栈
- **Beckhoff SSC**：直接在 `foeappl.c` 中将 `BL_FoeWrite`、`BL_FoeData` 转接到 `ecat_foe_ssc_write()`、`ecat_foe_ssc_data()`。
- **SOES**：将 `foe_file_cfg_t` 结构体的 `write` 与 `write_data` 绑定至 `ecat_foe_soes_write()` 与 `ecat_foe_soes_write_data()`。

---

## 🚀 固件打包工具使用

使用提供的 Python 工具将编译生成的 `.bin` 打包为 `.efw` 文件：
```bash
python3 tools/efw_pack.py \
  --bin build/app.bin \
  --project-id 0x10010001 \
  --project-name SERVO_DRV \
  --hw-id 0x00000001 \
  --target APP \
  --version 1.0.0 \
  -o SERVO_DRV_APP_1.0.0.efw
```

---

## 🧪 测试与验证

项目内置纯 C 单元测试，测试套件涵盖常规升级、文件名篡改、工程 ID 不匹配、目标未使能拦截、分区超额防御、数据包 CRC 篡改及协议栈适配层测试：

```bash
make test
```

---

## 📄 开源许可证

本项目基于 [MIT License](LICENSE) 开源发布。
