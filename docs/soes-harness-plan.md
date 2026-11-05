# SOES 状态机联动测试扩展可行性评估与规划 (Harness Expansion Plan)

> **声明**：本文档为技术可行性评估与实施规划，**尚未在当前代码库中实施**。当前宿主测试 harness 仅编译连接 `esc_foe.c`，未包含 `esc.c`。

## 1. 背景与目标

当前 `tests/test_soes_real_harness.c` 成功将真实 SOES 源码 `esc_foe.c` 链接至宿主测试环境，验证了真实 FoE 数据流状态转移、短包/零长度路径分歧、文件名匹配与错误码映射。

若希望进一步覆盖以下场景，需要引入 SOES 的核心状态机源文件 `esc.c`：
1. **AL 状态转换拦截**：验证应用钩子在 `BOOT -> INIT` 或 `BOOT -> PREOP` 状态转换被拒绝时的协议栈表现；
2. **0220h 寄存器 (AL Control / AL Status) 联动**：模拟主站写入状态请求以及 Clear-on-Read 寄存器读清特性；
3. **连续请求处理**：在固件传输未完成或完成后，主站下发第二次 INIT 请求时的协议栈复位逻辑。

## 2. 依赖项与硬件 PDI 桩函数分析

`esc.c` 是 SOES 协议栈的核心层，直接与物理 EtherCAT 从站控制器 (ESC) 的过程数据接口 (PDI) 交互。要在宿主单测环境下将其独立编译并运转，必须实现以下底层 PDI 模拟桩：

### 2.1 必需的桩函数与全局变量
| 桩函数 / 变量名称 | 职责与模拟逻辑 |
| :--- | :--- |
| `ESC_read(uint16_t address, void *buf, uint16_t len)` | 模拟从 ESC 寄存器/DPRAM 读取数据（如 AL Control 0x0120、Sync Manager 状态等） |
| `ESC_write(uint16_t address, const void *buf, uint16_t len)` | 模拟向 ESC 寄存器写入数据（如 AL Status 0x0130、AL Status Code 0x0134 等） |
| `ESC_init(...)` | 模拟硬件 PDI 初始化流程 |
| `ESC_reset(...)` | 模拟从站硬件复位脉冲 |
| `ESC_read_alstatus(...)` / `ESC_write_alstatus(...)` | 若协议栈版本封装了专用读写接口，需一并打桩 |

### 2.2 工作量与复杂度预估
- **PDI 模拟层代码量**：预计约为 150 - 250 行 C99 代码，主要用于维护一个模拟的 ESC 寄存器映射表（如 4KB DPRAM 与基础寄存器空间）；
- **中断与调度时序模拟**：需要模拟 PDI 中断与 `ESC_alerror()`、`ESC_state()` 的周期调用循环，涉及约 100 行状态机时钟推进逻辑；
- **总实施工作量**：预估开发与调优时间约为 1.5 - 2 人日。

## 3. 可验证的具体行为与预期收益

若完成上述扩展，可获得以下确凿验证能力：
1. **0x0220 AL Status Code 读清机制**：
   验证主站读取 AL Status Code 寄存器后，SOES 内部错误状态是否按 EtherCAT 规范被正确清除。
2. **BOOT 态状态机迁移阻断**：
   当固件正在写入或固件损坏且尚未成功提交时，应用层钩子返回错误，验证 `esc.c` 是否正确拒绝主站迁出 BOOT 态，并向 AL Status Code 填入正确的错误代码。
3. **二次 INIT 请求的会话复位行为**：
   验证在传输中途主站强制发送 INIT 状态切换时，`esc.c` 对上层 FoE 会话的同步清理通知机制。

## 4. 实施风险与权衡

- **过度模拟风险**：若桩函数行为脱离真实 ASIC/FPGA 硬件（如 ET1100、AX58100）的物理时序，可能导致在 harness 中通过但在真实总线上行为不一致；
- **上游版本依赖**：SOES 不同版本对 `ESC_read`/`ESC_write` 的签名与内部宏定义存在细微差异，可能降低测试 harness 在跨版本间的通用性。

因此，当前阶段维持 `esc_foe.c` 的聚焦测试是轻量且可靠的选择；未来若推进 AL 状态机深度集成，应参考本规划分步推进。
