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

**本地源文件基线与 SHA-256 (防版本漂移与基线锚定)**：
- 来源路径: 本地散文件目录 `/tmp/soes_src/soes/`
- 版本元数据: 目录无 `.git` 版本仓库，上游确切 Git Commit ID 未知 (UNKNOWN)。
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

### 6.2 Microchip LAN9252 数据手册引用 (`lan9252.txt`)

**官方数据手册下载与核验实况 (`/tmp/vendor_docs/`)**：
- 官方来源: Microchip 官网 `https://ww1.microchip.com/downloads/en/DeviceDoc/00001909A.pdf`
- 本地下载文件: `/tmp/vendor_docs/00001909A.pdf` (SHA-256: `f9b73097c21a1ad5e9b1a23ba10002c6bbeb7836422376c646e84313294e96c6`)
- 文本转换文件: `/tmp/vendor_docs/lan9252.txt` (40,871 行, SHA-256: `e9839ad1bbd0ee4fe725584b0d2fa8fd545693a6f974ff4f9e193f698790b2e1`)
- 核验比对说明：本表对照 Microchip 官方原版 DS00001909A PDF 及其文本转换文件进行核验，明确区分 PDF 阅读器物理页码与手册底部印刷页码。经逐项比对，技术内容、寄存器名与位定义与官方原版完全一致；关于历史文档中的行号与当前转换文本行号差异，因缺乏确凿转换版本记录，成因未作主观推测，下表客观对比各项出处（不假定同号即正确）。

| 官方章节 (Section) | PDF 物理页 | 底部印刷页 | 英文逐字原文 (English Verbatim Text) | 当前文本转换行号 (`/tmp/vendor_docs/lan9252.txt`) | 历史引用行号 (Historical Citation) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Table 6-1 (复位源与功能影响表) | Page 38 | Page 38 | `RESET SOURCES AND AFFECTED DEVICE FUNCTIONALITY` (`EEPROM Loader Run`) | 第 4694, 4718 行 | 第 4758 行 |
| Section 6.2.1.2 (RST# 引脚复位) | Page 39 | Page 39 | `6.2.1.2 RST# Pin Reset` (`Driving the RST# input pin low initiates a chip-level reset.`) | 第 4867, 4869 行 | 第 4874, 4875 行 |
| Section 12.14.20 (AL Control 寄存器) | Page 239 | Page 239 | `12.14.20 AL CONTROL REGISTER` (Offset: `0120h-0121h`) | 第 30981 行 | 历史未单独制表 |
| Section 12.14.21 (AL Status 寄存器) | Page 240 | Page 240 | `12.14.21 AL STATUS REGISTER` (Offset: `0130h-0131h`) | 第 31059 行 | 历史未单独制表 |
| Section 12.14.22 (AL Status Code 寄存器) | Page 241 | Page 241 | `12.14.22 AL STATUS CODE REGISTER` (Offset: `0134h-0135h`) | 第 31174 行 | 历史未单独制表 |
| Section 12.14.34 (AL Event Request 寄存器) | Page 253 (标题在 252 页) | Page 253 (标题在 252 页) | `This bit is cleared by reading the AL Control Register from PDI.` | 第 32264 行 | 历史误标为未核实 |
| Section 12.14.25 (Note 11 引用 12.8) | Page 243 | Page 243 | `Note 11: The default value of this field can be configured via EEPROM.` (引用 Page 201 Sec 12.8) | 第 31430 行 | 第 31434 行 |
| Section 12.14.49 | Page 261 | Page 261 | `12.14.49 EEPROM CONTROL/STATUS REGISTER` | 第 33483 行 | 第 33481, 33485 行 |
| Section 12.14.49 (Bit 15) | Page 261 | Page 261 | `Busy` | 第 33506 行 | 第 33496, 33498 行 |
| Section 12.14.49 (Bit 14) | Page 261 | Page 261 | `Error Write Enable` | 第 33518 行 | 第 33504 行 |
| Section 12.14.49 (Bit 13) | Page 261 | Page 261 | `Error Acknowledge/Command` | 第 33531 行 | 第 33518 行 |
| Section 12.14.49 (Bit 12) | Page 261 | Page 261 | `EEPROM Loading Status` | 第 33550 行 | 第 33532 行 |
| Section 12.14.49 (Bit 11) | Page 261 | Page 261 | `Checksum Error in ESC Configuration Area` | 第 33563 行 | 第 33544 行 |
| Section 12.14.49 (Bits 10:8) | Page 262 | Page 262 | `Command Register` (Write 010, Reload 100) | 第 33593-33599 行 | 第 33568, 33574, 33575 行 |
| Section 12.14.50 | Page 263 | Page 263 | `12.14.50 EEPROM ADDRESS REGISTER` | 第 33698 行 | 第 33698, 33702, 33717 行 |
| Section 12.14.51 | Page 263 | Page 263 | `12.14.51 EEPROM DATA REGISTER` | 第 33751 行 | 第 33744, 33748, 33769 行 |
| Section 12.14.51 (Note 27) | Page 265 | Page 265 | `Note 27: The default value of this field can be configured via EEPROM.` | 第 33978 行 | 第 33980 行 |

### 6.3 Beckhoff ET9300 应用笔记引用 (`an_et9300.txt`)

**官方文档获取实况说明**：
- 官方路径测试 1 (v1i10): `https://download.beckhoff.com/download/document/io/ethercat-development-products/an_et9300_v1i10.pdf`
  - 实测结果: HTTP 403 Forbidden (CloudFront WAF 拦截页面, HTML 919 字节, 包含 "403 ERROR: The request could not be satisfied. Request blocked.")；
- 官方路径测试 2 (v1i8): `https://download.beckhoff.com/download/document/io/ethercat-development-products/an_et9300_v1i8.pdf`
  - 实测结果: HTTP 404 Not Found (HTML 页面)；
- 官方路径测试 3 (Application_Notes 目录): `https://download.beckhoff.com/download/document/Application_Notes/an_et9300_v1i10.pdf` 及 `v1i8.pdf`
  - 实测结果: 均为 HTTP 404 Not Found；
- 访问控制策略: Beckhoff 官方 CDN 实施了反爬与访问控制策略，禁止外部自动化脚本公开匿名下载；
- 状态判定: 本地无源文件，未核验。严格保留为未核验状态，绝不伪造核验结论或宽泛宣称已核验。

| 文档出处 | 章节与描述内容 | 核验状态与说明 |
| :--- | :--- | :--- |
| 历史记录行号 | 第 15 章 第 129 页 `BOOTSTRAPMODE_SUPPORTED` 宏开关说明 | **【本地无源文件 / 未核验】** (Beckhoff 官方限制匿名下载，本地无 `an_et9300.txt`，属历史记录) |
| 历史记录行号 | 第 15 章 第 129 页 `Which mechanism is implement is up to the slave vendor.` | **【本地无源文件 / 未核验】** (Beckhoff 官方限制匿名下载，本地无源文件) |

### 6.4 ETG 官方规范引用
| 规范编号 | 引用主题 | 核验状态与说明 |
| :--- | :--- | :--- |
| `ETG.1000.6` | EtherCAT 应用层服务规范 (FoE 传输机制) | **【本地无源文件 / 未核验】** (外部标准，本地无官方 PDF) |
| `ETG.2010` | EtherCAT 从站架构与 ID 识别要求 | **【本地无源文件 / 未核验】** (外部标准，本地无官方 PDF) |
