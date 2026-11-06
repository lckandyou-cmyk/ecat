> 状态：研发阶段。核心升级状态机已闭环；SOES 适配层已接通真实 SOES `esc.c` + `esc_foe.c` 宿主测试 Harness，实现了基于前后置邮箱嗅探与中和钩子的零侵入接入方案，并通过 11 组核心协议行为及 9 大深度风险（密码时序、信箱计数保持、WAIT_FOR_DATA 自愈、多槽隔离等）宿主验证；提供可编译的最小 Bootloader 接入参考与集成验证套件（`examples/bootloader_soes/`）。物理硬件板卡与现场真实主站联调待现场开展，未经真机实测前不可用于生产。

# ecat (Universal EtherCAT FoE Upgrade Library)

面向嵌入式 EtherCAT 从站的零 malloc、可移植 **FoE (File over EtherCAT)** 固件升级通用核心库。

---

## 📌 当前开发阶段与研发状态

- **规范版本**：遵循 `docs/spec/efw-format-v0.1.md` 固件容器格式规范与 `docs/architecture/SAD-v0.1.md` 系统架构设计。
- **当前阶段**：
  - **阶段 A（固件解析与核验）**：已完成。支持 64 字节 `.efw` 头部的三阶段反序列化、小端安全读写、双向元数据校验及 IEEE 802.3 CRC32 运算。
  - **阶段 B（核心升级状态机与适配）**：核心机在宿主机桩上验证；SOES 适配层已接通真实 SOES `esc.c` + `esc_foe.c` 宿主 Harness（支持前置嗅探与后置中和钩子），完成 11 项核心行为与 9 大深度风险（密码时序、信箱计数保持、WAIT_FOR_DATA 自愈等）宿主验证；提供最小 Bootloader 接入参考 (`examples/bootloader_soes/`)；Beckhoff SSC 抽象适配层已规划并具备 Mock 契约测试；物理硬件板卡与现场真实主站联调待现场开展。
  - **全量单元测试与内存安全**：在 Apple clang 与 GNU gcc-16（含 `-fsanitize=address,undefined`）下全量自动化测试通过（测试断言与夹具统计见 make test 输出）。

---

## 🛡️ 设计目标（未经硬件验证）

1. **零动态内存分配 (Zero malloc)**：
   - 全流程纯 C99 实现，严禁调用 `malloc`/`free`，所有缓冲与上下文均由静态或局部内存分配，内存占用确定可控。
2. **严格物理分区信任**：
   - 物理存储基地址与容量上限由从站本地静态配置管理，设计上不信任固件容器携带的物理地址，防止越界踩踏 Bootloader。
3. **消除内部自指针与安全复制契约**：
   - 内部项目名通过静态数组持有，消除了内部自身指针自引用；
   - **非活动状态 (IDLE)** 支持安全按值复制/模板分发；**活动会话状态**严禁复制，防止底层物理硬件驱动冲突。
4. **防变砖生命周期契约 (Strict Driver Contract)**：
   - **`init()` 失败**：驱动自身初始化未完成，由驱动内部自行清理未初始化半成品，核心机不调用 `abort()`；
   - **`init()` 成功**：驱动资源进入活跃期。此后任何阶段失败（`invalidate` 失败、擦除失败、写入失败、CRC 不匹配、回读校验错误、主站中止），核心机设计规则为**调用且仅调用一次** `abort()` 恢复硬件安全状态；
   - **`finalize()` 成功**：固件有效标记置位，升级生命周期正常闭环。finalize 成功后，在同一会话内不再调用 abort；新的 WRQ 开始新会话。
5. **失效标记先行 (Invalidate First)**：
   - 头部校验全部通过后，在执行物理扇区擦除前，先调用 `invalidate()` 清除原有有效标记。若后续发生断电事故，能否停留在 Bootloader 等待重新升级取决于驱动 invalidate 写入的持久性和 Bootloader 启动时的检查，本库不提供该保证。
6. **按需覆盖擦除与物理回读双重防线**：
   - 流式接收时根据写入边界按需调用 `erase_to()`，并严格校验 `erased_end >= need_end`；
   - 升级提交前执行“流 CRC 比对（捕获传输损坏）+ 物理介质 read 回读增量比对（捕获存储坏块）”，双重校验一致方可 `finalize()`。

---

## 🔌 协议栈接入与约束说明

### 1. Simple Open EtherCAT Slave (SOES) 接入
- **适配层现状**：已在 `src/adapters/ecat_foe_soes.c` 与 `include/ecat_foe_soes.h` 实现基于前后置邮箱嗅探与中和钩子的零侵入接入方案（`ecat_foe_soes_pre_process_hook` 与 `ecat_foe_soes_post_process_hook`）。**在宿主测试 Harness 中直接编译链接真实 SOES `esc.c` 与 `esc_foe.c` 源码及硬件端口桩，覆盖基于端口桩的邮箱发送与多槽轮转路径**。通过前置嗅探暂存 WRQ、后置核验响应并就地将异常中和为标准 `FOE_OP_ERR (0x8000)`、严格保持原有信箱头计数器 `mbxcnt`，并通过 `ecat_foe_soes_set_foe_reset_fn()` 注入 `FOE_init` 实现了立即复位自愈，消除了原生 SOES 无 WRQ 回调的缺陷与假 ACK 风险。提供配套最小 Bootloader 接入参考与 5 大场景集成验证（`examples/bootloader_soes/`）。【仍属于宿主环境仿真，物理硬件板卡与现场真实主站动态联调待现场开展】。
- **SOES 获取方式与版本哈希基线**：
  - 用户请直接从官方开源仓库获取 SOES 源码：`git clone https://github.com/openEtherCATsociety/SOES.git`
  - 宿主测试套件使用的 SOES 源码文件提取自本地散文件目录，无上游 git 元数据，无法确定具体的上游 commit 或发布标签。
  - 严禁将持续演进的 master 分支假设为固定版本。测试套件仅对以下 4 个关键文件的 SHA-256 哈希基线负责：
    - `esc.c`: `1ef43fde2f2cf3306d16e8f8e3e86a1be20a013b0bffd833711749575f60c42c`
    - `esc.h`: `9c76c2d4a20b4f827539a4416383354cb45e95a9d086f444a26a0bbb5ff9a579`
    - `esc_foe.c`: `cd1709f103e5c4b786fb0be2857760f0e180c9123a553a2efa410accbf17a2b7`
    - `esc_foe.h`: `84f5011c258d1e25d8d9ab22bfb74f4ab45b1e6b01f3a6be13e5cf2138034eb7`
  - 用户可运行 `sha256sum soes/esc.c soes/esc.h soes/esc_foe.c soes/esc_foe.h` 比对基线；若上游后续变动导致哈希或函数行为改变，以本库适配层接口契约为准。
  - **无第三方源码分发**：本仓库遵循严格的合规性与洁净原则，**不包含、不分发任何 SOES 第三方源码**。测试时通过环境变量（如 `SOES_DIR=/path/to/soes`）指定外部源码目录。
- **原生协议栈限制与技术约束 (宿主 harness 观察与工程约束)**：
  - *【宿主观察】文件名匹配机制与空名/短前缀风险*：原生 SOES 在 `esc_foe.c` 的 WRQ 文件名匹配逻辑中采用 `0 == strncmp (foe_file_name, foe_cfg->files[i].name, num_chars)`。其匹配判定由接收长度 `num_chars` 决定前缀匹配，而非全名比对。宿主 harness 实测证实短前缀会误命中首个具有该前缀的表项，且空文件名（`num_chars == 0`）无条件命中 `files[0]`；
  - *【宿主观察】0 长度结束包与写回调错误忽略*：原生 SOES 在 `esc_foe.c` 的 0 长度结束包分支中直接忽略了 `FOE_fclose()` 的返回值并发送成功 ACK。宿主 harness 实测证实短包分支会严格触发 `FOE_abort(0x800B)`，但 0 长度结束包写失败无法通知主站；
  - *【语法核实】关闭 RRQ 的真实编译方式（不可定义为 0）*：原生 SOES 使用 `#ifdef FOE_READ_SUPPORTED` 控制代码编译。**不可通过定义为 0（如 `-DFOE_READ_SUPPORTED=0`）来禁用**！因 C 预处理器 `#ifdef` 仅判断宏是否存在，定义为 0 仍会被判定为真而编入 RRQ 逻辑并引发缓冲区越界读 (Buffer Over-read) 风险。**必须保持该宏完全未定义或显式 `#undef`**，方可走 `default:` 分支直接返回 `0x8000 (FOE_ERR_NOTDEFINED)`。
- **源码与规范引用清单**：关于 SOES、LAN9252 手册及 ETG 规范的全局行号引用与核验状态，详见 [soes-integration-options.md 第 6 节](docs/architecture/soes-integration-options.md#6-全局行号与外部规范引用盘点表-reference-inventory)。

### 验证矩阵 (Verification Matrix)

| 规范声明 / 功能特性 | 证据类型 | 验证位置 / 证据文件 | 状态与边界说明 |
| :--- | :--- | :--- | :--- |
| **核心升级状态机流式校验** | 宿主单元测试 (RAM 驱动) | `tests/test_core.c` | 通过，宿主内存仿真，MCU 物理足迹未测 |
| **.efw 二进制头解构与安全校验** | 宿主单元测试 | `tests/test_efw.c` | 通过，覆盖大小端、对齐与异常截断 |
| **内存安全与受限栈 (1MB/8MB)** | ASan / 宿主受限栈测试 | `tests/test_runner_core` | 通过，无泄漏、无越界，宿主栈深度达标 |
| **SOES WRQ 不调数据写回调** | 宿主 Harness (真实 `esc_foe.c`) | `tests/test_soes_real_harness.c` (Test 1) | 宿主观察，证实 WRQ 阶段不触发 `write_function` |
| **SOES 短前缀匹配特性** | 宿主 Harness (真实 `esc_foe.c`) | `tests/test_soes_real_harness.c` (Test 2) | 宿主观察，证实短前缀匹配，非硬件总线测试 |
| **SOES 零长度包忽略写回调错误** | 宿主 Harness (真实 `esc_foe.c`) | `tests/test_soes_real_harness.c` (Test 3) | 宿主观察，证实 0 长度包忽略 `FOE_fclose` 失败回复 ACK |
| **SOES 前后置钩子端到端闭环** | 宿主 Harness (真实 `esc.c` + `esc_foe.c`) | `tests/test_soes_real_harness.c` (Test 5 / 风险 1~9) | 宿主观察，通过前后置钩子实现零侵入端到端闭环，非硬件总线测试 |
| **最小 Bootloader 接入参考验证** | 宿主集成测试 (真实 SOES) | `examples/bootloader_soes/test_bootloader_integration.c` | 通过，覆盖有效升级、项目/硬件不符、存储故障、中途异常与本地基地址裁决，宿主内存仿真 |
| **SOES 真实从站硬件与主站联调** | 物理硬件 / 现场总线 | - | 【未核实/待开发与真机动态验证】 |

### 2. Beckhoff Slave Stack Code (SSC) 接入
- **状态**：【未核实 / 待开发】。目前适配层仅规划了接口边界，尚未包含面向真实 SSC 协议栈的回调实现。

---

## 🔐 完整性校验与安全边界说明

- **传输与介质完整性校验 (CRC32)**：
  - 固件容器解析与 Flash 驱动采用标准 IEEE 802.3 CRC32 算法，包含数据流实时累加校验与物理介质回读比对；
  - 该机制仅用于捕获通信线路干扰产生的偶发误码以及 Flash 介质的物理坏块或写入不一致；
- **非密码学身份认证 (No Cryptographic Authentication)**：
  - CRC32 **不具备**任何防篡改、防伪造、抗碰撞或防重放攻击的密码学保护能力；
  - 固件头中的 `project_id` 与 `hardware_id` 仅用于工程标识与硬件版本的兼容性匹配过滤，不代表设备身份认证或许可凭据；
- **未实现能力声明**：
  - 本库当前**未实现任何非对称数字签名（如 Ed25519、ECDSA）或密码学身份认证**，亦未集成安全启动（Secure Boot）密钥链；
  - 严禁将本库的 CRC32 校验或 ID 比对当作防御恶意固件注入的安全屏障；若目标系统存在防恶意攻击需求，用户须在固件载荷外增加密码学签名校验，或在主从站间建立安全通信信道。

---

## 🚀 最短上手路径 (Quickstart)

### 1. 运行核心机自动化测试
需要 C 编译器、Python 3、make 和用于 PDF 引文检查的 `pdftotext`：
```bash
# 使用 clang 构建并运行核心测试套件
make clean && make test CC=clang

# 启用 ASan/UBSan 内存安全性检查
make test-asan CC=clang
```

### 2. （可选）基于真实 SOES 源码运行宿主集成测试
用户自行克隆官方 SOES 源码后，指定目录运行真实协议栈宿主集成测试：
```bash
# 获取官方 SOES 源码
git clone https://github.com/openEtherCATsociety/SOES.git /path/to/SOES

# 运行真实 SOES 邮箱协议与 Bootloader 集成测试
make test-soes SOES_DIR=/path/to/SOES/soes
make test-bootloader SOES_DIR=/path/to/SOES/soes
```

### 3. 生成与打包固件镜像
使用内置脚本将编译好的应用固件打包为符合规范的 `.efw` 容器：
```bash
python3 tools/efw_pack.py pack \
  --bin path/to/app.bin \
  --project-id 0x10010001 \
  --project-name SERVO_DRV \
  --hardware-id 0x00000001 \
  --target MCU \
  --fw-version 1.0.0.0 \
  --out SERVO_DRV_MCU_V1.0.0.0.efw
```

---

## 📋 首次移植准备与清单 (Porting Guide)

首次将本库移植到物理硬件或新工程时，请完成以下准备与适配：

### 1. 板级分区与工程参数配置 (`bootloader_config.h`)
在工程配置中定义从站设备自身的物理边界与工程信息（参见 `examples/bootloader_soes/bootloader_config.h`）：
- `APP_FLASH_BASE_ADDR`: 应用固件物理存储起始地址（本地硬编码裁决，不信任外部报文）；
- `APP_FLASH_MAX_SIZE`: 应用分区容量上限（严格边界防越界踩踏）；
- `APP_FLASH_SECTOR_SIZE`: 目标介质的物理扇区擦除粒度；
- `APP_FLASH_WRITE_UNIT`: 目标介质的最小写入对齐单位（如 4 或 8 字节）；
- `BOOTLOADER_PROJECT_ID` / `BOOTLOADER_HARDWARE_ID`: 本地预设的工程标识符与硬件板型码。

### 2. 实现目标存储驱动接口 (`ecat_foe_target_driver_t`)
用户需基于目标板的 Flash HAL 库实现以下 7 个驱动函数（参考 `examples/bootloader_soes/bootloader_flash_driver.c`）：
1. `init`: 初始化存储外设，失败时自行释放资源；
2. `invalidate`: 清除原有固件有效标记（早于擦除执行）；
3. `erase_to`: 扇区按需擦除，确保擦除范围覆盖当前请求；
4. `write`: 写入数据至本地基地址偏移处；
5. `read`: 从物理介质读回已写入数据用于回读比对；
6. `finalize`: 双重 CRC 校验一致后提交固件有效标记；
7. `abort`: 异常发生时恢复外设至安全态，保持固件为无效态；

### 3. 协议栈串行调度接入
在从站主循环或轮询任务中，严格按照单执行流原子互斥顺序调用：
- `ecat_foe_soes_pre_process_hook`: 前置检查报文、暂存 WRQ；
- 协议栈原生处理进程（如 SOES 的 FoE 进程）；
- `ecat_foe_soes_post_process_hook`: 后置核验并中和异常响应。

---

## ⚠️ 已知限制与未覆盖范围 (Known Limitations)

目前全部代码、状态机及适配层均在 **macOS 宿主机环境** 下通过 GCC/Clang 编译器结合 RAM 模拟驱动（`mock_driver`）完成仿真测试与 ASan/UBSan 内存安全性检验。以下事项受限于当前开发阶段，明确未覆盖或具备已知约束：
1. **真实硬件板卡与现场主站联调待现场开展**：
   - **MCU 片内 Flash 擦写总线冻结 (Bus Stall)**：片内 Flash 擦除时可能导致 CPU 停止响应中断或协议栈丢包，需现场实测确认主站超时时间与单步擦除粒度；
   - **外部低速 SPI Flash 物理全量回读耗时**：大容量固件（如 2MB~4MB FPGA 镜像）在最后一次回调中同步回读并计算 CRC32 的总耗时是否会突破 TwinCAT 单包超时阈值；
   - **真实 ESC 芯片与主站联调**：如 Beckhoff ET1100、AX58100 或 LAN9252 芯片与 TwinCAT/EtherCAT 运动控制器之间的现场升级；
   - **MCU 足迹未测 (Footprint)**：代码与适配层在真实嵌入式交叉编译器（如 GCC ARM Embedded / armclang）下的实际 Flash 占用（`.text` + `.rodata`）、静态 RAM 占用（`.data` + `.bss`）以及运行时栈深度尚未经过实机测量，明确处于“MCU 足迹未测”状态。
2. **原生 SOES 零长度包忽略写回调错误限制**：
   - 原生 SOES 在收到零长度结束包时，调用 `FOE_fclose()` 后直接忽略其返回值并发送成功 ACK。主站收到最后一包 ACK 无法证明固件成功提交，集成方需要提供并验证主站可读取的状态标记。
3. **原生 SOES 短前缀匹配特性**：
   - 原生 SOES 采用接收长度前缀匹配而非全名比对，空文件名默认命中首个表项。从站配置表文件名需避免前缀重叠。
4. **单执行流互斥要求**：
   - SOES 协议栈及其前后置钩子非线程安全，必须在同一执行流或关中断保护下串行执行，禁止并发或中断插入。
5. **分包长度约束**：
   - 数据发送阶段非末尾包长度必须严格等于邮箱载荷上限，不可中途将满包拆分为短包，首个短包即标志文件传输结束。
6. **无密码学身份认证**：
   - CRC32 仅为偶发数据损坏检测，无防伪造、防篡改能力。

---

## 🤝 问题反馈与社区协作 (Issue Reporting)

若在移植或使用过程中遇到问题，欢迎提交 GitHub Issue：
- **Bug 报告**：请使用 [Bug Report 模板](.github/ISSUE_TEMPLATE/bug_report.md)，提供复现步骤、环境信息（ECAT commit、SOES 源码来源与 SHA-256 哈希、硬件板卡型号、主站版本及编译器）以及脱敏日志；
- **功能需求**：请使用 [Feature Request 模板](.github/ISSUE_TEMPLATE/feature_request.md)；
- **安全警告**：提交任何抓包和日志前，**切勿包含**真实的 FoE 通信密码、私钥、证书凭据或专有商业固件二进制！

---

## 🛠️ 构建与测试

### 环境依赖
- C99 兼容编译器：`clang` 或 `gcc` (推荐 GCC 10+)
- Python 3.8+ (用于固件打包与测试向量生成)
- `pdftotext` (poppler-utils 套件，用于厂商 PDF 数据手册引文自动化核验)
- `make`

### 工具说明
- `gen_pdf_cite_table.py --update` 会回写 manifest 的 `doc_line` 和 `line_text_hash`，防漂移依赖 `--check` 与 PDF 校验。

---

## 📂 源码目录结构

```text
.
├── include/
│   ├── ecat_foe_conf_template.h # 静态工程配置模板
│   ├── ecat_foe_core.h          # 核心状态机与 API 接口定义
│   ├── ecat_foe_efw.h           # .efw 容器格式解析接口
│   ├── ecat_foe_soes.h          # SOES 协议栈适配层接口
│   └── ecat_foe_target.h        # 目标存储介质抽象驱动接口
├── src/
│   ├── adapters/
│   │   └── ecat_foe_soes.c      # SOES 适配层实现
│   ├── ecat_foe_core.c          # 核心升级状态机实现 (纯 C99, 零 malloc)
│   └── ecat_foe_efw.c           # .efw 容器头反序列化与 CRC32 实现
├── examples/
│   └── bootloader_soes/         # 基于真实 SOES 的最小 Bootloader 接入参考
│       ├── bootloader_config.h  # 板级分区与工程配置（本地物理基地址裁决）
│       ├── bootloader_flash_driver.c / .h # 遵循生命周期契约的 Flash 驱动
│       ├── bootloader_app.c / .h# 核心机与 SOES 串行驱动应用
│       ├── bootloader_master_flow.c / .h # 主站发送端报文交互流
│       ├── test_bootloader_integration.c # 5 大场景宿主集成测试套件
│       └── README.md            # Bootloader 移植与接入参考文档
├── tools/
│   └── efw_pack.py              # .efw 固件容器打包与对齐填充工具
├── tests/
│   ├── mock_driver.c / .h       # 具备扇区擦除语义与故障注入的 RAM 驱动
│   ├── test_core.c              # 核心状态机全量功能测试套件
│   ├── test_efw.c               # .efw 容器解析单元测试套件
│   └── generate_vectors.py      # 二进制测试固件生成器
├── docs/
│   ├── architecture/SAD-v0.1.md # 系统架构设计说明书
│   ├── spec/efw-format-v0.1.md  # 固件容器格式规范
│   └── open-issues.md           # 待决事项与跟踪记录
├── Makefile                     # 自动化构建与测试脚本
└── README.md                    # 本文件
```
