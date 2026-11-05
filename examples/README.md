# EtherCAT FoE 核心状态机参考示例与驱动模板 (Examples & Templates)

本目录包含 EtherCAT FoE 固件升级核心状态机的宿主机参考示例（Host Demo）与底层驱动实现模板（Driver Template）。

## 1. 目录结构

```text
examples/
├── README.md                      # 本说明文档
├── FINDINGS.md                    # 示例开发技术发现与 API 完备性评估
├── host_demo/                     # 宿主机参考运行示例
│   ├── main.c                     # 8 大场景测试入口与文件 Flash 模拟驱动
│   └── expected_output.txt        # 宿主示例基准参考输出 (用于自动化对比回归)
└── driver_template/               # 底层物理介质驱动模板
    ├── ecat_foe_driver_template.h # 七大回调契约头文件
    └── ecat_foe_driver_template.c # 驱动骨架实现
```

## 2. 宿主机参考示例 (host_demo)

### 2.1 核心特性与设计规范
- **临时隔离环境与同名哨兵保护**：所有模拟 Flash 介质文件（数据与元数据）均在系统独立临时目录（如 `/tmp/foe_host_demo_XXXXXX`）中生成，测试结束自动清理自身目录；在当前工作目录预存同名哨兵文件，验证在全流程测试中绝不误删、污染或篡改用户工作区现有文件。
- **非破坏性 init 与失效先行契约**：驱动 `init()` 仅建立外设句柄，不以截断模式破坏现有数据；在执行任何擦除前必须由 `invalidate()` 将元数据标记置为无效；示例包含对上一版本旧数据和有效标记在校验/失效前的可观察断言。
- **真实 Python 打包器端到端链路**：场景 7 直接加载由 `tools/efw_pack.py` 打包的真实 `.efw` 固件（包含 `--foe-packet-size` 规避填充），验证打包器与核心机的数据对齐。
- **用户自定义板级驱动与多目标配置**：展示 Target 1（主 MCU 板载 Flash）与 Target 2（外部 SPI Flash，展示可选目标启用/禁用开关），突出底层存储驱动与硬件介质的解耦。
- **强 C 语言断言**：所有场景均包含明确的 `assert()` 断言，检查驱动状态、返回值、错误码与介质内容，避免仅依靠文本比对。

### 2.2 覆盖的 8 大场景
1. **Normal Success**：标准固件流完整接收、旧数据校验前可读断言、失效标记先行、分块写入、回读 CRC32 比对、finalize 提交；
2. **Header CRC Mismatch**：文件头 CRC32 故意篡改，核心在头部装配阶段识别并熔断；
3. **Project ID and Name Mismatch Suite**：细分测试 3a（WRQ 阶段拦截项目名称不匹配）与 3b（WRQ 通过但头部 `project_id` 不符，在阶段 3 全量元数据核验拦截，驱动未被擦写）；
4. **Stream Under-Receive**：数据流提前截断，`on_end` 校验接收总长不足并触发 abort；
5. **Stream Over-Receive**：数据流超出预期长度，核心在 `on_data` 触发即时防溢出熔断；
6. **Abort and Retry on Same Core**：中途断流 abort 后恢复为 IDLE 态，在同一个 core 实例上接收新 WRQ 重新升级成功；
7. **Padded Firmware End-to-End via efw_pack.py**：加载真实打包的补零固件，验证以短包收尾的端到端流程；
8. **Core Defense on Driver Finalize Failure**：演示核心机在驱动提交失败时主动触发 abort 并保持介质无效（注：真实 SOES 协议栈在零长度结束包忽略返回值的行为由 `tests/test_soes_real_harness.c` 独立验证）。

### 2.3 运行与验证命令
编译并执行示例：
```bash
make test-examples
```
或者手动编译运行：
```bash
clang -Wall -Wextra -Werror -std=c99 -pedantic -Iinclude -o examples/host_demo/host_demo examples/host_demo/main.c src/ecat_foe_efw.c src/ecat_foe_core.c
./examples/host_demo/host_demo
```
宿主示例输出已收敛为确定性文本，不包含时间戳或本地环境路径。自动化测试会将其实际输出与 `examples/host_demo/expected_output.txt` 进行逐行比较。

## 3. 驱动模板 (driver_template)

`driver_template` 演示了用户自定义物理介质存储驱动对接核心机所需的 7 个函数指针回调实现骨架与生命周期契约：
- `init`：初始化介质；非破坏性操作，失败时自行清理，核心不会调用 `abort`；
- `invalidate`：擦除前同步使物理有效标志失效；
- `erase_to`：按需扇区擦除（未失效前拒绝擦除）；
- `write`：物理分块写入；
- `read`：物理回读；
- `finalize`：校验通过后最后写入有效标记；
- `abort`：故障或中止时的幂等清理。

驱动模板已集成至 `make test-examples` 编译检查中，使用 `-Wall -Wextra -Werror -pedantic -std=c99` 编译确保合规。

## 4. 重要声明与已知限制

- **非掉电安全介质**：宿主示例使用普通宿主文件模拟 Flash，未实现断电保护的硬件原子性操作或乒乓分区；
- **非真实 MCU / 硬件环境**：本示例仅在宿主机操作系统（macOS / Linux）上作为逻辑参考运行，未经真实 MCU 架构（如 Cortex-M / RISC-V）或真实 ESC 从站控制器硬件验证；
- **未包含真实 SOES 生产端到端接入**：真实 SOES 生产运行依赖硬件 PDI 接口、从站邮箱中断及实时调度循环，本示例展示的是独立的核心状态机与驱动交互契约。
