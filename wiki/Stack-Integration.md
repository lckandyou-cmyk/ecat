# 协议栈接入

协议栈适配层负责把 FoE 生命周期映射到核心 API：验证 WRQ 和本地目标，按序转发 DATA，处理传输中止，并在升级结束后反映最终提交结果。适配应遵从该协议栈的调用约定，不绕过邮箱长度、密码或状态机检查。协议栈调用与核心/驱动操作需要串行化。

## SOES

仓库提供可编译的 SOES Bootloader 接入参考，入口见 [`examples/bootloader_soes/README.md`](../examples/bootloader_soes/README.md)。示例通过邮件箱前置检查和后置处理钩子配合 SOES 原生 FoE 流程：前置检查请求上下文，原生栈继续执行密码和状态校验，后置处理再激活核心会话或修正响应。按示例的调用顺序和邮箱槽位约定接入，不应直接复制它的板级地址或配置值。

已知限制：

- SOES 的传输结束行为与终止包长度有关；在已分析版本的零长度结束包路径上，关闭回调错误可能被忽略。因此，主站最后一个 ACK 不能作为镜像提交成功的唯一证据。集成需提供独立可查询的升级结果。
- 如果不需要 FoE 读取，SOES 配置中应保持 `FOE_READ_SUPPORTED` 未定义；相关文件名匹配边界和响应处理细节见[接入分析](../docs/architecture/soes-integration-options.md)。
- 宿主集成使用固定版本 SOES 源文件及硬件端口桩，不等于真实 EtherCAT 主站和板卡联调。

## Beckhoff SSC 同步接口

SSC 同步适配器封装在 [`include/ecat_foe_ssc.h`](../include/ecat_foe_ssc.h)。将工程中的 FoE 应用回调映射至适配器接口，并按实际 SSC 头文件核对类型、函数签名、邮箱缓冲区和状态切换通知。SSC 第三方源码未包含在此仓库；同步 Mock 契约测试不能证明真实 SSC 的回调粒度、结束通知或状态机行为。

## SSC 异步接口

目标 Flash 操作无法在协议回调中同步完成时，可评估 SSC 异步门控和异步核心路径。适配器需要转发 `pAPPL_FoeWriteData` 的数据、长度及 `bDataFollowing`，从 `pAPPL_MainLoop()` 定期推进 pending 操作，并在 SSC 重试相同数据块时才返回完成状态。待处理数据包必须保存在调用方提供的固定缓冲区中；相关回调、轮询和中止必须串行执行。

接入示例及宿主验证命令见 [`examples/ssc_async/README.md`](../examples/ssc_async/README.md)。真实 SSC 头文件、实际 Flash 操作、板卡和主站的组合路径仍需在产品工程验证。异步接口的阶段设计背景见 [`docs/architecture/ssc-async-stage1.md`](../docs/architecture/ssc-async-stage1.md)；该设计记录描述演进阶段，当前实现能力以头文件、示例和测试为准。

## 结果报告

FoE 协议响应、核心提交结果和设备启动状态是不同层面的信息。集成端应为主站或维护工具提供可读取的持久化升级结果，例如成功、失败阶段及诊断码；尤其不能只依赖可能早于物理提交状态的最终 ACK。返回哪些状态及如何清除记录由产品协议设计决定。
