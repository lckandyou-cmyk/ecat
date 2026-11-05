> 状态：草案，尚不可用。仅在宿主机单元测试中验证。SOES 适配层未接通真实协议栈（不调用 on_wrq 时首块数据会被拒绝，见 test_reproduce_soes_data_callback_without_wrq_stream_rejection）。未经硬件验证，不可用于生产。文件名协议架构待定。

# ecat (Universal EtherCAT FoE Upgrade Library)

面向嵌入式 EtherCAT 从站的零 malloc、可移植 **FoE (File over EtherCAT)** 固件升级通用核心库。

---

## 📌 当前开发阶段与研发状态

- **规范版本**：遵循 `docs/spec/efw-format-v0.1.md` 固件容器格式规范与 `docs/architecture/SAD-v0.1.md` 系统架构设计。
- **当前阶段**：
  - **阶段 A（固件解析与核验）**：已完成。支持 64 字节 `.efw` 头部的三阶段反序列化、小端安全读写、双向元数据校验及 IEEE 802.3 CRC32 运算。
  - **阶段 B（核心升级状态机与适配）**：核心机在宿主机桩上验证，SOES 适配层未接通真实协议栈。包含驱动生命周期互斥、水位按需擦除、双重 CRC 防线、防回滚与自愈机制。
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
- **适配层现状**：已在 `src/adapters/ecat_foe_soes.c` 与 `include/ecat_foe_soes.h` 实现接口转换打样与单测桩。**已有宿主测试 harness 编译链接真实 SOES `esc_foe.c` 并观察到短前缀匹配、0 长度包忽略写回调错误等真实协议栈行为（均为宿主观察，非物理硬件测试）**。在 harness 端到端测试（Test 5）中，测试代码手动调用了 `ecat_foe_soes_on_wrq`，但在原生 SOES 生产现实中并没有调用该 WRQ 钩子的调用方【仍属于宿主环境仿真，现场硬件与真实主站动态联调尚未开展】。
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
| **SOES 驱动已注册回调端到端** | 宿主 Harness (真实 `esc_foe.c`) | `tests/test_soes_real_harness.c` (Test 5) | 宿主观察，Test 5 手动调用 `on_wrq`，生产中无调用方 |
| **SOES 真实从站硬件与主站联调** | 物理硬件 / 现场总线 | - | 【未核实/待开发与真机动态验证】 |

### 2. Beckhoff Slave Stack Code (SSC) 接入
- **状态**：【未核实 / 待开发】。目前适配层仅规划了接口边界，尚未包含面向真实 SSC 协议栈的回调实现。

---

## ⚠️ 硬件验证限制 (Limitations)

目前全部代码、状态机及适配层均在 **macOS 宿主机环境** 下通过 GCC/Clang 编译器结合 RAM 模拟驱动（`mock_driver`）完成仿真测试与 ASan/UBSan 内存安全性检验。
以下事项受限于当前开发阶段，**尚未在物理硬件上完成实机测试**：
1. **【待真机验证】MCU 片内 Flash 擦写总线冻结 (Bus Stall)**：片内 Flash 擦除时可能导致 CPU 停止响应中断或协议栈丢包，需实测确认主站超时时间与单步擦除粒度；
2. **【待真机验证】外部低速 SPI Flash 物理全量回读耗时**：大容量固件（如 2MB~4MB FPGA 镜像）在最后一次回调中同步回读并计算 CRC32 的总耗时是否会突破 TwinCAT 单包超时阈值；
3. **【待真机验证】真实 ESC 芯片与主站联调**：如 Beckhoff ET1100、AX58100 或 LAN9252 芯片与 TwinCAT/EtherCAT 运动控制器之间的现场升级；
4. **【待真机验证】MCU 足迹未测 (Footprint)**：代码与适配层在真实嵌入式交叉编译器（如 GCC ARM Embedded / armclang）下的实际 Flash 占用（`.text` + `.rodata`）、静态 RAM 占用（`.data` + `.bss`）以及运行时栈深度尚未经过实机测量，明确处于“MCU 足迹未测”状态。

---

## 🛠️ 构建与测试

### 环境依赖
- C99 兼容编译器：`clang` 或 `gcc` (推荐 GCC 10+)
- Python 3.8+ (用于固件打包与测试向量生成)
- `pdftotext` (poppler-utils 套件，用于厂商 PDF 数据手册引文自动化核验)
- `make`

### 运行单元测试
```bash
# 默认使用 clang 构建并运行全量测试
make clean && make test

# 使用 GNU GCC 构建并运行全量测试
make clean && make test CC=gcc-16

# 启用 AddressSanitizer 与 UndefinedBehaviorSanitizer 内存安全检查
make test-asan CC=clang
```

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
