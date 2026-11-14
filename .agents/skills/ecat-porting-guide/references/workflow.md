# 可执行移植流程

每一关都要保存配置、命令和可观察证据。失败时停止后续安装步骤，保留现场并按本关失败定位处理。

| 关卡 | 输入与操作 | 产物与验收 | 失败定位 |
|---|---|---|---|
| 1. 基线 | MCU/ESC/PDI、协议栈和工具版本、工程文件、编译器、分区；记录 Git SHA、工作区改动并备份工程 | 环境清单、可恢复备份；确认现有工程接入或新建工程路径 | 检查工具版本、缺失文件、未提交改动及备份可读性 |
| 2. 原系统 | 连接目标板，读取 ESC 寄存器，测访问超时，运行原 App 并检查 OP/PDO | 原系统基线日志；无硬件时标记待验证 | 分开检查供电、复位、PDI 时序、ESC 地址和主站配置 |
| 3. 协议栈配置 | 按 SSC 或 SOES、现有工程或新建工程选择配置；核对 FoE/Bootstrap、邮箱 SM、访问宽度、身份、PDO 与 EEPROM/SII | Boot/App 配置和参数对照表；配置与产品需求一致 | 检查生成输入、回调宏、邮箱容量和 PDO 缓冲；CoE 按产品需求决定 |
| 4. 生成产物 | 在隔离输出目录运行对应工具；记录输入文件与工具版本，比较生成前后差异 | 源码、ESP/ESI 等产物；检查 FoE 回调签名、宏、PDO 映射、邮箱及身份 | 查看冲突日志和差异；恢复手写适配，不覆盖原工程配置 |
| 5. 库与驱动 | 纳入核心、适配器和目标驱动；按本地分区实现 init/invalidate/erase/write/read/finalize/abort | 工程文件清单、分区映射、驱动生命周期说明；边界和对齐来自具体器件 | 检查链接输入、回调缺失、偏移溢出、擦除水位与写入粒度 |
| 6. 启动交接 | 对齐 Boot/App 链接地址、向量表、升级请求标记、镜像有效性检查和跳转桩 | 链接配置及 App→Boot→FoE→退出→App 接线说明 | 检查向量内容、VTOR、MSP/Reset_Handler 和有效标记消费时机；区分 MCU 正运行 Bootloader 与 EtherCAT AL BOOT |
| 7. 构建检查 | 执行目标工程构建并检查 map 文件、段地址、容量、SRAM 运行代码和 Flash 依赖 | 构建日志、map 检查结果、警告清单 | 确认库、协议栈、驱动实际进入目标；不能只依据 Build successful |
| 8. 升级包 | 用仓库打包工具生成 EFW，检查头、payload、版本和 CRC；核对线上 FoE 请求名 | 包 hash、检查输出、FoE 文件名；固件版本与容器格式版本分别记录 | 核对工具参数、硬件/工程身份、容器格式及邮箱分包；后缀适配只针对已验证组合 |
| 9. 安装 | 备份当前镜像与设备描述后安装 Boot/App；只有确需改变 SII 时才写 ESC EEPROM | 安装记录；区分复制 ESI、写 ESC EEPROM 与 FoE 写 MCU | 确认目标状态、文件类型、EEPROM 启动数据和恢复途径 |
| 10. 实板验收 | 在指定主站与板卡测试启动、SDO、OP/PDO、进入/退出 BOOT、连续升降级、异常包、重试、断网和擦写/提交断电 | 记录主站/SSC 版本、构建 SHA、包 hash、响应、耗时、BUSY 计数和每项结果 | 按阶段日志区分协议超时、Flash 失败、校验失败和启动交接失败；未执行项保持待验证 |

## 同步与异步路径

同步路径在 SSC DATA 回调中执行核心和驱动操作，通常实现最直接；必须实测擦除、写入及回读耗时是否满足主站响应限制。

异步路径需要完整接通核心会话、单包重试门控、目标操作 runner 和主循环 poll。等待操作期间仅对内容、长度及结束标志相同的数据包返回 BUSY；不得对所有普通包无条件 BUSY。重复包不得重复写入。末包确认必须等待全部写入、读回 CRC 与 finalize 完成。队列式接收与单包停等是不同设计，应分别说明缓冲容量和满载行为。

## SSC 工具自动化

ET9300 自动化是可选路径。先发现已授权安装的工具和版本，再选择加载现有 ESP 或新建工程；每项配置写入后读回，生成到独立目录并检查产物。失败要明确报错。API 不可用时提供 GUI 等价步骤。公共仓库不得包含受保护的 SSC 源码或 DLL，也不得要求固定个人绝对路径。自动化应释放自己创建的 Excel 实例，不结束用户已有进程，使用任务独立临时目录和结构化参数，按官方模板表达对象类型与 PDO 方向。

## 示例与实测边界

仓库的宿主测试仅验证公开 API 契约。不能据此声明 Beckhoff SSC 实栈、TwinCAT 主站或 MCU Flash 已通过。针对 STM32F407/FCE1353 的专用 HBI、分区和启动适配仅对该板有效。每次交付都列出实际执行的构建、包检查与板测步骤及未验证项。

## 可执行的最小宿主示例

从仓库根目录构建现有 host demo：

```sh
make build
```

该目标编译 EFW parser 与同步核心 runner，结果放在 `build/` 并由 `tools/safe_build.py` 登记。它验证宿主编译路径，不代表协议栈或目标 Flash 已接通。SSC adapter 契约与异步 target runner 可分别运行：

```sh
make test-ssc-async CC=clang
make test-target-async CC=clang
```

完整异步核心契约测试可运行：

```sh
make test-core-async CC=clang
```

在应用中为异步 target 配置 `asynchronous=true`、`async_ops` 和 `async_user_ctx`，创建静态 `ecat_foe_core_async_t`、packet buffer 与 operation buffer；operation buffer 容量至少为 `max(64, 所有启用异步目标的 write_unit)`。gate packet buffer、核心 packet buffer 与 operation buffer 各自独立且不得重叠；两个 packet buffer 均须容纳 SSC DATA 的最大载荷。将 `ecat_foe_core_async_ssc_ops()` 传给 SSC gate，并在 `pAPPL_FoeWriteData(..., bDataFollowing)` 中转发该标志。每次 `pAPPL_MainLoop()` 调用 `ecat_foe_ssc_on_main_loop()`。同步目标仍使用既有 `driver` 和同步核心 API。

该宿主测试覆盖分包头、BUSY/同包重试、异步擦除/写入/回读与 finalize。接入真实项目时还必须核实实际 SSC BUSY 值、回调串行关系和主站重试行为；宿主测试不能替代 Beckhoff SSC、TwinCAT 或 MCU 实板验收。

## SSC 现有工程接线

输入是可恢复的原 Boot/App 工程、实际 SSC 生成文件及本地分区表。将 `src/ecat_foe_efw.c`、`src/ecat_foe_core.c`、`src/ecat_foe_target_async.c`、`src/ecat_foe_core_async.c`、`src/adapters/ecat_foe_ssc_async.c`、`src/adapters/ecat_foe_ssc.c` 和目标驱动加入 Boot 构建，并把本库 `include/` 加入 include path；确认工程中只存在一个 main。同步路径使用 `ecat_foe_efw.c`、`ecat_foe_core.c` 和 `ecat_foe_ssc.c`，同时链接 SSC adapter 所需的 `ecat_foe_ssc_async.c`。新旧 SSC 适配器均还需链接 `src/ecat_upgrade.c`、`src/ecat_upgrade_session.c` 和 `src/adapters/ecat_foe_transport.c`。共享入口按 [统一架构](../../../../docs/architecture/transport-independent-upgrade.md) 完成 bind_upgrade，不能混用未绑定旧 API 操作同一核心。工程文件或 IDE group 的具体位置由现有构建系统决定。

目标 descriptor 的 project ID、project name、hardware ID、硬件版本、目标 ID、容量及写入单位来自板级配置；异步 target 的 `async_ops` 实现 `start/poll/abort/abort_session`。操作可即时完成或 pending；仅在完成时输出擦除水位或读回数据，启用防回滚时 FINALIZE 必须原子提交镜像与版本下限。依次调用 `ecat_foe_core_init`、`ecat_foe_core_async_init`、`ecat_foe_ssc_async_init`（传 `ecat_foe_core_async_ssc_ops()` 与会话指针），再调用 `ecat_foe_ssc_init_async`。检查每个初始化返回值；BUSY 和错误值从实际 SSC 头文件核对，tick 必须单调且与 timeout 单位一致。

在 SSC 生成的实际声明处注册 WRQ、DATA、错误回调：WRQ 转发至 `ecat_foe_ssc_on_write`，DATA 转发至 `ecat_foe_ssc_on_data_following`，错误/中止转发至 `ecat_foe_ssc_on_abort`；`pAPPL_MainLoop` 调用 `ecat_foe_ssc_on_main_loop`。函数参数类型、内存限定符、字节长度及运行上下文以目标 SSC 头文件核对；回调与 poll 须串行。不要在异步模式调用同步 `ecat_foe_ssc_on_data`。可运行的初始化与模拟驱动参考 `tests/test_core_async.c`，它不包含可烧录的 SSC 或板级 Flash 驱动。

构建后在 map 中核对上述对象文件已链接、Boot/App 地址及容量符合本地分区、需要在 SRAM 执行的代码和向量满足芯片约束。检查失败先核对工程 group、include path、链接脚本和实际 SSC 回调声明；不得仅凭宿主测试声称实板通过。

## 生成和检查 EFW

将示例值替换为本地配置，输入是已从正确 App 链接地址生成的 `app.bin`；输出路径须不存在：

```sh
python3 tools/efw_pack.py pack --bin app.bin --project-id 0x10010001 --project-name SERVO_DRV --hardware-id 0x00000001 --hw-ver-min 1 --hw-ver-max 1 --target MCU --fw-version 1.0.0.0 --out SERVO_DRV_MCU_V1.0.0.0.efw
python3 tools/efw_pack.py inspect SERVO_DRV_MCU_V1.0.0.0.efw --extract-bin verified-app.bin
cmp app.bin verified-app.bin
shasum -a 256 SERVO_DRV_MCU_V1.0.0.0.efw
```

检查 inspect 输出中的格式 2、头长 148、目标、版本、payload 长度与 CRC；设备 SDO 版本、WRQ 文件名和包版本须匹配。上述示例未启用签名或 `--foe-packet-size` 填充；启用签名时按设备信任配置使用公钥检查，启用填充时提取内容可能包含填充，按工具输出及目标版本规则核对。线上 FoE 请求名与操作系统文件路径分别确认。

## ET9300 GUI 后备操作

菜单名称因 ET9300 版本而异；以本机工具帮助和实际界面为准。先记录已安装工具版本、授权和官方模板路径，备份现有 ESP、Excel 对象字典及手写文件；复制工程到独立输出目录。

1. 启动已授权的 SSC Tool。现有工程从备份副本打开 ESP 并保留原设置；新建工程选择与目标 ESC/PDI 匹配的官方模板。导入既有对象字典或使用正式模板，不凭固定 Excel 行号生成对象。
2. 在设备、硬件、邮箱和应用配置页面分别检查 CPU/ESC 访问宽度、PDI、轮询/DC/中断、常规与 Bootstrap 邮箱地址和大小、身份以及 Boot/App 的 FoE、Bootstrap、CoE/PDO 和默认 main 选项。实际字段名称、可选值以该版本界面与生成宏为准。
3. 运行工具的配置检查或冲突检查，保存 ESP 后在隔离目录生成源码及设备描述。核对实际生成的宏、FoE 源文件、回调声明、PDO 长度、邮箱配置和 ESI 身份；记录冲突日志。将产物与原工程 diff，手动迁移经确认的映射、ESM 和硬件适配，禁止盲目覆盖。
4. 将审查过的产物加入工程，执行上述构建、map 与 EFW 检查。ESI 文件复制、ESC EEPROM/SII 写入及 MCU FoE 下载是独立操作；仅在确需更新 SII 且已备份恢复数据时执行 EEPROM 写入。

若 GUI 或生成器不能加载旧工程、读回配置或生成一致产物，保留输入与日志并停止替换原工程；先核对工具版本、模板、授权和字段，再选择可验证的操作。尚未在目标 ET9300/工程组合执行的步骤标为待验证。
