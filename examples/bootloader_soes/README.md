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

### 1.3 可选设备端签名验签
当前 EFW v2 支持可选 Ed25519ph 签名。未配置时，参考示例继续接受合法未签名包；启用验签时必须在本地配置中接入 `ecat_foe_signature_verifier_t` 和受保护公钥，缺少验签回调或找不到 Key ID 时初始化/升级失败关闭。签名只在最终有效标记或防回滚联合提交前，对目标存储回读的载荷完成验证。

参考 Bootloader 示例目前**没有接入密码学后端，也没有配置设备信任公钥**，因此它只演示未签名兼容路径，不是已启用验签的产品配置。可替换的 Monocypher 适配骨架及密钥表接法见 [`examples/crypto/README.md`](../crypto/README.md)；格式与唯一签名编码见 [`docs/spec/efw-format-v0.2.md`](../../docs/spec/efw-format-v0.2.md)。签名是包认证，不是 TwinCAT FoE 传输密码，也不自动启用防回滚。

单分区设备可能在最终验签前已经擦除旧固件；签名失败会阻止新镜像提交，但不能恢复被擦除的旧镜像，也不提供断电恢复或防变砖保证。旧版解析器可能忽略新扩展，签名包不能强制旧设备验签。

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
- `APP_FLASH_MAX_SIZE`: `256 * 1024`（应用分区 256 KB payload 容量上限）；SOES 文件表的 `max_data` 包含 EFW 头部与 payload，并在初始化时检查 32 位加法溢出。核心和驱动仍按 payload 容量限制存储。
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
SOES 协议栈本身非多线程安全，接入必须遵循单执行流原子互斥契约。调用 `bootloader_app_step` 时，接收指针必须指向 SOES 实际消费的输入槽位 0，长度必须是实际收到的字节数；无输入时传 `NULL, 0`。前置钩子返回 `ECAT_FOE_SOES_ERR_UNSAFE_DISPATCH` 时绝不能调用 `ESC_foeprocess`：示例中止活跃会话并尝试复位 FoE，不发送响应，邮箱所有者须丢弃该输入槽位，再按设备策略恢复。普通 WRQ 文件名拒绝仍交由 SOES 生成协议错误。使用原生 `ESC_mbxprocess` 时仍须确保其邮箱长度校验确实发生在 FoE 分发前：
```text
   +------------------------------------------------------------+
   | 1. ecat_foe_soes_pre_process_hook                          |
   |    - 校验实际槽位和接收长度；只读预检并暂存 WRQ            |
   +------------------------------------------------------------+
               | 不安全：中止/复位，邮箱所有者丢弃输入
               | 安全（包括普通 WRQ 预检拒绝）
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
- **状态机复位回调注入**：调用 `ecat_foe_soes_set_foe_reset_fn(&adapter, FOE_init)`。当邮箱后置钩子中和异常 ACK，或 BOOT→INIT/PREOP/SAFEOP/OP 状态钩子中止活跃会话时，调用 `FOE_init()` 将 SOES 复位回 `FOE_READY`；须先按主站及设备状态机恢复邮箱，才能重新传输；
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
     1. *主站发送端限制*：非末尾的数据包必须是满包；不能将一个满包拆成两个短包，因为第一个短包就会结束传输。容器大小恰好是满包整数倍时，零长度结束包的 ACK 无法证明提交成功，必须另行核验结果；
     2. *升级结果状态后验*：主站不能仅凭最后一包 ACK 断言成功。集成方需要提供并验证可供主站读取的持久化升级结果或固件有效标志；本示例没有实现此类 CoE 对象，也未验证现场主站的状态读取流程。

---

## 6. 宿主集成验证套件与覆盖场景

宿主集成测试源码位于 `test_bootloader_integration.c`，覆盖 5 大场景：
1. **场景 1: 有效固件端到端升级闭环**：
   - 构造包含 148 字节 v2 `.efw` 容器头与 128 字节有效载荷的标准固件镜像；
   - 经由真实 SOES 发送 WRQ -> DATA 1/2 (各 116 字节满包) -> DATA 3 (44 字节短包)；
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

### 7.2 SOES 获取方式与版本哈希基线
- **获取方式**：用户请直接从官方开源仓库获取 SOES 源码：
  ```bash
  git clone https://github.com/OpenEtherCATsociety/SOES.git /path/to/SOES
  git -C /path/to/SOES checkout --detach 6ef7b9415e936c061ad30626ca00004bdde2ac62
  soes_test_dir=$(mktemp -d)
  cp /path/to/SOES/soes/esc.c /path/to/SOES/soes/esc.h \
  /path/to/SOES/soes/esc_foe.c /path/to/SOES/soes/esc_foe.h "$soes_test_dir/"
  make test-soes SOES_DIR="$soes_test_dir"
  make test-bootloader SOES_DIR="$soes_test_dir"
  ```
- 测试使用四个上游文件及本仓库 `tests/soes_bsp` 的宿主配置和桩。完整 SOES 源码目录含有优先级更高的其他头文件，直接指定该目录会导致构建失败；测试结束后可自行删除临时目录。这不构成完整协议栈或真实硬件验证。
- **测试源码版本说明与哈希基线**：
  - 原始测试文件来自无 Git 元数据的本地散文件；上游提交 `6ef7b9415e936c061ad30626ca00004bdde2ac62` 的下列四个文件与原基线 SHA-256 一致。此比对不能证明原始散文件的完整来源或其他文件一致；
  - 严禁将持续滚动的 master 主干假设为固定版本。为确保测试结果的可复现性，测试套件仅针对以下 4 个关键文件的 SHA-256 哈希基线负责：
    - `esc.c`: `1ef43fde2f2cf3306d16e8f8e3e86a1be20a013b0bffd833711749575f60c42c`
    - `esc.h`: `9c76c2d4a20b4f827539a4416383354cb45e95a9d086f444a26a0bbb5ff9a579`
    - `esc_foe.c`: `cd1709f103e5c4b786fb0be2857760f0e180c9123a553a2efa410accbf17a2b7`
    - `esc_foe.h`: `84f5011c258d1e25d8d9ab22bfb74f4ab45b1e6b01f3a6be13e5cf2138034eb7`
  - 用户获取 SOES 源码后，可通过 `sha256sum` 或校验脚本比对哈希；若上游后续变动导致哈希或函数行为不一致，需以本库适配层声明的协议栈接口约束为准；
- **无第三方源码分发**：本仓库遵循严格的合规性与洁净原则，**不包含、不复制、不分发任何 SOES 第三方源码**。构建与测试时通过环境变量（如 `SOES_DIR=/path/to/soes`）动态指定外部源码路径。

### 7.3 完整性、签名与安全边界
- **数据完整性校验**：固件容器解析与 Flash 驱动采用 IEEE 802.3 CRC32 算法，对比传输数据流与物理回读数据，仅用于捕获通信线路干扰带来的偶发误码与存储介质物理坏块；
- **可选密码学签名**：v2 可使用 Ed25519ph；签名需由可信发布流程生成，设备必须本地启用验签并配置受保护公钥。默认未启用验签时，签名包只检查格式/CRC，不认证发布者；
- **非密码学身份认证**：CRC32 不具备任何防恶意篡改、防伪造或防重放攻击的能力。签名验证也不代表镜像可以启动，启动镜像格式、安全启动及密钥轮换由产品负责；
- **软硬件兼容性标识**：`.efw` 头部中的 `project_id`、`hardware_id` 仅用于工程识别符与硬件板型的兼容性匹配过滤，不代表设备身份认证；
- **可用性限制**：验签在写入载荷之后、激活标记之前进行。单分区设备验签失败可能已经失去旧镜像；要获得掉电恢复需采用 A/B、暂存区或平台事务机制。
- **旧设备限制**：旧解析器可能拒绝 v2，也可能忽略未知扩展；不能依赖签名扩展强制其验签。
- **未验证范围**：本仓库尚未在指定 MCU 上编译密码学后端，也没有 Flash、RAM、栈或耗时实测；真实 SSC、主站和硬件升级仍待具体产品验证。
