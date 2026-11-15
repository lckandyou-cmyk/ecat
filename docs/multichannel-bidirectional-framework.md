# 嵌入式多通道双向数据传输与固件升级框架指南

## 1. 架构总览 (Architecture Overview)

本框架提供了一套面向资源受限嵌入式系统的面向嵌入式设备的目标注册、同步双向数据传输与固件升级接口。当前以宿主测试为主，硬件验证和部分功能仍待完成。

```text
FoE 协议栈 ── FoE 适配桥 ──┐
文件流参考入口 ─────────────┤
自定义总线 ── 传输端点 ───┼── ecat_transfer_t (权威统一核心) ──┬── 固件安全策略机 ── 固件驱动
                             │    (会话仲裁 / ACL / 同步&异步)    └── 数据驱动 (同步/真正异步)
遗留固件升级 API (兼容) ────┘            │
   └── ecat_upgrade_t ───────────────────┘ (共享会话租约仲裁器 ecat_upgrade_session_t)
```

系统以 `ecat_transfer_t` 为唯一的权威调度核心；FoE 协议栈、文件流与自定义端点均可原生直连统一传输核心。旧升级管理器（`ecat_upgrade_t`）作为薄向下兼容层，与统一传输管理器通过全局唯一的 `ecat_upgrade_session_t` 会话仲裁器实现强绑定与排他互斥，避免双重租约与状态撕裂。

---

## 2. 三大核心特性

### 2.1 通信通道自由选配与零开销裁剪 (Pluggable Transports)
- **按需选择入口**：`make build-upgrade` 默认启用 FoE，文件流参考入口默认关闭；可通过构建变量关闭或启用这些入口。通用注册表、传输管理器和端点源码目前始终加入该静态库。
- **支持全关与自定义**：系统允许关闭所有内置协议，仅保留核心框架。通过 `ecat_transport_endpoint_t` 仅需几行代码即可将任意私有总线（如 CAN、UART、SPI 等）接入框架。

### 2.2 目标区块自由扩展 (Target Block Registry)
- **注册目标**：通过 `ecat_target_registry_t` 注册数据或固件目标，数量受 `ECAT_TARGET_REGISTRY_MAX_BLOCKS` 限制（默认 16）。固件导入仍须映射到核心已启用的 1～4 号目标；注册更多数据区块不扩展固件核心目标数。
- **业务类型区分**：
  - **固件类区块 (`ECAT_TARGET_TYPE_FIRMWARE`)**：由固件核心解析 EFW 并检查目标和设备兼容性。Ed25519ph 验签和防回滚由本地配置决定，默认不强制启用；启用后须按核心契约提供相应后端与驱动。
  - **数据类区块 (`ECAT_TARGET_TYPE_DATA`)**：面向参数、日志、配置、字库等纯数据流，轻量读写并支持流式 CRC32 完整性计算。
- **物理边界硬编码**：设备本地硬编码掌控各分区的物理基地址与容量上限，坚决不信任外部请求携带的任意物理地址。

### 2.3 完整双向导入/导出与访问控制矩阵 (Bidirectional Transfer & ACL)
- **双向流能力**：
  - **导入 (IMPORT)**：外部向设备写入数据或进行固件升级，具备按需自动擦除、分块写入与事务提交。
  - **导出 (EXPORT)**：设备向外部输出数据（如回读运行日志、导出参数备份等）。
- **细粒度访问控制矩阵 (ACL)**：
  - 可按通道、目标和读写权限配置白名单。ACL 默认关闭；调用 `ecat_target_registry_set_enforce_acl(&reg, true)` 才启用白名单，目标自身的权限仍然生效。
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
- **会话仲裁**：共享同一个核心的管理器使用共享租约；无核心的独立管理器各自拥有租约。调用必须串行化；多个独立核心或管理器使用同一物理存储时，集成方还须提供跨实例互斥。

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

### 5.2 异步传输能力与真实边界说明 (Asynchronous Transfer & Hardware Boundaries)
- **已实现/已验证的异步能力**：
  - 目标描述符 `ecat_target_desc_t` 中的 `asynchronous` 标志已在权威双向传输引擎 (`ecat_transfer_t`) 中**完整实现**；
  - **真正的非阻塞异步 DATA 驱动**：完整支持 `ecat_async_data_ops_t` 接口，覆盖 `INIT -> INVALIDATE -> ERASE_TO -> WRITE -> FINALIZE` 完整写入生命周期，以及 `READ_OPEN -> READ -> READ_CLOSE` 独立只读生命周期，支持 `PENDING` 挂起与 `ecat_transfer_poll` 轮询推进；
  - **固件异步复用与防伪造绑定**：异步固件导入直接复用真实固件升级核心 (`ecat_foe_core_async_t`)，强制校验后端 ops 绑定有效性，防止伪造 ops 绕过密码学验签；
  - **通用端点异步支持**：传输端点 (`ecat_transport_endpoint_t`) 支持异步启动挂起 (`begin_pending`)、非阻塞轮询推进以及完成/中止事件单次且仅触发一次的严格保障。
- **真实硬件与运行时边界如实声明**：
  - 本仓库未内置具体的物理 MCU 片上外设寄存器驱动（如物理芯片 USART/CAN/SPI 控制器中断收发与 DMA 驱动），实际硬件工程须由集成方挂载芯片外设驱动；
  - 尚未在真实物理 32 位 MCU 芯片上进行动态在线升级联调，真实硬件投产前须在目标芯片上完成底层驱动移植核验。

### 5.3 只读生命周期契约与固件容器流上限规范说明 (Readonly Lifecycle & Container Limit)
- **只读导出生命周期契约 (EXPORT Lifecycle)**：
  - **无生命周期纯 read 驱动**：若 `read_open == NULL && read_close == NULL && io.init == NULL`，导出请求直接通过 `io.read` 执行，无需获取/释放生命周期锁；
  - **有生命周期的只读驱动**：必须显式且成对提供 `.read_open` 与 `.read_close`。在导出开始时调用 `read_open`，导出正常结束或取消时调用 `read_close`（仅释放只读锁，绝不触碰镜像有效性标记）；
  - **安全防御**：若驱动仅配置了写生命周期的 `io.init` 但缺少安全的 `read_close`，导出请求在 `begin` 阶段直接被拒绝（`ECAT_TRANSFER_ERR_DRIVER_INIT`），**绝不回退调用 `io.init`**，彻底杜绝因驱动在取消/失败时误调用 `io.abort` 损坏介质镜像有效性的隐患。
- **固件容器流总长物理上限 (Firmware Container Limit)**：
  - 依据 `docs/spec/efw-format-v0.2.md:53`，固件头部恒定为 148 字节，空包规避填充若存在亦计入 `payload_length`，传输流中绝不存在独立的尾部未覆盖填充区，流总长恒定为 `header_length + payload_length`；
  - 驱动层不足 `write_unit` 的物理 Flash 补齐仅发生在设备内部缓冲写入阶段，不属于传输流尺寸；
  - 固件容器流物理总上限精确为 `min(target.max_size, core.max_payload_size) + 148U`。任何超过此物理上限的流在 `begin` 阶段立即被精准拦截。

## 6. 集成边界与检查

- **入口与共享状态**：FoE 和文件流适配器使用 `ecat_upgrade_t`；自定义双向通道使用 `ecat_transport_endpoint_t` 和 `ecat_transfer_t`。共享同一个 `ecat_foe_core_t` 时，首个管理器的生命周期必须覆盖其余管理器；绑定同一核心的传输管理器必须使用同一个注册表。调用方须串行调用接口。
- **传输长度**：数据导入的 `expected_size` 为非零时，写入不能超过该值，结束时必须恰好匹配；0 表示未知长度。固件导入的声明长度是整个 EFW 容器长度，不是仅 payload 的长度。
- **失败与取消**：检查每次返回值；参数错误可能允许修正后重试，主动放弃时调用端点的 `abort`。传输结果不能替代设备启动成功的确认。
- **配置一致性**：`ECAT_TARGET_REGISTRY_MAX_BLOCKS`（默认 16）和 `ECAT_TARGET_REGISTRY_MAX_ACL_RULES`（默认 32）改变公开结构体布局，所有编译单元须使用相同数值；错配应在链接时因 `ecat_target_registry_abi_...` 符号报错。相关头文件要求编译器支持 `used` 和 `retain` 属性。
- **访问控制**：ACL 检查通道 ID 和目标权限，不提供通道身份认证。自定义适配器须保证通道身份可信，并按设备需要配置白名单；不应将权限标志当作加密或认证。
- **移植验证**：检查分区地址、擦写粒度、驱动上下文、断电恢复、看门狗、通信超时和启动交接；宿主 Mock、静态编译和单元测试不能替代 32 位 MCU 和真实协议栈实测。

本节示例为接口示意，`my_eeprom_*` 与 `data_buf` 由集成方提供，生产代码须检查初始化、注册、读写和结束操作的返回值。
