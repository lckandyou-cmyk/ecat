# 可注册固件目标设计

## 目标与边界

`ecat_target_registry_t` 是数据和固件目标的唯一权威配置；目标 ID 使用非零 `uint16_t`（`1..65535`），允许稀疏编号，数量受注册表编译期容量 `ECAT_TARGET_REGISTRY_MAX_BLOCKS` 和设备静态内存限制。注册表中的每个固件目标（`ECAT_TARGET_TYPE_FIRMWARE`）直接承载完整本地契约：分区容量 `max_size`、写入对齐粒度 `write_unit`、同步固件驱动 `driver.fw_ops` / `driver.user_ctx`、异步固件会话驱动 `driver.fw_async_ops` / `driver.fw_async_user_ctx` 以及初始化失败安全回收标志 `async_safe_session_abort`。核心仅保存项目名、工程 ID、硬件 ID/版本、验签回调和防回滚策略，并引用同一个 `const ecat_target_registry_t *registry`，不再维护第二份按 `target_id - 1` 索引的目标表。

注册表条目在生命周期内仅支持在空闲态追加注册（槽位下标 `0 .. target_count - 1` 恒定不移位、不允许重名或重复 ID），在传输会话活跃期间严禁修改注册表。配置与访问必须在同一串行上下文中执行。

固件导入在任何擦写或驱动会话开启之前，统一验证本地 ACL、目标类型（必须为已启用可写 `FIRMWARE`，拒绝 `DATA` 目标混入固件导入通道）、包内签名保护的 `target_id` 与会话选定目标一致性、项目与硬件匹配、非零本地容量上限、版本范围、防回滚下限与签名策略。文件提供的 ID 只能选择本地已注册且已启用的固件目标，绝不能指定物理地址或覆盖本地配置；`max_payload_size == 0` 一律按零容量失败关闭（Fail-Closed）拒绝。导出使用该目标注册的只读数据驱动，不绕过权限控制。

## 标识与协议

EFW v2 的 `target_id` 字段具备完整 16 位空间：允许 `1..65535`，`0` 保留为非法。FoE 目标名称采用**唯一双射规范编码**：
- `1..4` 保持既有规范名称：`1 -> MCU`、`2 -> FPGA`、`3 -> SII`、`4 -> EEPROM`；
- `5..65535` 使用 `T` 加四位大写十六进制：`T0005`～`TFFFF`（例如 `42` 编码为 `T002A`，完整请求名如 `PROJECT_T002A_V1.2.3.4.efw`）；
- 严禁歧义别名：`T0000`～`T0004`、小写十六进制（如 `T002a`）或非四位宽度（如 `T2A`）一律按未知目标拒绝。

FoE 请求文件名总长度继续受 `ECAT_FOE_MAX_FILENAME_LEN`（31 字节 ASCII）约束；使用 `TXXXX` 目标词元时，调用方应确保 `<PROJECT>_TXXXX_V<M>.<N>.<P>.<B>.efw` 总长不超过 31 字节。打包、检查、FoE 文件名解析和 SOES 映射遵循完全一致的规则；名称中的 ID、会话请求 ID 和签名保护的 EFW 头部 ID 必须严格相等。ESC SII 既有奇数字节双字节补齐规则仅适用于 `target_id == 3`，其他目标均按常规单字节尾部补齐规则处理。

目标查找通过注册表显式比对 `target_id` 并返回对应槽位下标，严禁把 `target_id` 当作数组下标或整数位移量。固件未完成状态按注册槽位保存在定长字节位图 `target_incomplete_slots[(ECAT_TARGET_REGISTRY_MAX_BLOCKS + 7U) / 8U]` 中，彻底消除高编号目标或大 `MAX_BLOCKS` 配置下的整数左移未定义行为。SOES 适配器的目标映射改用实例内聚的显式 `{target_id, file_idx}` 映射表与独立文件名同步缓冲区，移除文件静态全局缓冲，支持多实例隔离并严格校验重复映射、容量与文件表越界。

## 生命周期与兼容性

同步和异步固件核心共用同一目标解析、权限及 EFW 阶段 3 单目标核验函数：包内 `target_id` 与会话目标不一致时明确返回 `ECAT_FOE_EFW_ERR_TARGET_MISMATCH` 并映射为 `ECAT_FOE_CORE_ERR_HDR_CROSS_MISMATCH`，与未知目标 `ECAT_FOE_EFW_ERR_TARGET_UNKNOWN` 严格区分。异步挂起、取消、失败必须停止硬件并释放唯一会话；开启防回滚时，同步与异步 `FINALIZE` 失败统一报告 `ECAT_FOE_CORE_ERR_ANTI_ROLLBACK_UPDATE`，且板级版本下限接口按实际 `uint16_t target_id` 调用。目标容量以注册表本地 `max_size` 为唯一真源，传输层不再在运行期临时改写或恢复核心配置字段。

这是 C 配置接口的有意收敛：旧四项核心目标数组与 EFW 四元目标数组全部移除并统一为注册表配置，同步升级 `ecat_target_registry_abi.h`（v2）与 `ecat_foe_config_abi.h`（v3）编译期链接符号以拦截半旧目标文件混链。旧 `1..4` 号 `.efw` 容器二进制与传统 FoE 文件名保持 100% 兼容；全库对目标表、容量与异步操作缓冲保持零动态内存分配。

## 分步交付与验证

1. 固化本设计契约，收紧 EFW 单目标核验（拆分 `TARGET_MISMATCH`、`max_payload_size == 0` 失败关闭）并补齐高编号、签名与防回滚单测。
2. 扩展 `ecat_target_registry_t` 承载完整同步/异步固件驱动属性与槽位查找接口，补齐枚举、重名与异步 `abort` 校验并升级注册表 ABI 符号。
3. 迁移同步与异步固件核心及传输管理器至单一注册表真源，采用按字节槽位位图保存未完成状态，修复异步防回滚提交错误码并升级核心 ABI 符号。
4. 打通 FoE `T0005`～`TFFFF` 规范名称解析、实例内聚的 SOES `{target_id, file_idx}` 映射以及 `ecat-efw` 打包/检查/解包工具，补齐端到端跨层测试。
5. 同步示例与中英文规范文档，在干净归档环境下跑通普通测试、ASan/UBSan、编译期哨兵、真实 Monocypher、真实 SOES、Bootloader、引文核验与发布 CLI 回归。

真实 MCU、Flash 掉电恢复行为和真实 SSC 主站时序仍需在目标硬件上实测验证；宿主测试与模拟驱动不得冒充实机验证结论。
