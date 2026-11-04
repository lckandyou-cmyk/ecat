# SOES 协议栈接入方案评估与风险说明 (SOES Integration Options & Risks)

本文档针对原生 **SOES (Simple Open EtherCAT Slave)** 协议栈接入通用固件升级核心库进行技术可行性、边界限制、源码证据与风险评估。
**当前协议架构维持现状，待最终技术方案审议确定后再行演进，不改动现有协议规范。**

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
以下分析基于对 SOES 原生源码第 70~73 行的**静态代码推断 (Static Code Inspection)**，尚未在主站物理环境下完成全流程动态验证：
1. **由接收长度决定的前缀匹配**：
   - 比较长度参数为 `num_chars`（即主站发送的文件名长度）。这属于前缀匹配，绝非严格的全名精确匹配；
2. **【静态推断】空文件名命中首条配置隐患**：
   - 当主站发来的 WRQ 报文未携带文件名或文件名长度为 0 时，`num_chars == 0`；
   - 根据 C99 标准，`strncmp(s1, s2, 0)` 恒等于 0，`0 == 0` 条件在循环的第 1 次迭代（`i = 0`）即无条件成立；
   - **推断后果**：主站发出的任何空文件名写请求，都会被原生 SOES 默认误路由并绑定到首条文件配置（通常为 `files[0]` 对应的 MCU 固件）；此现象属于静态逻辑推断，【待真机动态验证】；
3. **【静态推断】短前缀误绑定多目标隐患**：
   - 若从站配置中包含多个文件表项（如 `files[0].name = "SERVO_DRV_MCU.efw"`，`files[1].name = "SERVO_DRV_FPGA.efw"`）；
   - 若主站下发了通用前缀（如 `"SERVO_DRV"`，`num_chars == 9`），`strncmp` 将在前 9 个字节匹配成功，直接命中 `files[0]`；
   - 即使用户本意是升级 FPGA，若前缀通用，也会被不可抗拒地绑定到首个匹配项上；
4. **动态版本文件名被阻断**：
   - 若配置表中为基准名（长度 17），主站按规范下发带动态版本号的文件名（如 `"SERVO_DRV_MCU_V1.0.0.0.efw"`，长度 26）；
   - 由于 `num_chars == 26`，`strncmp` 将比较 26 字节，导致比对越界或不相等，原生 SOES 直接返回 `FOE_ERR_NOTFOUND (0x8001)`。

### 1.3 候选方案对比 (待决)
| 方案 | 具体做法 | 预期优势 | 局限与待核实事项 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| **方案 A<br>(固定目标文件名)** | 协议约定主站升级时下发固定文件名（如 `SERVO_DRV_MCU.efw`）。硬件 ID、工程 ID、目标 ID 及版本号完全由 64 字节二进制 `.efw` 文件头承载并校验。 | 利用原生 SOES 静态表项机制，从站无需打补丁。 | - 主站无法从文件名直观获知版本；<br>- **【未核实】** 是否与所有主站工具和外部协议栈完全兼容尚无充分实测证据。 | **待裁定** |
| **方案 B<br>(SOES 栈打补丁)** | 修改 SOES `esc_foe.c`，在 WRQ 入口增加用户动态路由钩子：<br>`int (*foe_wrq_hook)(const char *name, size_t len)`。 | 支持从文件名动态提取版本与目标，与现有规范严格一致。 | 侵入第三方协议栈源码，增加维护与版本升级负担。 | **待裁定** |

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

- **源码事实**：在 `data_len == 0` 分支，`FOE_fclose()` 的返回值确实被直接丢弃，即使底层提交返回错误，SOES 依然会发出成功 ACK 并重置状态。

### 2.2 防砖能力与可靠性客观边界 (消除绝对化保证)
此前文档中关于“绝不会变砖”、“必然无法启动”等断言不够客观严谨。设备在异常中断或校验失败后能否可靠停留在 Bootloader，**高度依赖以下系统与工程边界条件，不存在绝对保证**：
1. **驱动有效标记的非易失性持久化**：
   - 核心机在解完固件头后调用 `invalidate()`，要求清除固件有效标记；
   - **关键依赖**：此操作必须在非易失介质（Flash 扇区或特定标志位）上实现掉电原子性持久化。若硬件仅在 RAM 中修改标记、或标记擦写期间发生断电造成状态未定义，设备重启后可能误跳入损坏固件；
2. **Bootloader 启动逻辑引导契约**：
   - 依赖物理 Bootloader 在系统上电复位时，严格先行检查该有效标记。若标记非法或已擦除，才强制停留在 Bootloader 等待重新升级；
3. **finalize 成功后的版本下限持久化非原子性**：
   - 依据 SAD 规范与防回滚设计：若固件烧录完毕且 `finalize()` 成功执行（固件有效标记已置位），后续若执行防回滚更新版本下限失败，**固件依然保持有效状态**（核心机返回防回滚保存失败错误，但不回滚已烧录的固件）；
   - 在此场景下设备重启依然会正常引导新固件，而非无法启动。

---

## 3. 关闭 RRQ 的真实编译方式与缓冲区越界风险

### 3.1 预编译宏机制核查
原生 SOES 采用 `#ifdef FOE_READ_SUPPORTED`（`esc_foe.c:110, 510, 598`）控制读逻辑编译。
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

### 4.2 现有实现状态界定 (区分设计与实现)
- **【单测桩仿真与局限性】现有适配桩状态**：目前适配层 `src/adapters/ecat_foe_soes.c` 实现了双参数 `ecat_foe_soes_post_state_hook(adapter, as, an)` 的单测桩，能够在 `*as == 0x13U` 且 `last_commit_failed == true` 时改写 `*an = 0x13U` (BOOT|Error) 并记录 `last_al_error = 0x0014U` (ALERR_NOVALIDFIRMWARE)；
- **局限性与待验证项**：当前仍为单测桩，**未引入 SOES 真实头文件，未在真实硬件上集成编译，未进行真实主站动态联调【未核实/待开发与真机动态验证】**。真实硬件环境下由于邮箱已在 switch 内部通过 `ESC_stopmbx()` 停止，主站无法在该状态下通过 FoE 邮箱继续交互，必须先按 EtherCAT 规范走状态机复位恢复流程。

---

## 5. 源码引用自动化校验清单 (Registered Quotes)

为了保证本文档引用的每一处 SOES 源码绝对真实，项目提供了三方校验脚本 `tools/verify_soes_quotes.py` 与外部清单 `docs/quotes_manifest.json`，针对以下明确登记的源码条目进行全量行号与文本三方比对：

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

*说明：校验脚本通过读取 `docs/quotes_manifest.json`，同时提取本文档中的 `<!-- quote-begin: <ID> -->` 块与本地 SOES 源码对应行进行三方严格核验。本工具仅对上述明确登记的 8 处引用条目作出真实性承诺，不涵盖未登记引文。*

---

## 6. 全局行号与外部规范引用盘点表 (Reference Inventory)

为落实严谨客观的工程标准，本项目对文档中所有涉及具体行号或外部规范的出处引用进行全量盘点，明确区分【本地自动化三方核验】、【本地人工比对核验】与【本地无源文件 / 未核验】：

### 6.1 SOES 协议栈源码引用 (本地 `/tmp/soes_src/soes/`)

**本地源文件基线与 SHA-256 (防止版本漂移)**：
- 来源仓库: OpenEtherCATsociety/SOES (本地路径: `/tmp/soes_src/soes/`)
- `esc.c`: `1ef43fde2f2cf3306d16e8f8e3e86a1be20a013b0bffd833711749575f60c42c`
- `esc.h`: `9c76c2d4a20b4f827539a4416383354cb45e95a9d086f444a26a0bbb5ff9a579`
- `esc_foe.c`: `cd1709f103e5c4b786fb0be2857760f0e180c9123a553a2efa410accbf17a2b7`
- `esc_foe.h`: `84f5011c258d1e25d8d9ab22bfb74f4ab45b1e6b01f3a6be13e5cf2138034eb7`

| 源码文件与行号 | 引用内容或用途 | 核验模式与状态 |
| :--- | :--- | :--- |
| `esc_foe.c:70-73` | WRQ 文件名匹配判定 (`0 == strncmp`) | **【自动三方逐行严格核验 (Q1)】** |
| `esc_foe.c:459-465` | 0 长度包结束分支 (`FOE_fclose` 返回值被忽略) | **【自动三方逐行严格核验 (Q2)】** |
| `esc_foe.c:493-506` | 短包结束分支 (`FOE_fclose` 严格返回值检查) | **【自动三方逐行严格核验 (Q3)】** |
| `esc.h:317-318` | `pre/post_state_change_hook` 钩子函数指针声明 | **【自动三方逐行严格核验 (Q4)】** |
| `esc.h:712` | `ESC_ALerror` 错误码设置函数原型声明 | **【自动三方逐行严格核验 (Q5)】** |
| `esc.c:1128-1135` | `pre_state_change_hook` 调用时机与迁移码合成 | **【自动三方逐行严格核验 (Q6)】** |
| `esc.c:1191-1196` | `BOOT_TO_INIT` 停用邮箱及赋值 `an = ESCinit` | **【自动三方逐行严格核验 (Q7)】** |
| `esc.c:1304-1308` | `post_state_change_hook` 触发时机（停邮箱之后） | **【自动三方逐行严格核验 (Q8)】** |
| `esc_foe.h:17` | `#define FOE_FN_MAX 31` 文件名长度上限 | **【本地人工核对】** 对照本地源文件确认 |
| `esc_foe.c:158-163` | 接收缓冲区满调用 `write_function` 及地址累加 | **【本地人工核对】** 对照本地源文件确认 |
| `esc_foe.c:187-195` | `FOE_fclose` 关闭文件时写入剩余缓冲区 | **【本地人工核对】** 对照本地源文件确认 |
| `esc.c:1155-1160` | `INIT_TO_BOOT` 与 `BOOT_TO_BOOT` 合并分支，调用 `ESC_startmbxboot` 启动 Bootstrap 邮箱 | **【本地人工核对】** 对照本地源文件确认 |
| `esc.c:1197-1202` | 非 INIT 状态切入 BOOT 直接报错设置 | **【本地人工核对】** 对照本地源文件确认 |

### 6.2 Microchip LAN9252 数据手册引用 (`lan9252.txt`)
| 文档行号出处 | 手册章节与描述内容 | 核验状态与说明 |
| :--- | :--- | :--- |
| 第 4758 行 | 表 6-1: EEPROM Loader Run 对应 RST# Pin 列为 YES | **【本地无源文件 / 未核验】** (本地无 `lan9252.txt`，属历史记录) |
| 第 4874, 4875 行 | 第 6.2.1.2 节: RST# 引脚拉低触发芯片级复位 | **【本地无源文件 / 未核验】** |
| 第 31434 行 | Note 11: Enhanced Link 限制说明 | **【本地无源文件 / 未核验】** |
| 第 33481, 33485 行 | 0502h-0503h EEPROM CONTROL/STATUS REGISTER | **【本地无源文件 / 未核验】** |
| 第 33496, 33498 行 | Bit 15: EEPROM Busy 状态位 | **【本地无源文件 / 未核验】** |
| 第 33504 行 | Bit 14: Error Write Enable | **【本地无源文件 / 未核验】** |
| 第 33518 行 | Bit 13: Error Acknowledge/Command | **【本地无源文件 / 未核验】** |
| 第 33532 行 | Bit 12: EEPROM Loading Status | **【本地无源文件 / 未核验】** |
| 第 33544 行 | Bit 11: Checksum Error in ESC Config Area | **【本地无源文件 / 未核验】** |
| 第 33568, 33574, 33575 行 | Bits 10:8: Command (Write 010, Reload 100) | **【本地无源文件 / 未核验】** |
| 第 33698, 33702, 33717 行 | 0504h-0507h EEPROM ADDRESS REGISTER | **【本地无源文件 / 未核验】** |
| 第 33744, 33748, 33769 行 | 0508h-050Bh EEPROM DATA REGISTER | **【本地无源文件 / 未核验】** |
| 第 33980 行 | Note 27: MI link 限制说明 | **【本地无源文件 / 未核验】** |

### 6.3 Beckhoff ET9300 应用笔记引用 (`an_et9300.txt`)
| 文档行号出处 | 章节与描述内容 | 核验状态与说明 |
| :--- | :--- | :--- |
| 第 9725 行 | 第 15 章 第 129 页 `BOOTSTRAPMODE_SUPPORTED` 宏开关说明 | **【本地无源文件 / 未核验】** (本地无 `an_et9300.txt`，属历史记录) |
| 第 9728 行 | 第 15 章 第 129 页 `Which mechanism is implement is up to the slave vendor.` | **【本地无源文件 / 未核验】** |

### 6.4 ETG 官方规范引用
| 规范编号 | 引用主题 | 核验状态与说明 |
| :--- | :--- | :--- |
| `ETG.1000.6` | EtherCAT 应用层服务规范 (FoE 传输机制) | **【本地无源文件 / 未核验】** (外部标准，本地无官方 PDF) |
| `ETG.2010` | EtherCAT 从站架构与 ID 识别要求 | **【本地无源文件 / 未核验】** (外部标准，本地无官方 PDF) |
