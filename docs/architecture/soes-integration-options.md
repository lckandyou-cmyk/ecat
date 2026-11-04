# SOES 协议栈接入方案评估与风险说明 (SOES Integration Options & Risks)

本文档针对原生 **SOES (Simple Open EtherCAT Slave)** 协议栈接入通用固件升级核心库进行技术可行性、边界限制与风险评估。
**当前协议架构维持现状，待最终技术方案审议确定后再行演进，不改动现有协议规范。**

---

## 1. WRQ 动态文件名匹配限制与候选方案

### 1.1 原生源码机理核查
在本地 `/tmp/soes_src/soes/esc_foe.c` 第 70~73 行，原生 SOES 处理 WRQ 文件名匹配的代码如下：
```c
70:    for (i = 0; i < foe_cfg->n_files; i++)
71:    {
72:       if (strncmp (foe_file_name, foe_cfg->files[i].name, num_chars) == 0)
73:       {
```
- **匹配特性**：`strncmp` 的比较长度参数由主站发送的文件名长度 `num_chars` 决定。这本质上是**由接收长度决定的前缀匹配，绝非严格的精确匹配**；
- **原生局限**：SOES 仅静态遍历 `foe_cfg->files[]` 数组，内部没有任何向上层应用抛出 WRQ 文件名进行动态识别或路由的回调钩子；
- **直接后果**：若在配置表 `files[]` 中注册目标基准名（如 `"SERVO_DRV_MCU.efw"`，长度 17），当主站按照规范下发携带动态版本号的文件名（如 `"SERVO_DRV_MCU_V1.0.0.0.efw"`，长度 26）时，`num_chars == 26`，`strncmp` 将对静态配置字符串进行 26 字节的比对，必将判定不匹配，原生 SOES 直接返回 `FOE_ERR_NOTFOUND (0x8001)` 拒绝请求。

### 1.2 候选方案对比
| 方案 | 具体做法 | 优势 | 代价 / 限制 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| **方案 A<br>(固定目标文件名)** | 协议约定主站升级时下发固定文件名（如 `SERVO_DRV_MCU.efw`、`SERVO_DRV_FPGA.efw`）。硬件 ID、工程 ID、目标 ID 及版本号完全由 64 字节二进制 `.efw` 文件头携带并校验。 | 100% 兼容原生 SOES 协议栈，**零补丁、零侵入**；各协议栈通用。 | 主站下发时从文件名字面上看不到动态版本号（但文件头提供强校验）。 | **待裁定** |
| **方案 B<br>(SOES 栈打补丁)** | 修改 SOES `esc_foe.c`，在 WRQ 入口增加用户动态路由钩子：<br>`int (*foe_wrq_hook)(const char *name, size_t len)`。 | 支持从文件名动态提取版本与目标，保持与当前规范严格一致。 | 侵入第三方开源协议栈源码，给后续协议栈升级和用户维护增加负担。 | **待裁定** |
| **方案 C<br>(静态表项穷举)** | 在静态表 `files[]` 中预先枚举可能的文件名前缀。 | 不侵入协议栈。 | 受限于 `num_chars` 为接收长度的前缀比对方向，无法枚举动态版本，不可行。 | **已排除** |

*结论：方案待决，目前核心库保持分层解耦，暂不变更现有协议架构。*

---

## 2. 零长度包错误被忽略与防砖安全性

### 2.1 原生源码事实
在 `/tmp/soes_src/soes/esc_foe.c` 第 459~465 行：
```c
459:    else if (data_len == 0)
460:    {
461:       DPRINT("FOE_data completed\n");
462:       FOE_fclose ();
463:       res = FOE_send_ack ();
464:       FOE_init ();
465:    }
```
- **返回值被忽略**：在 `data_len == 0` 分支中，`FOE_fclose()` 的返回值未被任何变量接收，未做任何错误判断，直接发送了 ACK；
- **对比短包分支**（第 495 行）：`if ((ncopied != data_len) || FOE_fclose ())`，短包分支严格检查了 `FOE_fclose()` 的返回值并在失败时调用 `FOE_abort(FOE_ERR_PROGERROR)`。

### 2.2 风险场景分析
当升级文件总长度（头部 + Payload）恰好为分包大小（例如 512 字节）的整数倍时，主站传输完最后一块满包后，会依照协议发送一个 `data_len == 0` 的 DATA 包标识传输结束。
若此时核心机在 `on_end()` 执行尾部校验、CRC32 回读或 `finalize()` 时返回错误，由于 `FOE_fclose()` 返回值被 SOES 忽略，SOES 依然向主站返回了成功 ACK，导致主站误认为固件烧录成功。

### 2.3 底层防砖机制与缓解措施
1. **底层防砖保证（Invalidate First）**：
   - 核心机在解完固件头并验证通过后，在开始任何写入/擦除前，已立即调用底层驱动的 `invalidate()` 将现有固件标记为无效；
   - 即使 0 长度包返回了假 ACK，设备内部物理固件的有效标记仍然为 `false`；
   - 设备重启后，Bootloader 检测到固件无效，**绝不会跳转到损坏的应用固件，而是强制驻留在 Bootloader**，等待主站重新发起升级，**绝对不会变砖**。
2. **缓解措施 1：打包工具规避填充**：
   - 使用 `tools/efw_pack.py --foe-packet-size 512`，在固件尾部填充 0xFF 破坏整除，使传输结尾几乎总走短包路径（尽力而为缓解）；
3. **缓解措施 2：状态迁移拦截（见第 4 节）**。

---

## 3. 关闭 RRQ 的真实编译方式与宏定义陷阱

在 `/tmp/soes_src/soes/esc_foe.c` 中：
- 第 110 行：`#ifdef FOE_READ_SUPPORTED` 控制 `FOE_fread()` 声明；
- 第 510 行：`#ifdef FOE_READ_SUPPORTED` 控制 `FOE_busy()` 声明；
- 第 598 行：`#ifdef FOE_READ_SUPPORTED` 控制 `case FOE_OP_RRQ:` 分支。

### 预编译行为精确核对：
1. **未定义（唯一正确关闭方式）**：
   - 编译参数**不传入** `-DFOE_READ_SUPPORTED`，或源码显式 `#undef FOE_READ_SUPPORTED`；
   - 此时 `#ifdef` 为假，`case FOE_OP_RRQ:` 分支不被编译。收到 RRQ 请求（Opcode 1）时直接滑入 `default:` 分支，调用 `FOE_abort(FOE_ERR_NOTDEFINED)`（宏数值 `0x8000`）。
2. **定义为 0（常见配置陷阱）**：
   - 若在 Makefile 或编译器选项中传入 `-DFOE_READ_SUPPORTED=0`；
   - 由于 C 预处理器中 `#ifdef` 仅检查宏是否存在而不检查其数值，**`#ifdef` 仍被判定为真**！
   - SOES 会编译包含 `FOE_read()` 代码，并尝试从静态内存 `foe_cfg->fbuffer` 读取，导致无法关闭读请求，存在安全与内存隐患！
3. **定义为 1**：
   - 开启 RRQ 读取功能。

---

## 4. BOOT 状态机钩子与错误码桥接机制

### 4.1 接口定义与机制设计
在 `/tmp/soes_src/soes/esc.h` 第 318 行与第 712 行：
- `void (*post_state_change_hook) (uint8_t * as, uint8_t * an);`
- `void ESC_ALerror (uint16_t errornumber);`
- `ALERR_NOVALIDFIRMWARE` 数值为 `0x0014`（`esc.h:111`）。

**桥接拦截逻辑**：
当升级过程发生错误（如 0 长度包提交失败导致 `adapter->last_commit_failed == true`）后：
1. 主站尝试将从站迁出 BOOT 状态（`BOOT -> INIT`，此时 `*as == STATE_BOOT && *an == STATE_INIT`）；
2. 在 `post_state_change_hook` 中拦截该迁移，将目标状态改回错误指示状态（`*an = STATE_BOOT | ESCerror`）；
3. 调用 `ESC_ALerror(ALERR_NOVALIDFIRMWARE)`，向 EtherCAT 寄存器写入 AL 状态错误码 `0x0014`，明确通知主站固件无效。

### 4.2 当前实现局限与现状
- **【待开发验证】**：当前代码库 `src/adapters/ecat_foe_soes.c` 仅为本地单元测试打样桩，尚未引入真实 SOES 的 `esc.h` 头文件，未实际调用 `ESC_ALerror`；
- **【未核实】**：在真实 SOES 协议栈和不同 EtherCAT 主站（TwinCAT、Codesys 等）环境下，改写 `*an` 是否会破坏主站状态机恢复流程，需待真实硬件联调确认。

---

## 5. 阶段性验证计划

```text
+-----------------------+     +-----------------------+     +-----------------------+
|  阶段 1: 宿主机单测   | --> | 阶段 2: 真实协议栈编译 | --> |  阶段 3: 物理硬件联调  |
| (纯 C99 / ASan 验证)  |     | (SOES 真实源码集成)   |     | (ESC 芯片 + 主站工具) |
+-----------------------+     +-----------------------+     +-----------------------+
```

1. **阶段 1：宿主机仿真验证【已完成】**
   - 验证核心状态机、头部反序列化、流式 CRC32、双重检验防线、驱动生命周期互斥及自愈机制；
   - 覆盖率：Clang / GCC 下共 455 项断言全部通过，ASan/UBSan 零内存泄漏与越界。
2. **阶段 2：真实 SOES 源码集成编译【待开展】**
   - 在构建系统中引入 `/tmp/soes_src/soes` 真实源码；
   - 严格在未定义 `FOE_READ_SUPPORTED` 条件下编译，验证 RRQ 被拒为 `0x8000`；
   - 编写真实集成用例验证 `foe_file_cfg_t` 回调与 `post_state_change_hook`。
3. **阶段 3：物理硬件与主站联调【待真机验证】**
   - 硬件载体：Cortex-M MCU + 物理 ESC 芯片（Beckhoff ET1100 / Microchip LAN9252 / ASIX AX58100）；
   - 主站工具：TwinCAT 3 / 专用 EtherCAT 升级工具；
   - 关键验证项：
     - 片内 Flash 擦写总线挂起（Bus Stall）对 EtherCAT 同步帧的影响；
     - 外部 SPI Flash 物理全量回读耗时是否导致 TwinCAT 响应超时；
     - 升级中途拔网线、强制断电后的 Bootloader 自愈恢复能力。
