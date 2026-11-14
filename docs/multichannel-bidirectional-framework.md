# 嵌入式多通道双向数据传输与固件升级框架指南

## 1. 架构总览 (Architecture Overview)

本框架提供了一套面向资源受限嵌入式系统的**高可靠、零动态内存分配 (0 malloc)、完全可裁剪且可自由扩展的双向数据传输与固件升级核心**。

```text
       可选/自定义传输通道 (Transports)                可扩展存储目标区块 (Targets)
       ┌───────────────────────────┐                ┌─────────────────────────┐
       │ FoE (EtherCAT File-over-EC)│                │ MCU 固件主/备分区       │
       ├───────────────────────────┤                ├─────────────────────────┤
       │ Host File (文件流参考入口) │  双向传输管理器  │ FPGA 逻辑镜像区         │
       ├───────────────────────────┤ (ecat_transfer)├─────────────────────────┤
       │ UART / 串口 (自定义协议)   │ ══════════════ │ 参数配置区 (EEPROM/Flash)│
       ├───────────────────────────┤                ├─────────────────────────┤
       │ CAN / 现场总线             │                │ 黑匣子运行日志区         │
       ├───────────────────────────┤                ├─────────────────────────┤
       │ 用户私有工业总线 / BLE / 网口│                │ 用户自定义私有存储区块   │
       └───────────────────────────┘                └─────────────────────────┘
                    │                                            ▲
                    └─────── 访问控制矩阵 (ACL 白名单验证) ───────┘
```

---

## 2. 三大核心特性

### 2.1 通信通道自由选配与零开销裁剪 (Pluggable Transports)
- **按需开启**：内置的 FoE、文件流等通道可按需开启。如果不需要某个通道，在构建期关闭对应宏，不引入任何无用代码。
- **支持全关与自定义**：系统允许关闭所有内置协议，仅保留核心框架。通过 `ecat_transport_endpoint_t` 仅需几行代码即可将任意私有总线（如 CAN、UART、SPI 等）接入框架。

### 2.2 目标区块自由扩展 (Target Block Registry)
- **打破固定目标限制**：不再硬编码 4 种固定目标（MCU/FPGA/SII/EEPROM）。集成方可以通过 `ecat_target_registry_t` 动态或静态注册任意数量的目标区块。
- **业务类型区分**：
  - **固件类区块 (`ECAT_TARGET_TYPE_FIRMWARE`)**：必须经过严格的 EFW 容器头部解析、Ed25519ph 数字签名、硬件版本匹配、单调递增防回滚与双向物理回读 CRC32 比对。
  - **数据类区块 (`ECAT_TARGET_TYPE_DATA`)**：面向参数、日志、配置、字库等纯数据流，轻量读写并支持流式 CRC32 完整性计算。
- **物理边界硬编码**：设备本地硬编码掌控各分区的物理基地址与容量上限，坚决不信任外部请求携带的任意物理地址。

### 2.3 完整双向导入/导出与访问控制矩阵 (Bidirectional Transfer & ACL)
- **双向流能力**：
  - **导入 (IMPORT)**：外部向设备写入数据或进行固件升级，具备按需自动擦除、分块写入与事务提交。
  - **导出 (EXPORT)**：设备向外部输出数据（如回读运行日志、导出参数备份等）。
- **细粒度访问控制矩阵 (ACL)**：
  - 支持按 `(transport_id, target_id, direction)` 配置严格白名单。
  - 例如：可配置 FoE 仅能刷写固件，调试串口仅能读写参数区，CAN 仅能导出日志。非法越权访问在握手时立即被拦截，防止非授权写入或数据泄露。

---

## 3. 快速接入示例

### 3.1 注册存储目标区块
```c
#include "ecat_target_registry.h"

ecat_target_registry_t reg;
ecat_target_registry_init(&reg);

/* 注册参数区 (支持读写) */
ecat_target_desc_t param_desc = {
    .target_id = 2U,
    .name = "sys_params",
    .type = ECAT_TARGET_TYPE_DATA,
    .permissions = ECAT_PERM_RW,
    .max_size = 4096U,
    .enabled = true,
    .driver = {
        .io = {
            .init = my_eeprom_init,
            .erase_to = my_eeprom_erase,
            .write = my_eeprom_write,
            .read = my_eeprom_read,
            .finalize = my_eeprom_finalize,
            .abort = my_eeprom_abort
        },
        .get_data_size = my_eeprom_get_size
    }
};
ecat_target_registry_add(&reg, &param_desc);
```

### 3.2 配置访问控制矩阵 (ACL)
```c
/* 启用强制 ACL 白名单过滤 */
ecat_target_registry_set_enforce_acl(&reg, true);

/* 允许 UART (ID=10) 读写参数区 (ID=2) */
ecat_target_registry_add_acl(&reg, 10U, 2U, ECAT_PERM_RW);
```

### 3.3 初始化传输管理器并绑定传输端点
```c
#include "ecat_transfer.h"
#include "ecat_transport_endpoint.h"

ecat_transfer_t mgr;
ecat_transfer_init(&mgr, &reg, NULL);

ecat_transport_endpoint_t uart_ep;
ecat_transport_endpoint_init(&uart_ep, 10U, "uart_protocol", &mgr, NULL);

/* 发起数据写入 */
ecat_transport_endpoint_begin_import(&uart_ep, 2U, 128, 0);
ecat_transport_endpoint_write_chunk(&uart_ep, data_buf, 128);
ecat_transport_endpoint_finish_import(&uart_ep);
```

---

## 4. 嵌入式约束保证
- **零动态分配 (Zero malloc)**：所有结构体大小编译期确定，可在静态数据段 (`.bss`) 或调用方栈上分配。
- **单一互斥租约 (Mutex Lease)**：多通道共享底层 Flash 驱动时，通过会话令牌强行保证操作互斥与排他性，杜绝多通道并发操作硬件造成的总线踩踏。
