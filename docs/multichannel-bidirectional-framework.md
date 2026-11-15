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

/* 注册参数区 (支持读写；若需导出且涉及硬件互斥/锁，提供成对的 read_open 与 read_close) */
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
        .get_data_size = my_eeprom_get_size,
        .read_open = my_eeprom_read_open,   /* 导出只读生命周期: 与 read_close 成对 */
        .read_close = my_eeprom_read_close  /* 导出结束/取消释放只读锁，绝不误伤镜像 */
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

---

## 5. 当前完成度与实现边界说明 (Implementation Status & Boundaries)

为了保证工程交付的严谨性与对标工业级嵌入式产品的准确性，特此对当前框架的实现边界作以下明确区分：

### 5.1 硬件适配器完成度说明 (Hardware Adapters)
- **已实现/已接入适配器**：
  - `src/adapters/ecat_foe_transport.c`：面向 EtherCAT 从站协议栈（Beckhoff SSC / SOES）FoE 状态机的适配桥接层。
  - `src/adapters/ecat_upgrade_file.c`：面向宿主机标准 I/O 流 (`FILE *`) 的测试与集成参考适配器。
- **未在此仓库内实现的内容（标为未实现）**：
  - 具体的物理外设硬件驱动（如真实的 MCU 片上 UART 硬件中断/DMA 驱动、物理 CAN 现场总线控制器驱动、SPI 外设驱动等）**未内置在当前仓库中**。
  - 架构设计中的 UART、CAN 等通道 ID 属于通道编号规范，单元测试中的 `ep_uart`、`ep_can` 属于通用传输端点接口 (`ecat_transport_endpoint_t`) 的原型验证，实际硬件工程集成时须由集成方根据目标板级芯片 SDK 实现对应外设中断收发回调。

### 5.2 异步传输标志完成度说明 (Asynchronous Flags)
- **未实现的内容（标为未实现/规划中）**：
  - 目标描述符 `ecat_target_desc_t` 中的 `asynchronous` 标志目前在双向传输管理器 (`ecat_transfer_t`) 中**尚未实现**。
  - 当前 `ecat_transfer_t` 的 `ecat_transfer_begin`、`ecat_transfer_write`、`ecat_transfer_read`、`ecat_transfer_finish` 等 API 严格采用同步调用模型；对于非阻塞分步轮询与异步完成回调，双向传输层暂不执行异步调度。当前对异步目标块的处理保留给底层的 `ecat_foe_core_async` 核心，双向传输层暂不执行异步调度。

### 5.3 只读生命周期契约与固件容器流上限规范说明 (Readonly Lifecycle & Container Limit)
- **只读导出生命周期契约 (EXPORT Lifecycle)**：
  - **无生命周期纯 read 驱动**：若 `read_open == NULL && read_close == NULL && io.init == NULL`，导出请求直接通过 `io.read` 执行，无需获取/释放生命周期锁；
  - **有生命周期的只读驱动**：必须显式且成对提供 `.read_open` 与 `.read_close`。在导出开始时调用 `read_open`，导出正常结束或取消时调用 `read_close`（仅释放只读锁，绝不触碰镜像有效性标记）；
  - **安全防御**：若驱动仅配置了写生命周期的 `io.init` 但缺少安全的 `read_close`，导出请求在 `begin` 阶段直接被拒绝（`ECAT_TRANSFER_ERR_DRIVER_INIT`），**绝不回退调用 `io.init`**，彻底杜绝因驱动在取消/失败时误调用 `io.abort` 损坏介质镜像有效性的隐患。
- **固件容器流总长物理上限 (Firmware Container Limit)**：
  - 依据 `docs/spec/efw-format-v0.2.md:53`，固件头部恒定为 148 字节，空包规避填充若存在亦计入 `payload_length`，传输流中绝不存在独立的尾部未覆盖填充区，流总长恒定为 `header_length + payload_length`；
  - 驱动层不足 `write_unit` 的物理 Flash 补齐仅发生在设备内部缓冲写入阶段，不属于传输流尺寸；
  - 固件容器流物理总上限精确为 `min(target.max_size, core.max_payload_size) + 148U`。任何超过此物理上限的流在 `begin` 阶段立即被精准拦截。
