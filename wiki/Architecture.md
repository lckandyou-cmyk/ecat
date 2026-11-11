# 架构概览

## 组件边界

ECAT 将升级流程拆成四层：

| 层 | 职责 | 仓库入口 |
| --- | --- | --- |
| EFW 工具 | 将现有映像封装为包，并检查格式、元数据和 CRC | [`ecat-efw`](../ecat-efw)、[`tools/`](../tools/) |
| FoE 核心 | 验证请求与 EFW 流、管理会话和校验、调用目标驱动 | [`include/ecat_foe_core.h`](../include/ecat_foe_core.h)、[`src/`](../src/) |
| 协议栈适配 | 将具体从站栈的 FoE 回调、邮箱和状态机事件映射到核心 | [`include/ecat_foe_soes.h`](../include/ecat_foe_soes.h)、[`include/ecat_foe_ssc.h`](../include/ecat_foe_ssc.h) |
| 目标存储驱动 | 通过设备本地分区配置擦写、回读、失效标记和提交固件 | [`include/ecat_foe_target.h`](../include/ecat_foe_target.h) |

项目不包含 EtherCAT 协议栈、MCU 厂商 Flash 驱动或 Bootloader 启动交接逻辑。集成工程负责串行化协议栈与核心调用，并提供设备本地身份、分区、升级结果和失败恢复策略。

## 固件数据流

```text
应用映像 .bin
  → ecat-efw pack 生成 .efw
  → EtherCAT 主站通过 FoE 发送
  → 从站协议栈适配器处理 WRQ / DATA / ERR
  → 核心解析头部、校验项目/硬件/目标及载荷
  → 本地存储驱动失效标记、擦除和写入
  → 核心比较传输 CRC 与物理回读 CRC
  → 可选验签及本地策略检查
  → 驱动提交镜像有效标记
```

这是一种责任划分示意。EFW 中的项目、硬件和目标信息用于兼容性检查，不能决定物理地址。写入基址与上限完全来自设备本地驱动配置。

## 会话与提交

同步驱动路径的关键顺序是先初始化驱动并使旧有效标记失效，再按需擦除和写入；数据完整后从目标存储回读校验，成功才提交有效标记。中途失败应由驱动清理并保持映像无效。驱动契约、失败回调语义和防回滚模式见 [`include/ecat_foe_target.h`](../include/ecat_foe_target.h)。

异步路径使用固定缓冲区和调用方提供的 start/poll 操作，在应用主循环推进耗时操作；协议栈可在操作未完成时对同一数据包返回 BUSY，收到匹配重试后再确认。异步接口不替应用决定 Flash 控制器、超时和调度策略。具体接入示例见 [`examples/ssc_async/README.md`](../examples/ssc_async/README.md)。

## 安全与验证边界

- CRC32 用于发现意外损坏，不认证发布者。
- Ed25519ph 验签默认关闭；启用时由设备本地配置受信公钥和验签后端。验签不能替代启动映像检查、防回滚或掉电恢复。
- 单分区设备可能在校验完成前已擦除旧映像；“失败时保持无效”不等于“断电后仍有可启动旧固件”。
- 宿主模拟可验证逻辑契约，不能证明真实 Flash、ESC、协议栈或主站行为。当前范围见[验证状态](Testing.md)。

更多设计背景和限制见 [`docs/architecture/SAD-v0.1.md`](../docs/architecture/SAD-v0.1.md)、[`docs/architecture/soes-integration-options.md`](../docs/architecture/soes-integration-options.md) 及 [EFW v2 格式规范](../docs/spec/efw-format-v0.2.md)。
