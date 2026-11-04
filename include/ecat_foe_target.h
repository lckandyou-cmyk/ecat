/**
 * @file ecat_foe_target.h
 * @brief 目标驱动接口抽象定义 (依据 SAD-v0.1 第 4.2 节草案)
 *
 * 目标驱动是用户对接实际物理存储介质 (如片内 Flash、外部 SPI Flash、I2C EEPROM)
 * 的纯 C 函数指针集合。分区容量只认本地静态配置 ECAT_FOE_xxx_MAX_SIZE，驱动不暴露容量。
 *
 * 驱动生命周期与清理契约 (核心严格保证):
 * 1. init 失败清理责任: 若 init() 自身返回非零失败，表示驱动初始化未完成，驱动内部自行负责
 *    清理未初始化半成品资源；核心状态机绝不调用 abort()。
 * 2. init 成功后异常清理责任: 一旦 init() 成功返回 0，驱动生命周期会话正式激活。此后发生的
 *    任何后续失败 (包括紧接着的 invalidate 失败、按需擦除失败、写入失败、流 CRC 不匹配、
 *    物理回读失败/CRC 不符、finalize 失败或外部中止)，核心机均严格保证调用且仅调用一次 abort()。
 * 3. finalize 成功后互斥保护: finalize() 执行成功后固件置为有效，核心状态机永久锁定会话，
 *    此后即使发生任何后续清理动作或收到新请求，绝不会再调用 abort()。
 *
 * 驱动调用时序 (在回调内部同步完成):
 * init -> invalidate -> [erase_to (按需) / write]* -> 流 CRC 比对 -> [core: read & crc32]* -> finalize()
 * 任何一步失败立即转向调用 abort()。finalize() 与 abort() 严格互斥。
 */

#ifndef ECAT_FOE_TARGET_H
#define ECAT_FOE_TARGET_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ecat_foe_target_driver {
    /**
     * @brief 1. 初始化存储介质
     * 触发时机: 头部装配完成且"擦除前必须全过"核验通过后调用
     * 职责: 解锁 Flash 控制器、建立外设通信、校验供电状态
     * 清理契约: 若返回非 0 失败，驱动自行负责清理未完成资源，核心不调用 abort()
     * @param user_ctx 用户自定义上下文指针
     * @return 0 成功，非 0 失败
     */
    int (*init)(void *user_ctx);

    /**
     * @brief 2. 置无效标记 (擦除前调用，防止掉电误判)
     * 触发时机: init 成功后、执行任何擦除动作之前调用
     * 职责: 在物理介质中清除该目标的"有效/就绪"标记
     * @param user_ctx 用户自定义上下文指针
     * @return 0 成功，非 0 失败
     */
    int (*invalidate)(void *user_ctx);

    /**
     * @brief 3. 按需擦除到指定结束偏移
     * 触发时机: 数据流写入偏移超出核心机维护的当前擦除水位时同步调用
     * 职责: 驱动自行负责扇区划分与擦除，擦除覆盖到 need_end 所需的所有扇区，
     *       并通过 *erased_end 返回实际已擦到的物理边界
     * @param need_end   当前必须覆盖到的字节偏移 (相对分区起始)
     * @param erased_end 输出实际擦除完成的物理边界
     * @param user_ctx   用户自定义上下文指针
     * @return 0 成功，非 0 失败
     */
    int (*erase_to)(uint32_t need_end, uint32_t *erased_end, void *user_ctx);

    /**
     * @brief 4. 对齐分块写入
     * 触发时机: 数据凑满 write_unit 或是最后一块以 0xFF 补齐后调用
     * @param offset 分区内相对偏移 (字节)
     * @param data   写入数据缓冲区
     * @param len    数据长度 (字节，必为 write_unit 整倍数)
     * @param user_ctx 用户自定义上下文指针
     * @return 0 成功，非 0 失败
     */
    int (*write)(uint32_t offset, const uint8_t *data, uint32_t len, void *user_ctx);

    /**
     * @brief 5. 物理介质底层读取
     * 触发时机: 核心在数据接收完毕后分步调用以计算读回 CRC32
     * @param offset 分区内相对偏移 (字节)
     * @param buf    读取输出缓冲区
     * @param len    读取长度 (字节)
     * @param user_ctx 用户自定义上下文指针
     * @return 0 成功，非 0 失败
     */
    int (*read)(uint32_t offset, uint8_t *buf, uint32_t len, void *user_ctx);

    /**
     * @brief 6. 成功提交
     * 触发时机: 仅在全部数据流接收、流 CRC 比对及物理回读 CRC32 均无误后调用
     * 职责: 写入物理固件有效标记、更新元数据或切换活动分区
     * 清理契约: 执行成功后固件永久置为有效，核心锁定，后续绝不调用 abort()
     * @param user_ctx 用户自定义上下文指针
     * @return 0 成功，非 0 失败
     */
    int (*finalize)(void *user_ctx);

    /**
     * @brief 7. 中止清理
     * 触发时机: init 成功后发生的任何错误、超时或中止时调用
     * 职责: 锁定硬件控制器、保持固件处于无效状态、释放驱动资源
     * 清理契约: 每个激活会话至多调用 1 次；与 finalize() 严格互斥
     * @param user_ctx 用户自定义上下文指针
     */
    void (*abort)(void *user_ctx);
} ecat_foe_target_driver_t;

#ifdef __cplusplus
}
#endif

#endif /* ECAT_FOE_TARGET_H */
