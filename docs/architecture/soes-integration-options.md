# SOES 协议栈接入方案评估与风险说明 (SOES Integration Options & Risks)

本文档针对原生 **SOES (Simple Open EtherCAT Slave)** 协议栈接入通用固件升级核心库进行技术可行性、边界限制、源码证据与风险评估。
**当前采用 SOES 前后置钩子接入；实际版本和硬件行为仍需验证。**

---

## 1. WRQ 文件名匹配机制与局限分析

### 1.1 原生源码逐字核验 (esc_foe.c:70-73)
在本地真实 `/tmp/soes_src/soes/esc_foe.c` 第 70~73 行，原生 SOES 处理 WRQ 文件名匹配的代码逐字如下：

<!-- quote-begin: Q1_ESC_FOE_STRNCMP -->
```c
   for (i = 0; i < foe_cfg->n_files; i++)
   {
      if (0 == strncmp (foe_file_name, foe_cfg->files[i].name, num_chars))
      {
```
<!-- quote-end: Q1_ESC_FOE_STRNCMP -->

*(注：第 72 行源码实为 `0 == strncmp (...)`，此前报告改写为 `strncmp (...) == 0` 属表述不严谨，现已严格纠正为逐字原文)*

### 1.2 匹配机理与潜在隐患分析 (静态推断与动态验证界定)
以下分析基于对 SOES 原生源码第 70~73 行的**静态代码推断 (Static Code Inspection)** 与宿主测试 Harness 动态验证，尚未在物理从站硬件与主站环境下完成全流程动态验证：
1. **由接收长度决定的前缀匹配**：
   - 比较长度参数为 `num_chars`（即主站发送的文件名长度）。这属于前缀匹配，非严格的全名比对；
2. **【静态推断】空文件名命中首条配置隐患**：
   - 当主站发来的 WRQ 报文未携带文件名或文件名长度为 0 时，`num_chars == 0`；
   - 根据 C99 标准，`strncmp(s1, s2, 0)` 恒等于 0，`0 == 0` 条件在循环的第 1 次迭代（`i = 0`）即无条件成立；
   - **推断后果**：主站发出的任何空文件名写请求，都会被原生 SOES 默认误路由并绑定到首条文件配置（通常为 `files[0]` 对应的 MCU 固件）；此现象属于静态逻辑推断，【待真机动态验证】；
3. **【宿主实测已证实】短前缀误绑定多目标隐患**：
   - 若从站配置中包含多个文件表项（如 `files[0].name = "SERVO_DRV_MCU.efw"`，`files[1].name = "SERVO_DRV_FPGA.efw"`）；
   - 若主站下发了通用前缀（如 `"SERVO_DRV"`，`num_chars == 9`），`strncmp` 将在前 9 个字节匹配成功，直接命中 `files[0]`；
   - 即使用户本意是升级 FPGA，若前缀通用，也会被绑定到首个匹配项上。**该行为已在宿主测试 Harness（`test_runner_soes_real` Harness Test 2）中完成动态实测验证**，证实短前缀匹配特性确实存在；物理从站与真实主站环境仍待实测；
4. **动态版本文件名被阻断**：
   - 若配置表中为基准名（长度 17），主站按规范下发带动态版本号的文件名（如 `"SERVO_DRV_MCU_V1.0.0.0.efw"`，长度 26）；
   - 由于 `num_chars == 26`，`strncmp` 将比较 26 字节，导致比对越界或不相等，原生 SOES 直接返回 `FOE_ERR_NOTFOUND (0x8001)`。

### 1.3 候选方案对比与现行实现
| 方案 | 具体做法 | 预期优势 | 局限与待核实事项 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| **方案 A<br>(固定目标文件名)** | 协议约定主站升级时下发固定文件名（如 `SERVO_DRV_MCU.efw`）。硬件 ID、工程 ID、目标 ID 及版本号完全由当前固定 148 字节的 `.efw` 文件头承载并校验。 | 利用原生 SOES 静态表项机制，从站无需打补丁。 | - 主站无法从文件名直观获知版本；<br>- **【未核实】** 是否与所有主站工具和外部协议栈完全兼容尚无充分实测证据。 | 备选方案 |
| **方案 B<br>(SOES 栈打补丁)** | 修改 SOES `esc_foe.c`，在 WRQ 入口增加用户动态路由钩子：<br>`int (*foe_wrq_hook)(const char *name, size_t len)`。 | 支持从文件名动态提取版本与目标，与现有规范严格一致。 | 侵入第三方协议栈源码，增加维护与版本升级负担。 | 备选方案 |
| **方案 C<br>(前后置邮箱钩子，现行实现)** | 前置钩子嗅探 WRQ 并做只读预检；原生 SOES 处理后，由后置钩子核验输出槽位并就地将异常中和为标准 0x8000 ERR、保持信箱计数并注入复位自愈。 | 零侵入第三方协议栈源码，支持动态文件名解析，保障主站可观察一致失败。 | 严格要求单执行流串行调用，禁止并发与插入状态迁移；已在宿主完成全套验证，真实板卡待验。 | **已在宿主环境实现并跑通验证 (当前主线)** |

---

## 2. 零长度包错误被忽略与防砖可靠性边界

### 2.1 原生源码逐字核验 (esc_foe.c:459-465 与 493-506)
在 `/tmp/soes_src/soes/esc_foe.c` 中：

<!-- quote-begin: Q2_ESC_FOE_ZERO_LEN_CLOSE -->
```c
   else if (data_len == 0)
   {
      DPRINT("FOE_data completed\n");
      FOE_fclose ();
      res = FOE_send_ack ();
      FOE_init ();
   }
```
<!-- quote-end: Q2_ESC_FOE_ZERO_LEN_CLOSE -->

对比第 493~506 行短包分支：

<!-- quote-begin: Q3_ESC_FOE_SHORT_PACKET_CLOSE -->
```c
      else
      {
         if ((ncopied != data_len) || FOE_fclose ())
         {
            DPRINT("FOE_fclose failed to write extra buffer\n");
            FOE_abort (FOE_ERR_PROGERROR);
         }
         else
         {
            DPRINT("FOE_data completed\n");
            res = FOE_send_ack ();
            FOE_init ();
         }
      }
```
<!-- quote-end: Q3_ESC_FOE_SHORT_PACKET_CLOSE -->

- **源码事实与宿主实测**：在 `data_len == 0` 分支，`FOE_fclose()` 的返回值确实被直接丢弃，即使底层提交返回错误，SOES 依然会发出成功 ACK 并重置状态。该行为已在宿主独立测试 Harness（`test_runner_soes_real` Harness Test 3）中注入失败完成实测复现验证。

### 2.2 防砖能力与可靠性客观边界
此前文档中关于“不会变砖”、“必然无法启动”等断言不够客观严谨。设备在异常中断或校验失败后能否可靠停留在 Bootloader，**高度依赖以下系统与工程边界条件，不存在无条件保证**：
1. **驱动有效标记的非易失性持久化**：
   - 核心机在解完固件头后调用 `invalidate()`，要求清除固件有效标记；
   - **关键依赖**：此操作必须在非易失介质（Flash 扇区或特定标志位）上实现掉电原子性持久化。若硬件仅在 RAM 中修改标记、或标记擦写期间发生断电造成状态未定义，设备重启后可能误跳入损坏固件；
2. **Bootloader 启动逻辑引导契约**：
   - 依赖物理 Bootloader 在系统上电复位时，严格先行检查该有效标记。若标记非法或已擦除，才强制停留在 Bootloader 等待重新升级；
3. **防回滚原子提交的板级验证边界**：
   - 防回滚模式必须实现 `finalize_with_version_floor()`，把镜像激活与下限更新作为断电可恢复事务；事务失败时核心调用 `abort()` 且不得激活候选镜像；
   - 当前宿主测试只验证模拟事务的调用与失败语义，未证明具体板级实现的掉电安全。目标 Bootloader 必须在上板前补齐事务日志/恢复逻辑并验证复位路径。

---

## 3. 关闭 RRQ 的真实编译方式与缓冲区越界风险

### 3.1 预编译宏机制核查
原生 SOES 采用 `#ifdef FOE_READ_SUPPORTED` 控制读逻辑编译。
- **未定义（唯一正确关闭方式）**：不定义 `FOE_READ_SUPPORTED`，代码中无 RRQ 分支，收到 RRQ 报文落入 `default:` 分支直接返回 `0x8000 (FOE_ERR_NOTDEFINED)`；
- **【不可定义为 0】定义为 0 的配置陷阱**：若传入 `-DFOE_READ_SUPPORTED=0`，因 C 预处理器 `#ifdef` 仅判定宏是否存在，宏依然生效，会错误编译包含 RRQ 逻辑。

### 3.2 启用 RRQ 时的越界读 (Buffer Over-read) 风险分析
在 `esc_foe.c` 中：
- 第 94 行：`FOEvar.fend = foe_cfg->files[i].max_data;`
- 第 125~130 行：
```c
   while (maxlength && (FOEvar.fend - FOEvar.fposition))
   {
      maxlength--;
      *(data++) = foe_cfg->fbuffer[FOEvar.fposition++];
      ncopied++;
   }
```
- **物理风险**：`foe_cfg->files[i].max_data` 代表该文件的标称最大字节数，而 `foe_cfg->fbuffer` 为底层连续内存缓冲指针。
  如果应用层配置了较大的 `max_data`（如 64KB），但为了节省从站 RAM，`fbuffer` 分配的实际内存较小（如 4KB）：
  一旦主站发起 RRQ 读取，`FOEvar.fend` 被设置为 64KB，`FOE_fread` 将持续从 `fbuffer` 线性递增读取，直接导致**缓冲区越界读 (Buffer Over-read)**，可能泄露内存敏感信息或引发 MCU 总线 HardFault 异常。
- **结论**：固件升级从站若无需提供回读，必须保持 `FOE_READ_SUPPORTED` 为**完全未定义**状态。

---

## 4. BOOT 状态机钩子、时序与错误码桥接

### 4.1 SOES 钩子定义与 esc.c 调用时序逐行证据
在 `/tmp/soes_src/soes/esc.h` 第 317~318 行与第 712 行：

<!-- quote-begin: Q4_ESC_H_STATE_HOOKS -->
```c
   void (*pre_state_change_hook) (uint8_t * as, uint8_t * an);
   void (*post_state_change_hook) (uint8_t * as, uint8_t * an);
```
<!-- quote-end: Q4_ESC_H_STATE_HOOKS -->

<!-- quote-begin: Q5_ESC_H_ALERROR_FUNC -->
```c
void ESC_ALerror (uint16_t errornumber);
```
<!-- quote-end: Q5_ESC_H_ALERROR_FUNC -->

在 `/tmp/soes_src/soes/esc.c` 中：

1. **迁移码合成与 pre 钩子** (第 1128~1135 行)：

<!-- quote-begin: Q6_ESC_C_PRE_HOOK_INVOCATION -->
```c
   /* Mask high bits ALcommand, low bits ALstatus */
   as = (uint8_t)((ac << 4) | (as & 0x0f));

   /* Call post state change hook case it have been configured  */
   if (ESCvar.pre_state_change_hook != NULL)
   {
      ESCvar.pre_state_change_hook (&as, &an);
   }
```
<!-- quote-end: Q6_ESC_C_PRE_HOOK_INVOCATION -->

   - `as` 的语义：传入前高 4 位为请求的 `ALcontrol` 命令，低 4 位为当前 `ALstatus`。对于 `BOOT -> INIT` 迁移，`ac = 0x01 (ESCinit)`，`as & 0x0f = 0x03 (ESCboot)`，组合值为 `0x13`；
   - `an` 的语义：目标状态变量指针（初始与当前状态相同）。

2. **状态迁移处理与邮箱关闭时序** (第 1191~1196 行)：

<!-- quote-begin: Q7_ESC_C_BOOT_TO_INIT -->
```c
      case BOOT_TO_INIT:
      {
         ESC_stopmbx ();
         an = ESCinit;
         break;
      }
```
<!-- quote-end: Q7_ESC_C_BOOT_TO_INIT -->

   - **关键时序事实**：当 `as == BOOT_TO_INIT` 时，`ESC_stopmbx()` 在此处**直接执行，从站邮箱已被停用关闭**。

3. **post 钩子调用点** (第 1304~1308 行)：

<!-- quote-begin: Q8_ESC_C_POST_HOOK_INVOCATION -->
```c
   /* Call post state change hook case it have been configured  */
   if (ESCvar.post_state_change_hook != NULL)
   {
      ESCvar.post_state_change_hook (&as, &an);
   }
```
<!-- quote-end: Q8_ESC_C_POST_HOOK_INVOCATION -->

   - **执行时机**：`post_state_change_hook` 在 `switch(as)` 退出后被调用。
   - **技术影响分析**：当进入 post 钩子时，`ESC_stopmbx()` 已经完成，邮箱已被停用。若在此处将 `*an` 改写回 `ESCboot | ESCerror` 并调用 `ESC_ALerror(ALERR_NOVALIDFIRMWARE)`，虽然可以在 AL 状态寄存器中呈现错误并阻断进入 INIT，但**邮箱已停，主站无法在该状态下通过邮箱继续交互，必须先重新走状态机恢复流程**。

4. **AL Event 检查与提前退出机制** (第 1099~1110 行)：

<!-- quote-begin: Q9_ESC_C_ESC_STATE_ALEVENT_CHECK -->
```c
   /* Do we have a state change request pending */
   if (ESCvar.ALevent & ESCREG_ALEVENT_CONTROL)
   {
      ESC_read (ESCREG_ALCONTROL, (void *) &ESCvar.ALcontrol,
                sizeof (ESCvar.ALcontrol));
      ESCvar.ALcontrol = etohs (ESCvar.ALcontrol);
   }
   else
   {
      /* nothing to do */
      return;
   }
```
<!-- quote-end: Q9_ESC_C_ESC_STATE_ALEVENT_CHECK -->

   - **时序与机理分析**：`ESC_state()` 首先检查 `ESCvar.ALevent & ESCREG_ALEVENT_CONTROL`（0220h 寄存器 Bit 0）。若该事件位未置位，函数直接 `return;` 退出，不会主动读取 `ESCREG_ALCONTROL` (0120h)。
   - **与硬件行为联动**：依据 LAN9252 手册 [pdf:PDF_Q6_AL_EVENT_CLEAR_PDI]，PDI 读取 0120h 会自动清除 0220h Bit 0。若先前 APP 读取了 0120h，新启动的协议栈如果仅依赖 `ESCvar.ALevent` 轮询，将直接命中 `else return;` 无法进入状态迁移逻辑。

### 4.2 现有实现状态界定 (区分宿主验证与物理硬件)
- **适配层实现状态**：适配层 `src/adapters/ecat_foe_soes.c` 已完整实现两组机制：
  1. **状态机钩子**：实现了 `ecat_foe_soes_post_state_hook`，在 `*as == 0x13U` 且核心机未正常完成 finalize（或处于未闭环活跃会话/错误态）时改写 `*an = 0x13U` (BOOT|Error) 并通过注入的 `al_error_fn` 回调上报 `0x0014U` (ALERR_NOVALIDFIRMWARE)；
  2. **前后置邮箱钩子架构 (方案 C)**：实现了 `ecat_foe_soes_pre_process_hook` 与 `ecat_foe_soes_post_process_hook`，支持前置嗅探暂存 WRQ、后置核验响应并将异常就地中和为标准 0x8000 ERR、保持原有信箱头计数器并注入 `FOE_init` 复位自愈；
- **宿主验证与真机边界**：
  - **宿主环境已验证**：适配层已在宿主测试 Harness（`tests/test_soes_real_harness.c`）中直接编译链接真实 SOES `esc.c` 与 `esc_foe.c` 源码及硬件端口桩，完成 11 项核心协议行为测试与 9 大深度风险验证，并在 `examples/bootloader_soes/` 中跑通 5 大场景最小 Bootloader 集成测试；
  - **物理硬件待验证**：生产环境真实 MCU/ESC 物理芯片与现场真实主站动态联调尚未开展，明确处于【属于宿主环境仿真，物理硬件待真机验证】状态。真实硬件环境下由于邮箱已在 switch 内部通过 `ESC_stopmbx()` 停止，主站无法在该状态下通过 FoE 邮箱继续交互，必须先按 EtherCAT 规范走状态机复位恢复流程。

---

## 5. 源码引用自动化校验清单 (Registered Quotes)

为了保证本文档引用的每一处 SOES 源码真实可靠，项目提供了三方校验脚本 `tools/verify_soes_quotes.py` 与外部清单 `docs/quotes_manifest.json`，针对以下明确登记的源码条目进行全量行号与文本三方比对：

| 引用编号 | 文件路径 | 行号范围 | 校验内容描述 |
| :--- | :--- | :--- | :--- |
| `Q1_ESC_FOE_STRNCMP` | `esc_foe.c` | 70 - 73 | WRQ 文件名匹配判定 (`0 == strncmp`) |
| `Q2_ESC_FOE_ZERO_LEN_CLOSE` | `esc_foe.c` | 459 - 465 | 0 长度包结束分支 (`FOE_fclose` 返回值被忽略) |
| `Q3_ESC_FOE_SHORT_PACKET_CLOSE` | `esc_foe.c` | 493 - 506 | 短包结束分支 (`FOE_fclose` 严格返回值检查) |
| `Q4_ESC_H_STATE_HOOKS` | `esc.h` | 317 - 318 | 状态迁移钩子函数指针定义 (双指针参数) |
| `Q5_ESC_H_ALERROR_FUNC` | `esc.h` | 712 - 712 | `ESC_ALerror` 错误码设置函数声明 |
| `Q6_ESC_C_PRE_HOOK_INVOCATION` | `esc.c` | 1128 - 1135 | `pre_state_change_hook` 调用与迁移码合成时机 |
| `Q7_ESC_C_BOOT_TO_INIT` | `esc.c` | 1191 - 1196 | `BOOT_TO_INIT` 分支执行 `ESC_stopmbx` 时机 |
| `Q8_ESC_C_POST_HOOK_INVOCATION` | `esc.c` | 1304 - 1308 | `post_state_change_hook` 调用点位于停邮箱之后 |
| `Q9_ESC_C_ESC_STATE_ALEVENT_CHECK` | `esc.c` | 1099 - 1110 | `ESC_state()` 检查 ALevent CONTROL 事件标志与提前退出 |

*说明：校验脚本通过读取 `docs/quotes_manifest.json`，同时提取本文档中的 `<!-- quote-begin: <ID> -->` 块与本地 SOES 源码对应行进行三方严格核验。本工具仅对上述明确登记的 9 处引用条目作出真实性承诺，不涵盖未登记引文。*

---

## 6. 全局行号与外部规范引用盘点表 (Reference Inventory)

为落实严谨客观的工程标准，本项目对文档中所有涉及具体行号或外部规范的出处引用进行全量盘点，明确区分【本地自动化三方核验】、【本地人工比对核验】与【本地无源文件 / 未核验】：

### 6.1 SOES 协议栈源码引用 (本地 `/tmp/soes_src/soes/`)

**本地源文件基线与 SHA-256 (防版本漂移与基线锚定)**：
- 来源路径: 本地散文件目录 `/tmp/soes_src/soes/`
- 版本元数据: 原目录无 `.git`；上游提交 `6ef7b9415e936c061ad30626ca00004bdde2ac62` 的以下四个文件哈希与本地基线一致，不能由此推断原始目录的完整来源。
- 散文件基线 SHA-256：
  - `esc.c`: `1ef43fde2f2cf3306d16e8f8e3e86a1be20a013b0bffd833711749575f60c42c`
  - `esc.h`: `9c76c2d4a20b4f827539a4416383354cb45e95a9d086f444a26a0bbb5ff9a579`
  - `esc_foe.c`: `cd1709f103e5c4b786fb0be2857760f0e180c9123a553a2efa410accbf17a2b7`
  - `esc_foe.h`: `84f5011c258d1e25d8d9ab22bfb74f4ab45b1e6b01f3a6be13e5cf2138034eb7`
*注意：上述 SHA-256 仅代表当前本地散文件环境的比对基线，绝不能当作上游官方 Release 认证。*

| 登记条目与源码归属 | 校验内容或用途 | 核验模式与状态 |
| :--- | :--- | :--- |
| Quote `Q1` (`esc_foe.c`) | WRQ 文件名匹配判定 (`0 == strncmp`) | **【自动三方逐行严格核验 (Q1)】** |
| Quote `Q2` (`esc_foe.c`) | 0 长度包结束分支 (`FOE_fclose` 返回值被忽略) | **【自动三方逐行严格核验 (Q2)】** |
| Quote `Q3` (`esc_foe.c`) | 短包结束分支 (`FOE_fclose` 严格返回值检查) | **【自动三方逐行严格核验 (Q3)】** |
| Quote `Q4` (`esc.h`) | `pre/post_state_change_hook` 钩子函数指针声明 | **【自动三方逐行严格核验 (Q4)】** |
| Quote `Q5` (`esc.h`) | `ESC_ALerror` 错误码设置函数原型声明 | **【自动三方逐行严格核验 (Q5)】** |
| Quote `Q6` (`esc.c`) | `pre_state_change_hook` 调用时机与迁移码合成 | **【自动三方逐行严格核验 (Q6)】** |
| Quote `Q7` (`esc.c`) | `BOOT_TO_INIT` 停用邮箱及赋值 `an = ESCinit` | **【自动三方逐行严格核验 (Q7)】** |
| Quote `Q8` (`esc.c`) | `post_state_change_hook` 触发时机（停邮箱之后） | **【自动三方逐行严格核验 (Q8)】** |
| `esc_foe.h` 文件名长度宏 | `#define FOE_FN_MAX 31` 文件名长度上限 | **【本地人工核对】** 对照本地头文件确认 |
| `esc_foe.c` 缓冲区累加逻辑 | 接收缓冲区满调用 `write_function` 及地址累加 | **【本地人工核对】** 对照本地源文件确认 |
| `esc_foe.c` 文件关闭余量逻辑 | `FOE_fclose` 关闭文件时写入剩余缓冲区 | **【本地人工核对】** 对照本地源文件确认 |
| `esc.c` Bootstrap 邮箱启动分支 | `INIT_TO_BOOT` 与 `BOOT_TO_BOOT` 分支启动 Bootstrap 邮箱 | **【本地人工核对】** 对照本地源文件确认 |
| `esc.c` 非 INIT 切入 BOOT 分支 | 非 INIT 状态切入 BOOT 直接报错设置 | **【本地人工核对】** 对照本地源文件确认 |

### 6.2 Microchip LAN9252 数据手册引用

**官方数据手册下载与核验实况 (`/tmp/vendor_docs/`)**：
- 官方来源: Microchip 官网 `https://ww1.microchip.com/downloads/en/DeviceDoc/00001909A.pdf`
- 本地下载文件: `/tmp/vendor_docs/00001909A.pdf` (SHA-256: `f9b73097c21a1ad5e9b1a23ba10002c6bbeb7836422376c646e84313294e96c6`)
- 核验比对说明：本表对照 Microchip 官方原版 DS00001909A PDF 进行自动化与人工盘点，明确区分 PDF 阅读器物理页码与手册底部印刷页码。目前仅第 Q1 至 Q6 项纳入自动化清单并经由 `tools/verify_pdf_quotes.py` 工具完成逐页文本与章节归属严格核验；其余条目尚未纳入自动化核验清单，客观如实标为【未核实】，绝不作无证据推断或宣称全量已验证。

<!-- pdf-table-begin -->
| 官方章节 (Section) / 寄存器名称 | PDF 物理页 | 底部印刷页 | 英文逐字原文 (English Verbatim Text) | 自动化核验状态与依据 |
| :--- | :--- | :--- | :--- | :--- |
| Table 6-1 (复位源与功能影响表) | Page 38 | Page 38 | `RESET SOURCES AND AFFECTED DEVICE FUNCTIONALITY` (`EEPROM Loader Run`) | **【PDF 工具逐页与章节归属核验通过】** (Q1) |
| Section 6.2.1.2 (RST# 引脚复位) | Page 39 | Page 39 | `6.2.1.2 RST# Pin Reset` (`Driving the RST# input pin low initiates a chip-level reset.`) | **【PDF 工具逐页与章节归属核验通过】** (Q2) |
| Section 12.14.20 (AL Control 寄存器) | Page 239 | Page 239 | `12.14.20 AL CONTROL REGISTER` (Offset: `0120h-0121h`) | **【PDF 工具逐页与章节归属核验通过】** (Q3) |
| Section 12.14.21 (AL Status 寄存器) | Page 240 | Page 240 | `12.14.21 AL STATUS REGISTER` (Offset: `0130h-0131h`) | **【PDF 工具逐页与章节归属核验通过】** (Q4) |
| Section 12.14.22 (AL Status Code 寄存器) | Page 240 | Page 240 | `12.14.22 AL STATUS CODE REGISTER` (Offset: `0134h-0135h`) | **【PDF 工具逐页与章节归属核验通过】** (Q5) |
| Section 12.14.34 (AL Event Request 寄存器) | Page 253 (标题在 252 页) | Page 253 (标题在 252 页) | `This bit is cleared by reading the AL Control Register from PDI.` | **【PDF 工具逐页与章节归属核验通过】** (Q6) |
| Note 11 (EEPROM 配置默认值说明) | - | - | `The default value of this field can be configured via EEPROM.` | 未核实 |
| EEPROM CONTROL/STATUS REGISTER (Offset: `0500h-0501h`) | - | - | `EEPROM CONTROL/STATUS REGISTER` | 未核实 |
| EEPROM CONTROL/STATUS (Bit 15: Busy, Offset: `0500h-0501h`) | - | - | `Busy` | 未核实 |
| EEPROM CONTROL/STATUS (Bit 14: Error Write Enable, Offset: `0500h-0501h`) | - | - | `Error Write Enable` | 未核实 |
| EEPROM CONTROL/STATUS (Bit 13: Error Acknowledge/Command, Offset: `0500h-0501h`) | - | - | `Error Acknowledge/Command` | 未核实 |
| EEPROM CONTROL/STATUS (Bit 12: EEPROM Loading Status, Offset: `0500h-0501h`) | - | - | `EEPROM Loading Status` | 未核实 |
| EEPROM CONTROL/STATUS (Bit 11: Checksum Error in ESC Configuration Area, Offset: `0500h-0501h`) | - | - | `Checksum Error in ESC Configuration Area` | 未核实 |
| EEPROM CONTROL/STATUS (Bits 10:8: Command Register, Offset: `0500h-0501h`) | - | - | `Command Register` (Write 010, Reload 100) | 未核实 |
| EEPROM ADDRESS REGISTER (Offset: `0502h-0503h`) | - | - | `EEPROM ADDRESS REGISTER` | 未核实 |
| EEPROM DATA REGISTER (Offset: `0504h-0507h`) | - | - | `EEPROM DATA REGISTER` | 未核实 |
| Note 27 (EEPROM 数据默认值说明) | - | - | `The default value of this field can be configured via EEPROM.` | 未核实 |
<!-- pdf-table-end -->

### 6.3 Beckhoff ET9300 应用笔记引用 (`an_et9300.txt`)

**交接文档下载链接与本机实测核查说明**：
- 交接文档官方链接 1 (v1i10): `https://download.beckhoff.com/download/document/io/ethercat-development-products/an_et9300_v1i10.pdf`
  - 本机实测结果: `curl -sIL --max-time 15` 返回 `HTTP/2 403 Forbidden` (CloudFront 错误页面，HTML 919 字节)；
  - 浏览器访问实况: 浏览器匿名访问未核实 (受本地自动化脚本执行环境限制，未在真实图形浏览器中核验匿名访问情况)；
- 交接文档官方链接 2 (v1i8): `https://download.beckhoff.com/download/document/io/ethercat-development-products/an_et9300_v1i8.pdf`
  - 本机实测结果: `curl -sIL --max-time 15` 返回 `HTTP/2 403 Forbidden` (CloudFront 错误页面，HTML 919 字节)；
  - 浏览器访问实况: 浏览器匿名访问未核实；
- 状态判定: 本地无源文件，未核验。此前关于“会员受控/不开放匿名”的描述缺乏直接证据，予以更正。当前严格保留为未核验状态，绝不伪造核验结论。

| 文档出处 | 章节与描述内容 | 核验状态与说明 |
| :--- | :--- | :--- |
| 历史记录行号 | 第 15 章 `BOOTSTRAPMODE_SUPPORTED` 宏开关说明 | **【本地无源文件 / 未核验】** (本机 curl 报 403，浏览器匿名未核实，本地无源文件) |
| 历史记录行号 | 第 15 章 `Which mechanism is implement is up to the slave vendor.` | **【本地无源文件 / 未核验】** (本机 curl 报 403，浏览器匿名未核实，本地无源文件) |

### 6.4 ETG 官方规范引用
| 规范编号 | 引用主题 | 核验状态与说明 |
| :--- | :--- | :--- |
| `ETG.1000.6` | EtherCAT 应用层服务规范 (FoE 传输机制) | **【本地无源文件 / 未核验】** (外部标准，本地无官方 PDF) |
| `ETG.2010` | EtherCAT 从站架构与 ID 识别要求 | **【本地无源文件 / 未核验】** (外部标准，本地无官方 PDF) |

### 6.5 嵌入式 MCU 资源占用与硬件局限
- **MCU 足迹未测**：当前代码仅在 macOS 宿主机通过 Clang 针对宿主环境构建并进行单元测试；未在嵌入式交叉编译器（如 `arm-none-eabi-gcc`）下进行目标固件编译与尺寸测量。核心机与适配层的实际 Flash 代码体积、常量占用、静态 RAM 与运行时最大调用栈深度均处于未测状态，明确标记为“MCU 足迹未测”。

---

## 7. 真实 SOES 源码验证与宿主测试 Harness (Real SOES Verification)

本项目遵循“不凭空猜测、以真实代码为准”的工程原则，针对本地真实 SOES 协议栈源文件进行了仅编译验证、源码关键逻辑核对，并构建了宿主隔离测试 Harness。

### 7.1 源码版本基线与仅编译核验 (Compile-only Verification)

- **源码路径与版本**：
  - 本地源文件：`/tmp/soes_src/soes/esc_foe.c`
  - 源文件 SHA-256：`cd1709f103e5c4b786fb0be2857760f0e180c9123a553a2efa410accbf17a2b7`
  - 版本元数据：原本地散文件无 Git 元数据；上游提交 `6ef7b9415e936c061ad30626ca00004bdde2ac62` 的四个关键文件哈希匹配，不代表原目录其余内容来源已知。
- **裸编译核验 (无 BSP 桩)**：
  - 编译命令：`clang -c /tmp/soes_src/soes/esc_foe.c -o /tmp/esc_foe.o`
  - 执行结果：退出码 `rc=1`，报错为缺失平台头文件 `fatal error: 'cc.h' file not found`。证实 SOES 源码依赖外部 BSP 平台定义（如大小端、数据类型对齐宏）。
- **宿主 BSP 适配编译核验**：
  - 编译器与选项：引入项目轻量宿主 BSP 桩目录 `tests/soes_bsp/`，提供最小化 `cc.h`、`options.h` 与 `esc_coe.h`。因 SOES 源码包含 C99 GNU 扩展（如零长数组、匿名联合与无原型函数），需指定相应兼容选项：
    `clang -Wall -Wextra -Werror -std=c99 -Wno-zero-length-array -Wno-c11-extensions -Wno-strict-prototypes -Itests/soes_bsp -I/tmp/soes_src/soes -c /tmp/soes_src/soes/esc_foe.c -o /tmp/esc_foe.o`
  - 执行结果：退出码 `rc=0`，编译成功。
- **目标文件未定义符号核对 (`nm -u`)**：
  - 符号清单：`_ESC_claimbuffer`、`_ESCvar`、`_MBX`、`_MBXcontrol`、`_strncmp`。
  - 依赖定性：`_strncmp` 为 C 标准库函数；其余 4 个符号为 SOES 协议栈主模块（`esc.c`）和邮箱通信原语，表明 `esc_foe.c` 必须与 `esc.c` 的邮箱缓冲区及全局状态结构协同链接。

### 7.2 真实源码关键逻辑核对与代码行证据 (Code Evidence)

针对协议栈在实际升级交互中的关键行为，对照本地真实源码进行了逐项核查与事实确认：

1. **首块回调时机 (WRQ vs DATA)**：
   - **源码事实**：在 `FOE_write` 函数中，SOES 处理来自主站的 WRQ 写请求。其逻辑仅解析请求的文件名与密码，调用 `FOE_fopen` 校验文件存在性并回送 ACK(0)，将状态置为 `FOE_WAIT_FOR_DATA`；**WRQ 阶段完全不调用应用注册的 `write_function` 回调**。
   - **数据流动**：实际首块固件数据回调发生在后续收到首个 DATA 报文并累积到缓冲区满（`FOE_fwrite`）或文件传输结束时（`FOE_fclose`）。这意味着适配层必须等到首个 DATA 块到达时方可向底层核心机喂入实际数据流。

2. **文件名前缀匹配隐患 (Prefix Matching)**：
   - **源码事实**：已在清单登记条目 Quote `Q1_ESC_FOE_STRNCMP` 中明确证实，`FOE_fopen` 遍历配置列表时执行 `0 == strncmp(foe_file_name, foe_cfg->files[i].name, num_chars)`。
   - **缺陷表现**：比较长度 `num_chars` 严格取自主站请求报文中的文件名长度，未检查配置项文件名的完整长度。例如：主站请求短前缀 `"firmware"` 时，能够错误匹配上系统配置的 `"firmware_mcu.bin"`，导致非法短名称逃逸校验。

3. **零长度包返回值处理不对称性 (Zero-length Packet Close)**：
   - **源码事实**：对比已在清单登记的 Quote `Q2_ESC_FOE_ZERO_LEN_CLOSE` 与 Quote `Q3_ESC_FOE_SHORT_PACKET_CLOSE`：
     - 在短包分支（Quote `Q3`）中，源码对 `FOE_fclose()` 的返回值进行了严格检查，失败时会中止传输并回送错误码；
     - 但在 0 长度包结束分支（Quote `Q2`）中，源码直接调用 `FOE_fclose();`，**完全忽略其返回值**，随后无条件发送 ACK 并初始化完成。
   - **原生风险与项目处理**：零长度结束时原生 SOES 会在失败后排队 ACK；本项目后置钩子在核心失败时将其改为 FoE 错误 `0x8000`。完整钩子链路已在宿主测试，真实硬件与主站仍待验证。

### 7.3 宿主测试 Harness 设计与运行验证 (Host Harness)

为在无实体从站硬件的环境下实证上述 SOES 行为及其与核心状态机、前后置钩子的交互，本项目在 `tests/test_soes_real_harness.c` 中构建了宿主测试套件，直接编译链接真实 SOES `esc.c` 与 `esc_foe.c` 源码及硬件端口桩，覆盖基于端口桩的邮箱发送与多槽轮转路径：

- **测试设计范围**：
  1. **11 组核心协议行为测试**：
     - WRQ 仅匹配文件名并回送 ACK，不触发写数据回调；
     - 前缀匹配旁路现象（短前缀误放行，未知文件名中止）；
     - 零长度包与短包分支返回值处理不对称性；
     - 孤立直接 API 调用防御；
     - 真实协议机驱动端到端成功与失败流联动；
  2. **9 大深度风险实证与消除 (风险 1~9)**：
     - 风险 1: 密码拒绝与后置核验隔离；
     - 风险 2: 无可用输出邮箱时的安全失败；
     - 风险 3: 延迟响应与旧 ACK(0) 误激活防御；
     - 风险 4: 稀疏配置下的目标 ID 与文件表映射；
     - 风险 5: 真实发送链路端到端与信箱所有权保持；
     - 风险 6: 输出邮箱新鲜度与槽位上下文隔离；
     - 风险 7: 信箱计数器高 4 位保持（`mbxcnt` 不被篡改清零）；
     - 风险 8: `FOE_WAIT_FOR_DATA` 态自愈恢复与重试；
     - 风险 9: 多槽轮转隔离与真实硬件端口桩收发。
- **运行验证结果**：
  - 测试二进制 `test_runner_soes_real` 由 `Makefile` 的 `test-soes` 目标构建和运行；`test-asan` 不运行该 SOES 测试；
  - 配套最小 Bootloader 接入参考集成测试 `test_bootloader_integration` 已集成入 `test-bootloader` 目标（覆盖 5 大场景）；
  - 在宿主机正常栈限制（`ulimit -s 8192`）与受限栈深度（`ulimit -s 1024`）下全量测试通过，退出码均为 `rc=0`。

### 7.4 明确未执行内容与工程边界声明 (Limitations)

本项目客观如实界定本次真实 SOES 核验的边界，以下内容受环境限制**明确未执行**：
1. **无物理从站硬件**：未在真实物理芯片（如 Microchip LAN9252 或 Beckhoff ET1100 从站控制器 ASIC）上执行；
2. **无物理总线与收发器通信**：未连接物理网线、RJ45/MII 接口及物理 PHY 收发器，所有报文交互均在宿主机内存模拟邮箱中完成；
3. **无真实 EtherCAT 主站轮询**：未连接 TwinCAT、SOEM 等真实主站运行周期性过程数据通信与状态机轮询；
4. **嵌入式 MCU 足迹未测**：当前仅在 macOS 宿主环境 Clang 编译器下构建测试，未在 ARM Cortex-M 等嵌入式目标架构交叉编译器下构建，MCU 的 Flash 固件尺寸与最大调用栈深度尚未实际测量。

### DATA 阶段的协议栈中止

接入时应串行调用前置钩子、SOES 邮箱处理和后置钩子。适配器将本轮 DATA 与新排队的 FoE ACK 包号及写回调结果关联；过早结束、写回调失败、协议栈 ERR、无法确认 ACK 或无输出槽位时，中止尚活跃的核心会话，并在已配置复位回调时复位 SOES。已由核心记录的失败状态和已提交状态不会被改写；如果最终提交完成后响应丢失，提交无法撤销，主站仍需按实际设备策略恢复。该行为有真实 SOES 源码的宿主测试，物理设备和主站仍需验证。

不支持或畸形的 FoE 请求被协议栈拒绝时也同步中止活跃核心会话。WRQ 在原生 SOES 仍接收数据时触发 `0x8004` 中止时，后置钩子同步中止核心活跃会话；无新响应且驱动仍活跃时也会清理。文件名预检本身不触碰旧会话。
