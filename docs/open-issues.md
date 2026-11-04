# 通用 EtherCAT FoE 升级库开放问题与待定项汇总 (Open Issues)

本文档集中汇总 `docs/architecture/SAD-v0.1.md` 与 `docs/spec/efw-format-v0.1.md` 中所有标记为 **【待验证】**、**【未核实】**、**【待硬件确认】**、**【推断】** 的事项。严格分为三类进行跟踪闭环。

---

## 1. 可查文档解决（芯片手册 / ETG 官方规范）

本类问题通过查阅芯片官方 Datasheet 或 ETG 官方技术规范即可查实闭环：

1. **AL Event Request 寄存器（0220h）清除条件**：
   - 内容：LAN9252 手册 AL Event Request 寄存器（0220h，见 [pdf:PDF_Q6_AL_EVENT_CLEAR_PDI]）Bit 0 AL Control Event 证实由 PDI 读 0120h 自动清除；ET1100、AX58100 等其他 ESC 控制器的清除条件标记为【未核实】。
   - 出处：`docs/architecture/SAD-v0.1.md` 第 3.1 节第 6 条。
2. **SII Configuration Area Checksum 存放位置与计算算法【未核实】**：
   - 内容：LAN9252 手册 EEPROM 控制/状态寄存器【未核实（未纳入自动化核验清单）】定义了校验和错误位 Bit 11，但全文未给出 ESC Configuration Area 校验字存放的具体 Word 地址（如 Word 7）以及校验和的具体累加/反转算法。
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
   - 内容：典型现场工况下，不同网卡与周期配置下的 FoE 实际数据吞吐传输速率范围（用于评估环形缓冲队列容量）。
   - 出处：`docs/open-issues.md` 第 4 节第 2 条及 `docs/architecture/SAD-v0.1.md` 第 5.1 节第 14 条。
7. **Flash 擦写导致 CPU 总线挂起（Stall）的 MCU 型号清单【未核实】**：
   - 内容：具体哪些型号的 Cortex-M MCU 在执行片内 Flash 擦写期间会导致 CPU 硬件挂起、无法执行 RAM 中的协议栈代码。
   - 出处：`docs/open-issues.md` 第 4 节第 2 条及 `docs/architecture/SAD-v0.1.md` 第 5.1 节第 14 条。
8. **ESC 错误挂起状态后果推断【推断】**：
   - 内容：依据 LAN9252 手册 EEPROM 控制/状态寄存器【未核实（未纳入自动化核验清单）】仅给出了 EEPROM 加载失败状态位（Bit 11 与 Bit 12），工程推断其无法进入 BOOT 态、网络恢复完全失效。
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

3. **BOOT→INIT 拦截后主站二次请求处理策略【待决策，不实现】**：
   - 现状事实：当固件升级中途断开、校验失败或未完成闭环时（`ecat_foe_core_upgrade_incomplete()` 为真），适配层后置钩子会拦截主站发起的 `BOOT -> INIT` 状态迁移。
     - **复合迁移码与状态值语义区分**：
       - `*as` 匹配复合迁移码 `ECAT_TRANSITION_BOOT_TO_INIT`（`(ESCinit << 4) | ESCboot = 0x13U`，由 SOES 第 1128 行合成，高 4 位为目标状态 `0x1`，低 4 位为当前状态 `0x3`）；
       - `*an` 改写为 AL 状态值 `ECAT_AL_STATUS_BOOT_ERROR`（`ESCboot | ESCerror = 0x03 | 0x10 = 0x13U`，写入 `0x130` AL 状态寄存器）；
       - 两者数值恰好同为 `0x13`，但前者是状态迁移事件码，后者是带错误标志的物理状态值，语义完全不同。
     - **错误码设定**：适配层实际调用注入的错误回调设置 AL 状态码为 `ECAT_AL_STATUS_CODE_NO_VALID_FW`（`0x0014`，在真实 SOES 中对应 `ESC_ALerror(ALERR_NOVALIDFIRMWARE)`）。
   - 源码依据（`esc.c:1111-1135`）：
<!-- quote-begin: Q10_ESC_C_ALCONTROL_INIT_ERRACK -->
```c
   /* Mask state request bits + Error ACK */
   ac = ESCvar.ALcontrol & ESCREG_AL_STATEMASK;
   as = ESCvar.ALstatus & ESCREG_AL_STATEMASK;
   an = as;
   if (((ac & ESCerror) || (ac == ESCinit)))
   {
      /* if error bit confirmed reset */
      ac &= ESCREG_AL_ERRACKMASK;
      an &= ESCREG_AL_ERRACKMASK;
   }

   /* Error state not acked, leave original */
   if ((an & ESCerror) && ((ac & ESCerror) == 0))
   {
      return;
   }

   /* Mask high bits ALcommand, low bits ALstatus */
   as = (uint8_t)((ac << 4) | (as & 0x0f));

   /* Call post state change hook case it have been configured  */
   if (ESCvar.pre_state_change_hook != NULL)
   {
      ESCvar.pre_state_change_hook (&as, &an);
   }
```
<!-- quote-end: Q10_ESC_C_ALCONTROL_INIT_ERRACK -->
   - SOES 源码机制分析【推断，未验证】：
     - 当主站发送状态切换请求时，SOES 在上述引文第 1115 行判断 `if (((ac & ESCerror) || (ac == ESCinit)))`。若主站请求进入 INIT 状态（`ac == ESCinit`），SOES 将其视同错误确认（Error ACK），在第 1118-1119 行执行 `ac &= ESCREG_AL_ERRACKMASK; an &= ESCREG_AL_ERRACKMASK;`，清除目标状态的错误位（即清除了 AL 状态错误位，但不直接清除 `ESCvar.ALerror` 寄存器变量）。
     - 随后在第 1123 行跳过未确认错误检查，在状态迁移匹配中进入 `case BOOT_TO_INIT:`，执行 `ESC_stopmbx()` 并置 `an = ESCinit`。
     - 此后调用的 `post_state_change_hook`（第 1305 行）若介入拦截，会将 `*an` 改写为 `ESCboot | ESCerror`（`0x13U`）并调用 `ESC_ALerror(ALERR_NOVALIDFIRMWARE)`（`0x0014`）。
     - 在第 1310-1314 行，由于 `an & ESCerror` 保持为真，SOES 不会执行 `ESC_ALerror(ALERR_NONE)` 清除错误码。最终第 1321 行 `ESC_ALstatus(an)` 将带错误位的状态写入硬件寄存器。
   - 时序推演：首次 BOOT→INIT 被拦 → 主站再次请求 INIT → SOES 行为 → 钩子是否再拦【推断，未验证】：
     1. 首次 BOOT→INIT 被拦：固件升级未完成闭环时，主站请求进入 INIT；SOES 准备迁移至 `ESCinit`；适配层 `post_state_change_hook` 拦截，将目标状态改写为 `ESCboot | ESCerror` 并设置 AL 状态码为 `ALERR_NOVALIDFIRMWARE`（`0x0014`）；从站停留在 BOOT+Error 态。
     2. 主站再次请求 INIT：主站向 `ALcontrol` 再次写入 `ESCinit`（`0x01`）。
     3. SOES 行为：SOES 在第 1115 行因 `ac == ESCinit` 成立，执行 `an &= ESCREG_AL_ERRACKMASK` 清除错误位；在第 1123 行跳过检查；因从站当前实际状态仍为 `ESCboot`，再次判断为 `BOOT_TO_INIT` 迁移；再次执行 `ESC_stopmbx()` 并设 `an = ESCinit`。
     4. 钩子是否再拦：执行到第 1305 行 `post_state_change_hook` 时，由于核心机未完成状态由单一真值源 `target_incomplete_mask` 保持，`ecat_foe_core_upgrade_incomplete()` 仍为真，且状态迁移仍为从 BOOT 到 INIT，钩子拦截条件再次满足，再次改写为 `*an = ESCboot | ESCerror` 并设置 `ALERR_NOVALIDFIRMWARE`（`0x0014`）。从站将持续被拦截在 BOOT+Error 态，不会误进入 INIT。
     5. 真实协议栈与主站行为边界说明：上述时序完全属于对 SOES 单一文件源码的静态逻辑推演（【推断，未验证】），尚未在集成真实 SOES 协议栈和物理主站环境下验证过。现场主站（如 TwinCAT）在面对 `0x0014` 错误后的重试逻辑、超时处理以及状态机恢复步骤可能因实现而异，真实硬件行为需以现场动态测试为准。
   - 待决策问题：在主站确认错误（Error Acknowledge）后，若主站未重新发起 FoE 固件写入，而是再次发送 `BOOT -> INIT` 状态切换命令，从站协议栈是否应继续拦截。
   - 候选方案（保留供后续决策，本版本不替用户做选择）：
     - 候选 1：持续拦截。只要固件未成功闭环，后续每次 `BOOT -> INIT` 均拒绝并保持在 BOOT 错误态。优点是避免残缺固件引导；制约是主站若仅需执行只读诊断或网络拓扑复位时可能受阻。
     - 候选 2：仅单次拦截。首次拦截报警后，若主站明确复位错误并二次强制请求迁移，则允许进入 INIT。依赖 Bootloader 本地自检逻辑拦截并阻止运行残缺固件。
     - 候选 3：提供应用层策略决策回调。由上层应用结合是否存在 Golden 双备份镜像等硬件实际情况，动态决定是否放行。
   - 状态：【待决策，不实现】。

4. **FoE 文件名架构待决策【待决策，不实现】**：
   - 现状事实：
     1. SOES 源码事实：SOES 协议栈源码 `esc_foe.c` 第 70 到 73 行（已登记为三方引文 Q1）原生采用 `strncmp(foe_cfg->name, filename, strlen(foe_cfg->name))` 进行静态配置前缀比对，仅支持单个静态配置名。
     2. 缺少 WRQ 钩子：SOES 原生未向外部暴露 WRQ 文件名解析或动态拦截回调。
     3. 宿主机 C3 复现事实：测试 `test_reproduce_soes_data_callback_without_wrq_stream_rejection` 证实，若未经历 WRQ 文件名解析与会话建立，直接由 SOES 驱动 `write_cb` 灌入首块数据，核心机因未建立有效目标与版本上下文直接拒绝数据流（返回 -1，错误码 `ECAT_FOE_CORE_ERR_INVALID_STATE`，适配层返回非零触发 FoE abort）。
   - 候选方案（不替用户做选择，仅做技术分析）：
     - 候选 A：目标独立固定文件名方案。
       - 机制：每个目标分配单一固定文件名（如 MCU 固定为 `firmware.efw`，SII 固定为 `eeprom.efw`），版本号与硬件兼容性完全下沉至 `.efw` 容器内部头结构解析与校验。
       - 依赖与改动范围：无需修改 SOES 源码；需在适配层按目标维护配置。
       - 需要验证的点：是否满足现场运维对文件名直观可见版本的要求。
     - 候选 B：SOES 协议栈新增 WRQ 回调补丁方案。
       - 机制：向 SOES `esc_foe.c` 提交补丁，在 WRQ 阶段增加应用层回调钩子（如 `on_wrq`），动态调用核心机逆向切分解析文件名。
       - 依赖与改动范围：需维护三方开源库源码补丁分支。
       - 需要验证的点：长期跟随上游更新的维护成本与工程合规性。
     - 候选 C：邮箱层 / PDI 派发前置拦截方案。
       - 机制：在硬件邮箱读取或协议栈主循环分发之前，前置嗅探 FoE WRQ 邮箱帧，提取文件名触发核心机建立会话后再交由 SOES 处理后续数据。
       - 依赖与改动范围：改动集中在硬件适配/邮箱分发层，不修改 SOES 内部实现。
       - 需要验证的点：增加邮箱包前置解析耗时与时序同步复杂度。
   - 参考依据：
     - Beckhoff TwinCAT 体系中，`FB_EcFoeWriteFile` 功能块的 `sFoEPathName` 参数专门表示从站侧文件名，独立于 PC 侧本地文件名。
     - 官方参考链接：https://infosys.beckhoff.com/content/1033/tcplclib_tc2_ethercat/16248044683.html
   - 状态：【待决策，不实现】。

5. **升级未闭环状态在 deinit/reinit 与断电重启后的已知局限【已知局限】**：
   - 机制实现：核心机内部通过位掩码（`target_incomplete_mask`）按目标记录固件未完成状态，保证在单次 core 内存生命周期内，只有相同目标成功 finalize 才能解除其未完成状态，防止多目标交叉升级时误放行残缺固件。
   - 内存易失性局限：该位掩码驻留在 RAM 运行态。若应用层主动调用 `ecat_foe_core_deinit` 或重新 `ecat_foe_core_init`，结构体清零导致该状态丢失；若设备发生硬件断电重启，RAM 数据丢失，重新初始化后未完成状态必然归零。
   - 工程边界与防变砖要求：本核心库不变更持久化驱动契约接口，明确记录该局限，不声称在纯内存层解决跨掉电追踪问题。在真实硬件系统中，物理固件有效性必须依赖底层驱动在非易失性存储介质（如 Flash 标志位、校验区）中的真实记录，以及 Bootloader 引导启动时的完整性自检兜底。
