# ecat (Universal EtherCAT FoE Upgrade Library)

面向嵌入式 EtherCAT 从站的高安全、零 malloc、可移植 **FoE (File over EtherCAT)** 固件升级通用核心库。

---

## 📌 当前开发阶段与研发状态

- **规范版本**：遵循 `docs/spec/efw-format-v0.1.md` 固件容器格式规范与 `docs/architecture/SAD-v0.1.md` 系统架构设计。
- **当前阶段**：
  - **阶段 A（固件解析与核验）**：已完成。支持 64 字节 `.efw` 头部的三阶段反序列化、小端安全读写、双向元数据校验及 IEEE 802.3 CRC32 运算。
  - **阶段 B（核心升级状态机与适配）**：已完成核心机与 SOES 适配桩验证。包含驱动生命周期互斥、水位按需擦除、双重 CRC 防线、防回滚与自愈机制。
  - **全量单元测试与内存安全**：在 Apple clang 与 GNU gcc-16（含 `-fsanitize=address,undefined`）下全量自动化测试通过（共 439 项断言无一失败）。

---

## 🛡️ 核心架构与安全保证

1. **零动态内存分配 (Zero malloc)**：
   - 全流程纯 C99 实现，严禁调用 `malloc`/`free`，所有缓冲与上下文均由静态或局部内存分配，内存占用确定可控。
2. **绝对物理分区信任**：
   - 物理存储基地址与容量上限完全由从站本地静态配置管理，绝不信任固件容器携带的物理地址，杜绝越界踩踏 Bootloader。
3. **防变砖生命周期契约 (Strict Driver Contract)**：
   - **`init()` 失败**：驱动自身初始化未完成，由驱动内部自行清理未初始化半成品，核心机不调用 `abort()`；
   - **`init()` 成功**：驱动资源进入活跃期。此后任何阶段失败（`invalidate` 失败、擦除失败、写入失败、CRC 不匹配、回读校验错误、主站中止），核心机严格保证**调用且仅调用一次** `abort()` 恢复硬件安全状态；
   - **`finalize()` 成功**：固件有效标记置位，升级生命周期正常闭环。核心状态机永久锁定，此后**绝不调用** `abort()`。
4. **失效标记先行 (Invalidate First)**：
   - 头部校验全部通过后，在执行物理扇区擦除前，必须先调用 `invalidate()` 清除原有有效标记。若后续发生断电事故，设备冷启动时强制停留在 Bootloader 等待重新升级。
5. **按需覆盖擦除与物理回读双重防线**：
   - 流式接收时根据写入边界按需调用 `erase_to()`，并严格校验 `erased_end >= need_end`；
   - 升级提交前执行“流 CRC 比对（捕获传输损坏）+ 物理介质 read 回读增量比对（捕获存储坏块）”，双重校验一致方可 `finalize()`。

---

## 🔌 协议栈接入与约束说明

### 1. Simple Open EtherCAT Slave (SOES) 接入
- **适配层实现**：已在 `src/adapters/ecat_foe_soes.c` 与 `include/ecat_foe_soes.h` 实现适配薄层。
- **原生协议栈限制与现状说明**：
  - *动态文件名匹配*：原生 SOES（`esc_foe.c:70-73`）使用静态精确匹配（`strncmp(foe_file_name, foe_cfg->files[i].name, num_chars)`），无外部 WRQ 文件名回调钩子。动态版本文件名无法通过原生静态匹配。协议架构是否调整为固定目标文件名（如 `SERVO_DRV_MCU.efw`）或引入轻量钩子尚待最终裁定，当前代码库保持分层解耦，暂不变更现有架构；
  - *0 长度包分支*：原生 SOES 在 0 长度结束包分支（`esc_foe.c:459-465`）中忽略了 `FOE_fclose()` 的返回值。适配层通过 `post_state_change_hook` 在主站尝试退出 BOOT 状态（`BOOT -> INIT`）时拦截并设置 AL Status Code `0x0014 (ALERR_NOVALIDFIRMWARE)`，防止错误固件被引导运行；
  - *RRQ 读取*：原生 SOES 的 RRQ 读取直接从静态 `foe_cfg->fbuffer` 获取（`esc_foe.c:121-133`），文件配置结构体中无 `read_function`，未开启 `FOE_READ_SUPPORTED` 宏时直接返回 `0x8000 (NOTDEFINED)`。

### 2. Beckhoff Slave Stack Code (SSC) 接入
- **状态**：【未核实 / 待开发】。目前适配层仅规划了接口边界，尚未包含面向真实 SSC 协议栈的回调实现。

---

## ⚠️ 硬件验证限制 (Limitations)

目前全部代码、状态机及适配层均在 **macOS 宿主机环境** 下通过 GCC/Clang 编译器结合 RAM 模拟驱动（`mock_driver`）完成仿真测试与 ASan/UBSan 内存安全性检验。
以下事项受限于当前开发阶段，**尚未在物理硬件上完成实机测试**：
1. **【待真机验证】MCU 片内 Flash 擦写总线冻结 (Bus Stall)**：片内 Flash 擦除时可能导致 CPU 停止响应中断或协议栈丢包，需实测确认主站超时时间与单步擦除粒度；
2. **【待真机验证】外部低速 SPI Flash 物理全量回读耗时**：大容量固件（如 2MB~4MB FPGA 镜像）在最后一次回调中同步回读并计算 CRC32 的总耗时是否会突破 TwinCAT 单包超时阈值；
3. **【待真机验证】真实 ESC 芯片与主站联调**：如 Beckhoff ET1100、AX58100 或 LAN9252 芯片与 TwinCAT/EtherCAT 运动控制器之间的现场升级。

---

## 🛠️ 构建与测试

### 环境依赖
- C99 兼容编译器：`clang` 或 `gcc` (推荐 GCC 10+)
- Python 3.8+ (用于固件打包与测试向量生成)
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
