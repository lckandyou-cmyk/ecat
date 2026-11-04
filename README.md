# ecat (Universal EtherCAT FoE Upgrade Library)

面向嵌入式 EtherCAT 从站的高安全、零 malloc、可移植 **FoE (File over EtherCAT)** 固件升级通用核心库。

---

## 📌 当前开发阶段与研发状态

- **规范版本**：遵循 `docs/spec/efw-format-v0.1.md` 固件容器格式规范与 `docs/architecture/SAD-v0.1.md` 系统架构设计。
- **当前阶段**：
  - **阶段 A（固件解析与核验）**：已完成。支持 64 字节 `.efw` 头部的三阶段反序列化、小端安全读写、双向元数据校验及 IEEE 802.3 CRC32 运算。
  - **阶段 B（核心升级状态机与适配）**：已完成核心机与 SOES 适配桩验证。包含驱动生命周期互斥、水位按需擦除、双重 CRC 防线、防回滚与自愈机制。
  - **全量单元测试与内存安全**：在 Apple clang 与 GNU gcc-16（含 `-fsanitize=address,undefined`）下全量自动化测试通过（共 455 项断言无一失败）。

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
- **适配层现状**：已在 `src/adapters/ecat_foe_soes.c` 与 `include/ecat_foe_soes.h` 实现初步的接口转换打样与单测桩（测试短包识别与状态标志）。**真实 SOES 源码尚未集成编译，真实联调尚未开展【未核实/待开发验证】**。
- **原生协议栈限制与技术约束**：
  - *文件名匹配机制*：原生 SOES（`esc_foe.c:72`）采用 `strncmp(foe_file_name, foe_cfg->files[i].name, num_chars)`。其匹配判定**由接收到的文件名长度 `num_chars` 决定前缀匹配，而非精确匹配**。原生 SOES 无外部 WRQ 动态路由钩子，携带动态版本号的文件名若超出静态表项长度将导致匹配失败。固件协议架构是否变更为固定目标名（如 `SERVO_DRV_MCU.efw`）或引入轻量钩子尚待最终决策，当前核心库暂不变更现有协议架构；
  - *0 长度结束包与 AL 错误*：原生 SOES 在 0 长度结束包分支（`esc_foe.c:459-465`）中忽略了 `FOE_fclose()` 的返回值。虽然适配方案设想通过 `post_state_change_hook` 拦截退出 BOOT 状态，但**当前适配器并没有调用 `ESC_ALerror` 设置 AL 状态错误码，且真实 SOES 接入未完成，绝不能声称已经拦截或保证防止错误固件启动**。当前系统依赖在会话初期执行 `invalidate()` 确保物理介质固件被标记为无效，作为防砖底线；
  - *关闭 RRQ 的真实编译方式*：原生 SOES 使用 `#ifdef FOE_READ_SUPPORTED` 控制代码编译。若在编译配置中定义了 `-DFOE_READ_SUPPORTED=0`，由于宏已被定义，`#ifdef` 仍会判定为真并试图编译！**只有完全未定义（即 `#undef FOE_READ_SUPPORTED` 或不传该宏定义）时**，RRQ 才会落入 `default:` 分支直接返回 `0x8000 (FOE_ERR_NOTDEFINED)`。

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
