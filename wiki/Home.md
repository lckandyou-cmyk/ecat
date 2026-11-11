# ECAT Wiki

ECAT 是用于 EtherCAT FoE 固件升级的 C99 组件。本 Wiki 汇总快速上手、系统架构、移植、协议栈接入、固件包工具和故障排查资料。

> 项目仍处于研发阶段。宿主测试不能替代目标 MCU、真实协议栈和主站上的联调验证。本文会标明结论的验证范围。

## 导航

- [快速开始](Getting-Started.md)
- [架构概览](Architecture.md)
- [MCU 移植指南](Porting-Guide.md)
- [协议栈接入](Stack-Integration.md)
- [EFW 打包与签名工具](EFW-Tooling.md)
- [测试与验证](Testing.md)
- [故障排查](Troubleshooting.md)
- [文档约定](Documentation-Conventions.md)

## 仓库中的规范资料

- [项目 README](../README.md)
- [EFW v2 格式规范](../docs/spec/efw-format-v0.2.md)
- [系统架构说明](../docs/architecture/SAD-v0.1.md)
- [SOES 接入分析](../docs/architecture/soes-integration-options.md)

此 Wiki 是面向集成者的操作入口；格式规范和架构设计文档仍以仓库 `docs/` 中的原始文件为准。
