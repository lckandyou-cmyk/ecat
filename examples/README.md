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

### 2.1 覆盖场景
`host_demo` 仅调用核心公开 API（`include/ecat_foe_core.h`）与打包函数，验证以下 8 种场景：
1. **Normal Success**：标准固件流完整接收、头部校验通过、分块写入、回读 CRC32 比对、finalize 提交；
2. **Header CRC Mismatch**：文件头 CRC32 故意篡改，核心在头部装配阶段识别并熔断；
3. **Project ID Mismatch**：项目 ID 与本地配置不符，核心拒绝升级；
4. **Stream Under-Receive**：数据流提前截断，`on_end` 校验接收总长不足并触发 abort；
5. **Stream Over-Receive**：数据流超出预期长度，核心在 `on_data` 触发即时防溢出熔断；
6. **Abort and Retry on Same Core**：中途断流 abort 后，在同一个 core 实例上接收新 WRQ 重新升级成功；
7. **Padded Firmware**：采用尾部补零固件避开空包路径，以短包正常收尾；
8. **Zero-Length Limitation Demo**：演示原生 SOES 零长度包结束分支忽略 `finalize` 失败的协议栈局限性。

### 2.2 运行与验证命令
编译并执行示例：
```bash
make test-examples
```
或者手动编译运行：
```bash
clang -Wall -Wextra -Werror -std=c99 -pedantic -Iinclude -o examples/host_demo/host_demo examples/host_demo/main.c src/ecat_foe_efw.c src/ecat_foe_core.c
./examples/host_demo/host_demo
```
宿主示例输出已收敛为确定性文本，不包含时间戳或文件路径。自动化测试会将其实际输出与 `examples/host_demo/expected_output.txt` 进行逐行比较。

## 3. 驱动模板 (driver_template)

`driver_template` 演示了物理 Flash 驱动对接核心机所需的 7 个函数指针回调实现骨架与生命周期契约：
- `init`：初始化介质；失败时自行清理，核心不会调用 `abort`；
- `invalidate`：擦除前同步使物理有效标志失效；
- `erase_to`：按需扇区擦除；
- `write`：物理写入；
- `read`：物理回读；
- `finalize`：校验通过后最后写入有效标记；
- `abort`：故障或中止时的幂等清理。

驱动模板已集成至 `make test-examples` 编译检查中，使用 `-Wall -Wextra -Werror -pedantic -std=c99` 编译确保合规。

## 4. 重要声明与已知限制

- **非掉电安全介质**：宿主示例使用普通宿主文件模拟 Flash，未实现断电保护的原子操作或乒乓分区；
- **非真实 MCU / 硬件环境**：本示例仅在宿主机操作系统（macOS / Linux）上作为逻辑参考运行，未经真实 MCU 架构（如 Cortex-M / RISC-V）或真实 ESC 从站控制器硬件验证；
- **未包含真实 SOES 生产端到端接入**：真实 SOES 运行依赖硬件 PDI 接口、从站邮箱中断及实时调度循环，本示例展示的是独立的核心状态机与驱动交互契约。
