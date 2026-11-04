# 通用 EtherCAT FoE 升级库开放问题与待定项汇总 (Open Issues)

本文档集中汇总 `docs/architecture/SAD-v0.1.md` 与 `docs/spec/efw-format-v0.1.md` 中所有标记为 **【待验证】**、**【未核实】**、**【待硬件确认】**、**【推断】** 的事项。严格分为三类进行跟踪闭环。

---

## 1. 可查文档解决（芯片手册 / ETG 官方规范）

本类问题通过查阅芯片官方 Datasheet 或 ETG 官方技术规范即可查实闭环：

1. **AL Event Request 寄存器（0220h）清除条件【LAN9252 已核实 / 其他 ESC 待确认】**：
   - 内容：LAN9252 手册 Section 12.14.28（第 253 页）已核实 Bit 0 AL Control Event 明确注明 `This bit is cleared by reading the AL Control Register from PDI.`；ET1100、AX58100 等其他 ESC 控制器的清除条件仍待横向比对确认。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 3.1 节第 5 条。
2. **SII Configuration Area Checksum 存放位置与计算算法【未核实】**：
   - 内容：LAN9252 手册 Section 12.14.49（第 261 页）定义了校验和错误位 Bit 11，但全文未给出 ESC Configuration Area 校验字存放的具体 Word 地址（如 Word 7）以及校验和的具体累加/反转算法。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 4.3 节第 3 条。
3. **SII Bootstrap 邮箱配置段偏移与格式【未核实】**：
   - 内容：ETG.2010 官方规范中关于 Bootstrap 邮箱配置段的字偏移地址（暂标为 0x14-0x17、0x1C bit3）及其标准定义。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 4.3 节第 5 条。
4. **FoE 文件名编码与协议前缀限制【未核实】（长度上限【已核实 SOES】）**：
   - 内容：SOES 协议栈头文件 `soes/esc_foe.h` 明确宏定义 `#define FOE_FN_MAX      31`，文件名长度上限已确定为 31 字节；ETG.1000.6 官方规范对 FoE 文件名字符编码（ASCII / UTF-8）以及官方是否强制特定前缀的约束仍标记为 **【未核实】**。
   - 出处：`docs/spec/efw-format-v0.1.md` 第 7.1 节。
5. **EtherCAT 主站单包响应超时标准数值【未核实】**：
   - 内容：TwinCAT 及标准 EtherCAT 主站对 FoE 单包数据请求与写入响应的默认超时时间阈值。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 3 节时序图及 3.1 节第 3 条。
6. **现场工业网络 FoE 实际传输速率基准【未核实】**：
   - 内容：典型现场工况下，不同网卡与周期配置下的 FoE 实际数据吞吐传输速率范围（用于精确评估环形缓冲队列容量）。
   - 出处：`docs/open-issues.md` 第 4 节第 2 条及 `docs/architecture/SAD-v0.1.md` 第 5.1 节第 14 条。
7. **Flash 擦写导致 CPU 总线挂起（Stall）的 MCU 型号清单【未核实】**：
   - 内容：具体哪些型号的 Cortex-M MCU 在执行片内 Flash 擦写期间会导致 CPU 硬件彻底挂起、无法执行 RAM 中的协议栈代码。
   - 出处：`docs/open-issues.md` 第 4 节第 2 条及 `docs/architecture/SAD-v0.1.md` 第 5.1 节第 14 条。
8. **ESC 错误挂起状态后果推断【推断】**：
   - 内容：依据 LAN9252 手册 Section 12.14.49（第 261 页）仅给出了 EEPROM 加载失败状态位（Bit 11 与 Bit 12），工程推断其无法进入 BOOT 态、网络恢复完全失效。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 4.3 节第 1 条。

---

## 2. 需要 SSC 工程包（Beckhoff SSC 官方源码查实）

本类问题依赖查阅 Beckhoff 官方 Slave Stack Code (SSC) 协议栈源码即可确认：

1. **SSC 侧 FoE 结束判定与回调钩子机制【未核实】**：
   - 内容：SSC 协议栈官方源码在传输结束时的回调通知时机，是否有类似 `FOE_fclose` 的显式结束通知机制。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 1.2 节。
2. **SSC 侧回调粒度与内存聚合机制【未核实】**：
   - 内容：SSC 向上层应用交付数据是单个 FoE 邮箱包粒度还是提供聚合缓冲区。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 1.2 节。
3. **SSC 侧写入路径发 BUSY 与延迟处理支持【未核实】**：
   - 内容：SSC 在从站写入（WRQ / DATA）路径上是否支持向主站发送 `FOE_OP_BUSY` 报文或延迟确认。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 3.3 节。
4. **SSC 协议栈启动时重启接管挂起 BOOT 请求的行为【未核实】**：
   - 内容：APP 收到 INIT→BOOT 请求后跳转进入 Bootloader，新初始化的 SSC 能否在未确认状态下平滑接管并向 AL Status 写入 BOOT 确认。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 2.1 节第 1 条。

---

## 3. 需要真机（示波器测量 / 硬件联调验证）

本类问题必须在目标硬件板卡与物理示波器环境下进行实测验证：

1. **MCU 复位期间 ESC RST# 引脚电平与上拉可靠性【待硬件确认】**：
   - 内容：APP 调用 `NVIC_SystemReset()` 导致 MCU 进入复位期间，MCU GPIO 浮空高阻态下外部 4.7k-10k 上拉能否保证 ESC RST# 全程无低脉冲下冲。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 2.2 节。
2. **MCU 硬件复位全周期以太网 PHY 持续 Link Up 验证【待验证】**：
   - 内容：方案 A 系统复位期间，从站网口 PHY 供电与链路是否持续保持 Link Up，TwinCAT 主站不报掉线。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 2.2 节。
3. **旧板纯软件内存跳转总线状态与中断残留验证【待验证】**：
   - 内容：兼容方案 B 纯软件跳转瞬间，SPI 总线是否干净释放、ESC IRQ 中断引脚是否清零，反复跳转是否无 HardFault 与通信挂死。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 2.3 节。
4. **MCU 系统复位后 ESC 寄存器与邮箱 SM 配置保留情况【待验证】**：
   - 内容：MCU 硬件软复位后，ESC 内部已由主站下发的 SyncManager 配置与状态寄存器能否完整保留、协议栈能否无缝接上。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 2.1 节第 1 条。
5. **Bootloader 启动时主动比对 0120h 与 0130h 接管 BOOT 请求【待验证】**：
   - 内容：实测当 APP 读取 0120h 导致 0220h 事件标志清除后，Bootloader 启动时主动比对寄存器并触发状态机能否稳定进入 BOOT 态。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 3.1 节第 5 条。
6. **升级成功后切回 INIT 触发 MCU 系统软复位生效【已确定，待真机验证】**：
   - 内容：固件升级完成后，主站发起 `BOOT -> INIT` 状态切换能否稳定被 Bootloader 捕获并成功触发 MCU 系统复位启动新固件。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 3 节时序图及 5.1 节第 6 条。
7. **升级后重启走暖切换避免以太网 PHY 掉线【建议，待验证】**：
   - 内容：升级成功后通过专有魔数软复位 MCU、不触碰 ESC 复位 GPIO，实测从站能否在不经历 Link Down 的情况下平滑在 INIT 态重新上线。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 3.1 节第 6 条。
8. **按需覆盖擦除（erase_to）单扇区耗时与单包超时窗口实测【建议，待验证】**：
   - 内容：实测片内 Flash 单扇区擦除耗时是否在 TwinCAT 单包超时阈值内安全通过，不引起通信中断。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 3.1 节第 3 条。
9. **大容量外部 SPI Flash 同步物理回读 CRC32 耗时实测【待验证】**：
   - 内容：对于 1MB-4MB 的外部 SPI Flash，实测在最后一次回调内同步回读并计算 CRC32 的总耗时是否会突破主站单包超时阈值。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 4.2 节第 5 条与第 5.1 节第 13 条。

---

## 4. 协议栈缺陷规避与后续架构增强方案【待决策】

1. **0 长度包分支 `FOE_fclose` 返回值被 SOES 忽略的落地处理【已确定】**：
   - 现状事实：查阅 SOES 源码 `esc_foe.c` 的 0 长度包结束分支，在独立 0 长度包触发完成的分支中，`FOE_fclose()` 的返回值未被检查，SOES 直接向主站回复最终成功 ACK。
   - 触发条件澄清：空包路径出现条件是传输总长度（`header_length + payload_length`）恰为底层 FoE 单包数据容量（`ESC_FOE_DATA_SIZE`）的整数倍，与应用层 `foe_cfg->buffer_size` 无关；TwinCAT 实际分包行为标记为 **【未核实】**。
   - 已确定决策：v1 架构完全接受此原生限制，不打源码补丁。
   - 安全底线：`drv->invalidate()` 已提前清除固件就绪/有效标记，失败时设备重启设计目标为停留在 Bootloader 等待重新升级（取决于 Bootloader 引导前置校验完备性及介质掉电写入原子性）。
   - 缓解措施 1：打包工具新增可选参数 `--foe-packet-size N`，必要时在 payload 末尾填充 `0xFF` 规避空包路径（SII 目标补 2 字节整倍数），填充计入 `payload_length` 与 `payload_crc32`。此为尽力而为缓解，主站实际分包大小标记为 **【未核实】**。
   - 缓解措施 2：适配层记录“最后提交失败”状态，在主站发起 `BOOT -> INIT` 状态切换时通过 `post_state_change_hook`（`esc.c` 状态迁移后置钩子调用点）拒绝迁移（`*an = ESCboot | ESCerror`）并调用 `ESC_ALerror(ALERR_NOVALIDFIRMWARE)`（`0x0014`）设置 AL 状态码拦截。（SSC 侧状态迁移拒绝与设置 AL Status Code 机制标记为 **【未核实】**）。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 1.2 节、第 3.2 节、第 5.1 节第 15 条。

2. **流式环形缓冲队列解耦方案（v1 排除，后续版本待评审）【待决策】**：
   - 机制：在核心协议机与慢速物理介质驱动之间引入 Ring Buffer Queue，由回调快速写入内存队列后立即向主站回复 ACK，慢速物理擦写交由主循环非阻塞异步处理。
   - 依赖与制约：
     - 片内 Flash 擦写时 CPU 总线硬件挂起（Stall）的 MCU 芯片无法使用（CPU 被冻结无法服务通信）；
     - FoE 为单包停等流控，缓冲队列容量必须满足：
       $$\text{Buffer\_Size} \ge \text{最长擦写停顿时间} \times \text{FoE 传输速率}$$
       需要较大 RAM 内存开销。
   - 状态：v1 架构确定不引入，保留在本文档作为后续针对特定非阻塞硬件（如支持 WIP 轮询的外部 SPI Flash）的可选增强方案评审。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 5.1 节第 14 条。
