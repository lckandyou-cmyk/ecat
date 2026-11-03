# EtherCAT INIT→BOOT 状态切换与独立 Bootloader 衔接技术调研报告

## 摘要结论

经实际调取并逐行核对本地已下载的官方与开源原文件（Beckhoff ET9300 应用笔记 v1.10、SOES 源码、Microchip LAN9252 数据手册、Infineon XMC4800 范例工程文档与源码、Microchip MPLAB Harmony 应用文档、PSI 范例工程文档），关于 **APP 收到 INIT→BOOT 请求后复位进独立 Bootloader 衔接官方流程** 的调研结论如下：
1. **官方规范未限制实现形式**：Beckhoff 官方 ET9300 第 129 页明确规定固件写入与复位由从站厂商自行实现（"Which mechanism is implement is up to the slave vendor."），并未强制限制由 APP 还是 Bootloader 接收 FoE；
2. **ESC 引脚硬复位特性与板级待确认项**：LAN9252 官方手册第 39 页证实外部拉低 RST# 会导致整片芯片级硬复位（"Driving the RST# input pin low initiates a chip-level reset."）并重载 EEPROM。但 MCU 软复位是否会连带拉低该引脚，完全取决于板级原理图设计（**待硬件确认**）；
3. **SOES 源码状态机分支证实**：SOES `esc.c` 第 1155 行证实其状态机显式处理 `INIT_TO_BOOT` 并启动 Bootstrap 邮箱，第 1197 行证实非 INIT 状态切入 BOOT 直接报错；
4. **业界范例均由 APP 跑 FoE**：英飞凌（XMC4800）与 Microchip（ethercat_counter_foe_app）官方文档原文证实，二者均由 APP 在 BOOT 态下接收 FoE 固件写入副分区/副 Bank，再切回 INIT 重启，若 APP 损坏则无法恢复（推断，原文无此句），反向印证了本库“独立 Bootloader 跑 FoE”在可靠性架构上的必要性与独立性。

---

## 一、已核实清单（均已调取本地原文件逐行核对，附 15 字内原文与文件行号/页码）

### 1. Beckhoff ET9300 应用笔记（an_et9300_v1i10.pdf）
* **出处**：本地文本 `an_et9300.txt`（提取自官方 `an_et9300_v1i10.pdf`）
* **位置与原文关键句**：
  * 第 15 章，第 129 页，第 9725 行：
    `BOOTSTRAPMODE_SUPPORTED`
  * 第 15 章，第 129 页，第 9728 行：
    `Which mechanism is implement is up to the slave vendor.`
* **核实结论**：官方 SSC 仅定义宏开关，明确固件写入与复位机制由从站厂商自行决定，未提供具体跳转实现。

### 2. SOES 协议栈源码（OpenEtherCATsociety/SOES）
* **出处**：本地文件 `/tmp/soes_src/esc.c`、`esc_foe.h`、`esc_foe.c`、`LICENSE`
* **位置与原文关键句**：
  * `soes/esc.c` 第 1155 行：
    `case INIT_TO_BOOT:`
  * `soes/esc.c` 第 1160 行：
    `an = ESC_startmbxboot (ac);`
  * `soes/esc.c` 第 1197 行：
    `case PREOP_TO_BOOT:`
  * `soes/esc.c` 第 1202 行：
    `ESC_ALerror (ALERR_INVALIDSTATECHANGE);`
  * `soes/esc.c` 第 388 行：
    `ESC_ALerror (ALERR_INVALIDBOOTMBXCONFIG);`
  * `soes/esc_foe.h` 第 48 行：
    `uint8_t write_only_in_boot;`
  * `soes/esc_foe.c` 第 83 行：
    `return FOE_ERR_NOTINBOOTSTRAP;`
  * `LICENSE` 第 14 行：
    `As a special exception`
* **核实结论**：SOES 仅允许从 `INIT` 进入 `BOOT`，其余状态切入 BOOT 直接报错；支持配置仅在 BOOT 态下允许 FoE 写固件；开源许可证含 Linking Exception，允许静态链接不传染。

### 3. Microchip LAN9252 官方数据手册（DS00001909A）
* **出处**：本地文本 `lan9252.txt`（提取自官方 `00001909A.pdf`）
* **位置与原文关键句**：
  * 第 6.2.1.2 节，第 39 页，第 4874 行：
    `Driving the RST# input pin low initiates a chip-level reset.`
  * 第 6.2.1.2 节，第 39 页，第 4875 行：
    `This event resets all circuitry within the device.`
  * 表 6-1，第 38 页，第 4758 行：
    `EEPROM Loader Run` 对应 RST# Pin 列为 `YES`
* **核实结论**：LAN9252 的 RST# 引脚拉低会触发整片芯片级复位，内部电路、PHY 全部重置并强制重新加载 EEPROM。

### 4. Infineon 官方 XMC4800 固件升级范例
* **出处**：本地文件 `/tmp/ref_repos/infineon_readme.md` 与 `/tmp/ref_repos/infineon_license`
* **位置与原文关键句**：
  * `README.md` 第 405 行：
    `While the firmware is downloaded to the backup partition`
  * `README.md` 第 410~412 行：
    `After the upload is complete, go back to the slave device's 'Online' window, and set the device back to 'INIT' state. This triggers the restart of the device with bootloader executable. Firmware inside the application partition will be updated.`
  * `README.md` 第 307 行：
    `Then check the function APPL_StopMailboxHandler, it is called when slave is requested INIT state. The function "void FWUPDATE_StateTransitionInit(void)" of the example implementation is called to indicate the slave state changes from BOOT to INIT`
  * `LICENSE` 第 1 行：
    `INFINEON END USER LICENSE AGREEMENT`
* **核实结论与机制对比**：
  * **执行主体与时序**：英飞凌范例中，FoE 固件接收是在 **APP（FoE 工程）** 中进行的，新固件先写入备份分区；下载完成后，主站下发切回 **INIT** 状态命令，由 `APPL_StopMailboxHandler` 中的 `FWUPDATE_StateTransitionInit()` 触发 MCU 重启进入 Bootloader，Bootloader 仅负责将固件从备份分区搬到运行分区；
  * **对本库的参考价值**：证实了从站利用 EtherCAT 状态机迁移回调（如进入 INIT 态时的 `APPL_StopMailboxHandler`）作为安全停止总线通信、触发 MCU 复位跳转的工程可行性；
  * **与本库的核心区别**：
    1. 英飞凌是由 APP 接收 FoE，本库是由独立 Bootloader 接收 FoE；
    2. 英飞凌进 Bootloader 的触发时机是文件传完后的 `BOOT → INIT` 阶段，本库进 Bootloader 的触发时机是升级开始前的 `INIT → BOOT` 阶段；
    3. 英飞凌方案若 APP 损坏则无法恢复（推断，原文无此句），本库方案在 APP 损坏时依然可通过 Bootloader 进行 EtherCAT 现场升级。

### 5. Microchip 官方 MPLAB Harmony 范例
* **出处**：本地文件 `/tmp/microchip_lan9252_samd51.md` 与 `/tmp/ref_repos/microchip_license.md`
* **位置与原文关键句**：
  * `readme_lan9252_samd51.md` 第 15 行：
    `The firmware update is performed by FoE (File over EtherCAT) protocol. It is triggered by the EtherCAT Manager (TwinCAT Manager) which then download the firmware onto the EtherCAT Client (LAN9252)`
  * `readme_lan9252_samd51.md` 第 134 行：
    `The Dual Bank feature enables the FoE firmware to be executed from one bank while an updated version of the firmware is programmed into the other bank. The APP_BankSwitch() application function is called to swap the banks and to reset the device when the programming is complete.`
  * `mplab_harmony_license.md` 第 6 行：
    `MICROCHIP SOFTWARE IS PROVIDED SOLELY TO ASSIST YOU IN DEVELOPING PRODUCTS AND SYSTEMS THAT USE MICROCHIP PRODUCTS.`
* **核实结论**：该范例（ethercat_counter_foe_app）由 APP 接收 FoE：明确由应用固件在双 Bank 机制下运行并接收 FoE 固件写入非活动 Bank，完成后由 `APP_BankSwitch()` 切换 Bank，该范例不包含独立 Bootloader 跑协议栈的设计；许可证限制仅能用于 Microchip 产品。

### 6. Paul Scherrer Institute 范例
* **出处**：本地文件 `/tmp/ref_repos/psi_readme.md`
* **位置与原文关键句**：
  * `README.md` 第 46 行：
    `Hardware uses low-cost components (Artix-7, Lan9254)`
  * `README.md` 第 141~145 行：
    `addressed by the (case-insensitive) first letter of the filename which is written by FoE:`
    `'G' 0x000000 0x220000 Golden/Fallback Image`
* **核实结论**：纯 FPGA（Artix-7）VHDL 设计，依赖文件名首字母与 FPGA 硬件 Golden Image 机制，非 MCU 纯 C 架构。

---

## 二、未核实清单（说明原因与所需支持）

1. **ETG 官方规范原文（ETG.1000.6、ETG.2000、ETG.2010）**：
   * **未核实原因**：尝试直接从 `https://www.ethercat.org/download/documents/` 下载 `ETG1000_6_*.pdf`、`ETG2000_*.pdf`、`ETG2010_*.pdf`，服务器（CloudFront）均返回 **HTTP 403 Forbidden**（需 ETG 会员登录鉴权），无法在本地打开阅读；
   * **需提供文件**：由 ETG 官网登录下载的 ETG.1000.6、ETG.2000、ETG.2010 官方规范 PDF。
2. **Beckhoff 官方 SSC 内部实现（ecatslv.c、bootmode.c）**：
   * **未核实原因**：ET9300 SSC 工具源码需 ETG 账号在官方工具生成，公开网络无直接可读的官方原版文件；
   * **需提供文件**：由官方 SSC Tool 生成的包含 `ecatslv.c`、`bootmode.c` 的源码目录。
3. **Beckhoff ET1100 硬件数据手册 Section III**：
   * **未核实原因**：本地未下载 ET1100 Section III 原版 PDF；
   * **需提供文件**：`ET1100 Hardware Data Sheet Section III` PDF 文件。
4. **Paul Scherrer Institute 仓库独立许可证文件**：
   * **未核实原因**：向 GitHub 仓库请求 `LICENSE` 与 `COPYING` 均返回 **HTTP 404 Not Found**，仓库未包含独立许可证文件。

---

## 三、风险与架构建议

1. **硬件连带硬复位风险（待硬件确认）**：
   * **状态**：LAN9252 数据手册证实拉低 RST# 会导致全芯片复位与 EEPROM 重载；但 MCU 执行软复位时是否会连带拉低 ESC 的 RST#，**取决于实际板级的硬件布线与原理图设计**（待硬件确认）；
   * **架构建议**：库的使用说明中应明确指出该硬件边界：建议硬件设计做单向隔离；针对硬件已直连的既有板卡，库必须提供**纯内存跳转函数**（关闭全局中断 $\rightarrow$ 重设 MSP $\rightarrow$ 直接跳转 Bootloader Reset_Handler），避免触发芯片级硬件复位。
2. **状态机迁移衔接策略**：
   * **参考结论**：英飞凌范例证实了利用状态迁移回调执行系统复位/跳转是可行路径；
   * **架构建议**：在独立 Bootloader 架构中，APP 侧在收到 `INIT → BOOT` 请求时只做两件事：记录升级魔数、跳转进 Bootloader。由 Bootloader 启动并重新配置 PDI 后，在 Bootloader 内部向上报送 `STATE_BOOT` 完成握手。

---

## 四、需要人工提供放入本地的文件清单

1. `ETG1000_6_*.pdf`（核查官方应用层状态机条款）
2. `ETG2000_*.pdf`（核查官方 PreopTimeout 默认超时数值条款）
3. `ETG2010_*.pdf`（核查官方 SII 寄存器字偏移表格）
4. `ET1100_Datasheet_Section_3_*.pdf`（核查 ET1100 复位引脚电气特性）
5. 官方 SSC Tool 生成的示例代码包（含 `ecatslv.c`, `bootmode.c`, `foeappl.c`）
