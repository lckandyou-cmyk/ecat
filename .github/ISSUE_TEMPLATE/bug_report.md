---
name: 问题报告 (Bug Report)
about: 提交可复现的问题或异常现象
title: "[BUG] "
labels: ["bug"]
assignees: []
---

## 1. 现象描述 (Description)
请简要描述您遇到的异常现象、触发时机以及它对升级流程的影响：

## 2. 最小可复现步骤 (Reproduction Steps)
1. 第一步
2. 第二步
3. 第三步

## 3. 预期行为与实际结果 (Expected vs Actual)
- **预期行为**：
- **实际结果**：

## 4. 运行环境与版本信息 (Environment)
- **ECAT 库版本 / Commit**：
- **从站协议栈与版本**：
  - 若为 SOES：请注明源码来源及关键文件 SHA-256 哈希（或 commit），是否与文档基线一致
  - 若为 SSC：请注明 SSC 版本与配置
- **从站硬件配置**：
  - MCU 芯片型号（如 STM32F407、GD32F450 等）：
  - ESC 从站控制器芯片（如 ET1100、LAN9252、AX58100 等）：
  - Flash 介质类型（片内 Flash / 外部 SPI NOR Flash）：
- **主站环境**：
  - 主站软件及版本（如 TwinCAT 3 Build xxxx、SOEM 现场主站等）：
  - 操作系统：
- **工具链**：
  - 编译器及版本（如 arm-none-eabi-gcc 12.3、armclang 6.21、Host Clang/GCC）：

## 5. 日志与调试信息 (Logs & Traces)
> [!WARNING]
> **安全与隐私提醒**：
> 请务必对上传的所有日志、Wireshark/TwinCAT 抓包、终端输出进行**严格脱敏**！
> **切勿包含**任何真实的 FoE 传输密码、设备私钥、通信证书、签名凭据或专有商业固件二进制。

```text
在此粘贴脱敏后的串口调试日志或抓包说明
```

## 6. 其他上下文 (Additional Context)
（如有其他有助于定位问题的补充信息，请在此处说明）
