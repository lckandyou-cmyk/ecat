# 最小 Bootloader 接入参考 (SOES Bootloader Reference)

本目录提供基于本库通用公开 API 与上游开源 **Simple Open EtherCAT Slave (SOES)** 协议栈的最小可编译、可移植 Bootloader 接入参考实现与宿主端集成验证套件。

---

## 1. 架构定位与核心安全准则

### 1.1 物理基地址本地硬编码裁决 (Strict Local Address Isolation)
在固件升级过程中，Flash 擦除与写入的物理基地址由本地静态配置（`bootloader_config.h` 中的 `APP_FLASH_BASE_ADDR`）**独家裁决**：
- **不信任远程数据**：严禁直接采用 FoE WRQ 报文中的 `dest_start_address`、文件名或 `.efw` 容器内包含的任何外部地址作为物理写入指针；
- **防越界踩踏**：物理分区上限 `APP_FLASH_MAX_SIZE` 在驱动层进行硬性边界检查，从物理机制上杜绝恶意或损坏的固件越界破坏 Bootloader 自身代码区（`0x08000000 ~ 0x0801FFFF`）。

### 1.2 动态文件名协议保持
本参考实现严格遵循现有动态文件名解析协议：
```text
<PROJECT_NAME>_<TARGET>_V<Major>.<Minor>.<Patch>.<Build|Revision>.efw
```
例如：`SERVO_DRV_MCU_V1.0.0.0.efw`。前置钩子通过只读解析提取文件名元数据，与本地配置表比对匹配，不修改既有文件名协议。

---

## 2. 模块结构与文件清单

```text
examples/bootloader_soes/
├── README.md                     # 本参考文档
├── bootloader_config.h           # 板级静态配置 (工程标识、硬件版本与物理分区基地址)
├── bootloader_flash_driver.h     # 目标存储 Flash 驱动接口头文件
├── bootloader_flash_driver.c     # 目标存储 Flash 驱动参考实现 (七大生命周期与本地基地址裁决)
├── bootloader_app.h              # Bootloader 核心应用层头文件
├── bootloader_app.c              # Bootloader 核心应用层实现 (前后置钩子与协议栈串行调度)
├── bootloader_master_flow.h      # EtherCAT 主站交互流程与测试封装头文件
├── bootloader_master_flow.c      # EtherCAT 主站交互流程参考实现
└── test_bootloader_integration.c # 宿主端可执行的 5 大场景集成验证套件
```

---

## 3. 板级配置与存储驱动生命周期契约

### 3.1 板级配置 (`bootloader_config.h`)
定义本地工程识别符、硬件版本范围及 Flash 分区：
- `BOOTLOADER_PROJECT_ID`: `0x10010001U`（工程识别码）
- `BOOTLOADER_PROJECT_NAME`: `"SERVO_DRV"`（工程字符串）
- `BOOTLOADER_HARDWARE_ID`: `0x00000001U`（硬件板型码）
- `APP_FLASH_BASE_ADDR`: `0x08020000U`（应用固件写入物理基地址）
- `APP_FLASH_MAX_SIZE`: `256 * 1024`（应用分区 256 KB 容量上限）
- `APP_FLASH_SECTOR_SIZE`: `4096`（Flash 扇区擦除粒度 4 KB）
- `APP_FLASH_WRITE_UNIT`: `8`（Flash 最小对齐写入单位 8 字节）

### 3.2 目标存储驱动生命周期契约 (`bootloader_flash_driver.c`)
满足 `ecat_foe_target_driver_t` 接口契约：
1. **`init()` 失败自行清理**：若初始化失败返回非 0，驱动自行释放半成品，核心状态机不调用 `abort()`；
2. **`init()` 成功后异常恰好调用一次 `abort()`**：驱动一旦初始化成功，后续任何阶段（invalidate 失败、擦除失败、写入失败、校验不符或主站中止）核心机严格调用且仅调用一次 `abort()`；
3. **`invalidate()` 必须持久且早于擦除**：在擦除任何物理扇区前，先将固件有效标记置为无效（`APP_FLASH_INVALID_MAGIC`），确保若擦写中途突发掉电，设备重启能停留在 Bootloader 中等待重新升级；
4. **`erase_to()` 按需扇区擦除**：基于写入进度向上对齐擦除扇区，未执行 invalidate 前严禁擦除；
5. **`write()` 本地基地址写入**：物理目标地址严格计算为 `APP_FLASH_BASE_ADDR + offset`；
6. **`read()` 物理回读比对**：从介质读回已写入数据，供核心机执行物理 CRC32 双重校验；
7. **`finalize()` 最终提交有效标记**：仅在全部数据流接收完整、流 CRC 与物理回读 CRC32 比对完全一致后，最后写入有效标志（`APP_FLASH_VALID_MAGIC`）；
8. **`abort()` 幂等清理**：释放外设，保持有效标志为无效状态。

---

## 4. SOES 串行调度、初始化与异常恢复

### 4.1 核心串行轮询调度流 (`bootloader_app_step`)
SOES 协议栈本身非多线程安全，接入必须遵循单执行流原子互斥契约：
```text
   +------------------------------------------------------------+
   | 1. ecat_foe_soes_pre_process_hook                          |
   |    - 嗅探接收邮箱，只读解析文件名与预检，暂存 WRQ          |
   +------------------------------------------------------------+
                                |
                                v
   +------------------------------------------------------------+
   | 2. 原生 SOES FoE 处理进程 (ESC foeprocess)                 |
   |    - 原生密码与 BOOT 状态校验，状态机流转与排队响应        |
   +------------------------------------------------------------+
                                |
                                v
   +------------------------------------------------------------+
   | 3. ecat_foe_soes_post_process_hook                         |
   |    - 核验输出槽位；若核心激活失败或状态冲突，就地改写为    |
   |      0x8000 ERR 并触发协议栈复位回调 (FOE init)            |
   +------------------------------------------------------------+
```

### 4.2 初始化与异常恢复机制
- **信箱上下文绑定**：调用 `ecat_foe_soes_set_mbx_context()` 建立对 SOES 邮箱数组的状态观察，不篡改 SOES 缓冲区所有权；
- **状态机复位回调注入**：调用 `ecat_foe_soes_set_foe_reset_fn(&adapter, FOE_init)`。当后置钩子因核心激活失败或竞态冲突将已排队的 ACK(0) 就地改写为 0x8000 ERR 时，主动调用 `FOE_init()` 将 SOES 即刻复位回 `FOE_READY` 态；
- **信箱计数保持**：异常转换时严格保留原有信箱头部的高 4 位计数（`mbxcnt`），确保主站观察到协议计数连续一致；
- **自愈恢复重试**：即使未注册复位钩子，原生 SOES 在收到中途新 WRQ 时返回 0x8004 并自动复位，收到后续 DATA 时 write_cb 拦截并返回 0x800B 自动复位，收到主站 ERR 报文自动复位，确保从站通信自愈闭环。

---

## 5. 主站发送交互时序规范

主站与从站 Bootloader 的标准 FoE 固件升级交互时序如下：
1. **写请求阶段 (WRQ)**：
   - 主站下发 `FOE_OP_WRQ`（opcode=0x02），包含目标文件名与密码；
   - 从站返回 `FOE_OP_ACK`（opcode=0x04, packetnumber=0）。
2. **数据流发送阶段 (DATA)**：
   - 满包长度必须严格等于 `ESC_FOE_DATA_SIZE`（在 128 字节邮箱下为 116 字节）；
   - 主站按序号顺序下发 `FOE_OP_DATA`（opcode=0x03, packetnumber=1, 2, ... N）；
   - 从站逐包确认 `FOE_OP_ACK`（packetnumber=1, 2, ... N）。
3. **结束收尾阶段 (Terminal Packet) 与原生 SOES 行为约束**：
   - **非零短包结束路径 (0 < 长度 < 116 字节)**：
     SOES 源码在短包分支严格核验 `FOE_fclose()` 的返回值；若核心机双重 CRC32 校验失败或驱动 `finalize()` 失败，SOES 立即触发 Abort (0x800B) 报错；若校验成功方回复最后一包的成功 ACK，完成升级生命周期闭环。
   - **零长度包结束路径 (长度 == 0 字节) 的原生缺陷与限制**：
     原生 SOES 在收到零长度结束包时，调用 `FOE_fclose()` 后直接丢弃其返回值并无条件发送成功 ACK。**在此路径下，主站收到成功 ACK 不能作为 finalize 成功或固件有效的凭据**！
   - **保持现有动态文件名协议的实用限制与可验证措施**：
     在保持既有 `<PROJECT_NAME>_<TARGET>_V<M>.<m>.<p>.<b|r>.efw` 动态文件名协议不变的前提下，推荐采取以下双重工程防线：
     1. *主站发送端防线 (推荐措施)*：若固件容器大小恰好为 116 字节整数倍，主站发送端应避免下发 0 长度包结束，而是通过将最后一次传输拆分为两个非零短包（例如 58 + 58 字节），保证最后一包必然是非零短包，从而走 SOES 严格检查 `FOE_fclose()` 返回值的闭环防御分支，确保主站可直接通过总线 ACK/ERR 捕获最终提交结果；
     2. *升级结果状态后验 (兜底措施)*：若主站无法避免 0 长度结束包，主站不可仅凭最后一包 ACK 断言升级成功，应在升级结束后请求切换从站状态至 INIT/PREOP（利用适配层的 `post_state_change_hook`，固件未就绪时触发 `ALERR_NOVALIDFIRMWARE (0x0029)` 拦截），或通过 CoE 字典读取固件有效标志与版本号进行状态后验。

---

## 6. 宿主集成验证套件与覆盖场景

宿主集成测试源码位于 `test_bootloader_integration.c`，覆盖 5 大场景：
1. **场景 1: 有效固件端到端升级闭环**：
   - 构造包含 64 字节 `.efw` 容器头与 128 字节有效载荷的标准固件镜像；
   - 经由真实 SOES 发送 WRQ -> DATA 1 (116 字节满包) -> DATA 2 (76 字节短包)；
   - 断言 Flash 写入内容与载荷逐字节完全吻合，固件有效标记置位。
2. **场景 2: 工程标识与硬件版本不匹配安全拦截**：
   - 2A: 文件名匹配但容器头 `project_id` 不符，阶段 3 拦截并回复 Abort `0x800B`；
   - 2B: 容器头 `hardware_id` 不符，拦截并回复 Abort `0x800B`；Flash 保持未擦除状态。
3. **场景 3: 目标存储驱动硬件故障保护**：
   - 3A: 注入 Flash 擦除失败，即时熔断，触发 Abort 并调用驱动 `abort()`；
   - 3B: 注入 Flash 写入失败，即时熔断，触发 Abort 且保持介质无效标记。
4. **场景 4: 中途异常中止与自愈恢复重试**：
   - 传输中途主站发送 ERR 报文中断当前升级，SOES 与核心机干净复位回待命态；
   - 主站重新发起全新升级流程，全流程顺利完成并成功提交。
5. **场景 5: 写入物理基地址本地硬编码隔离验证**：
   - 验证全部写入操作严格受限于 `APP_FLASH_BASE_ADDR` 与 `APP_FLASH_MAX_SIZE`；
   - 验证 Bootloader 自身存储区（`0x08000000 ~ 0x0801FFFF`）所设哨兵数据完好无损。

---

## 7. 重要声明与边界区分

### 7.1 宿主验证 vs 物理硬件/真实主站实机验证
- **【已完成宿主端验证】**：本示例与集成测试在 macOS/Linux 宿主环境通过 GCC/Clang 编译器结合内存 Flash 仿真与虚拟硬件端口运行，直接链接真实 SOES `esc_foe.c` 与 `esc.c`，完整验证了逻辑契约、状态机流转与异常处理；
- **【尚未真机实测】**：
  1. 片内 Flash 物理擦写总线冻结（Bus Stall）与单步擦除粒度优化；
  2. 外部 SPI NOR Flash 物理回读耗时与主站通信超时窗口；
  3. 真实 ESC 从站芯片（如 Beckhoff ET1100、Microchip LAN9252、ASIX AX58100）的物理 PDI 中断与总线交互；
  4. 真实 EtherCAT 主站（如 Beckhoff TwinCAT、SOEM 现场主站）现场网线联调。

### 7.2 SOES 获取方式与版本说明
- **获取方式**：用户请直接从官方开源仓库获取 SOES 源码：
  ```bash
  git clone https://github.com/openEtherCATsociety/SOES.git /path/to/soes
  ```
- **基线版本**：适配层与本参考示例基于 SOES v3.1.0 / master 分支结构验证；
- **无第三方源码分发**：本仓库遵循严格的合规性与洁净原则，**不包含、不复制、不分发任何 SOES 第三方源码**。构建与测试时通过环境变量（如 `SOES_DIR=/path/to/soes`）动态指定外部源码路径。
