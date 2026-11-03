#!/usr/bin/env python3
"""
tests/generate_vectors.py: 生成测试固件向量包存入 tests/vectors/
"""

import os
import sys
import subprocess

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VECTORS_DIR = os.path.join(ROOT_DIR, 'tests', 'vectors')
PACK_TOOL = os.path.join(ROOT_DIR, 'tools', 'efw_pack.py')

os.makedirs(VECTORS_DIR, exist_ok=True)


def generate_all():
    # 1. 规范正例 (MCU 目标, 16 字节线性测试序列)
    p_mcu = bytes(range(16))
    bin_mcu = os.path.join(VECTORS_DIR, 'tmp_mcu.bin')
    with open(bin_mcu, 'wb') as f:
        f.write(p_mcu)

    cmd_pos = [
        sys.executable, PACK_TOOL, 'pack',
        '--bin', bin_mcu,
        '--project-id', '0x10010001',
        '--project-name', 'SERVO_DRV',
        '--hardware-id', '0x00000001',
        '--hw-ver-min', '1',
        '--hw-ver-max', '1',
        '--target', 'MCU',
        '--fw-version', '1.2.3.4',
        '--min-allow-version', '1.0.0.0',
        '--out', os.path.join(VECTORS_DIR, 'positive_mcu.efw')
    ]
    subprocess.check_call(cmd_pos)

    # 2. 尾部填充例 (MCU 目标, 原始 448 字节, 加上头 64 字节恰好 512 字节, 单包 512 时补 1 字节 0xFF)
    p_pad = bytes([i % 256 for i in range(448)])
    bin_pad = os.path.join(VECTORS_DIR, 'tmp_pad.bin')
    with open(bin_pad, 'wb') as f:
        f.write(p_pad)

    cmd_pad = [
        sys.executable, PACK_TOOL, 'pack',
        '--bin', bin_pad,
        '--project-id', '0x10010001',
        '--project-name', 'SERVO_DRV',
        '--hardware-id', '0x00000001',
        '--target', 'MCU',
        '--fw-version', '1.2.3.4',
        '--foe-packet-size', '512',
        '--out', os.path.join(VECTORS_DIR, 'padded_mcu.efw')
    ]
    subprocess.check_call(cmd_pad)

    # 3. SII 偶数填充例 (SII 目标, 原始 448 字节, 加上头 64 字节恰好 512 字节, 单包 512 时补 2 字节 0xFF)
    cmd_sii_pad = [
        sys.executable, PACK_TOOL, 'pack',
        '--bin', bin_pad,
        '--project-id', '0x10010001',
        '--project-name', 'SERVO_DRV',
        '--hardware-id', '0x00000001',
        '--target', 'SII',
        '--fw-version', '1.0.0.0',
        '--foe-packet-size', '512',
        '--out', os.path.join(VECTORS_DIR, 'padded_sii.efw')
    ]
    subprocess.check_call(cmd_sii_pad)

    # 4. FPGA 目标 (Target 2)
    p_fpga = bytes([(i * 3) % 256 for i in range(64)])
    bin_fpga = os.path.join(VECTORS_DIR, 'tmp_fpga.bin')
    with open(bin_fpga, 'wb') as f:
        f.write(p_fpga)
    cmd_fpga = [
        sys.executable, PACK_TOOL, 'pack',
        '--bin', bin_fpga,
        '--project-id', '0x10010001',
        '--project-name', 'SERVO_DRV',
        '--hardware-id', '0x00000001',
        '--target', 'FPGA',
        '--fw-version', '2.0.0.0',
        '--out', os.path.join(VECTORS_DIR, 'target_fpga.efw')
    ]
    subprocess.check_call(cmd_fpga)

    # 5. SII 目标无填充常规例 (Target 3, 偶数 32 字节)
    p_sii = bytes([(i * 7) % 256 for i in range(32)])
    bin_sii = os.path.join(VECTORS_DIR, 'tmp_sii.bin')
    with open(bin_sii, 'wb') as f:
        f.write(p_sii)
    cmd_sii = [
        sys.executable, PACK_TOOL, 'pack',
        '--bin', bin_sii,
        '--project-id', '0x10010001',
        '--project-name', 'SERVO_DRV',
        '--hardware-id', '0x00000001',
        '--target', 'SII',
        '--fw-version', '1.0.0.0',
        '--out', os.path.join(VECTORS_DIR, 'target_sii.efw')
    ]
    subprocess.check_call(cmd_sii)

    # 6. EEPROM 目标 (Target 4, 48 字节)
    p_eeprom = bytes([(i * 11) % 256 for i in range(48)])
    bin_eeprom = os.path.join(VECTORS_DIR, 'tmp_eeprom.bin')
    with open(bin_eeprom, 'wb') as f:
        f.write(p_eeprom)
    cmd_eeprom = [
        sys.executable, PACK_TOOL, 'pack',
        '--bin', bin_eeprom,
        '--project-id', '0x10010001',
        '--project-name', 'SERVO_DRV',
        '--hardware-id', '0x00000001',
        '--target', 'EEPROM',
        '--fw-version', '1.0.1.0',
        '--out', os.path.join(VECTORS_DIR, 'target_eeprom.efw')
    ]
    subprocess.check_call(cmd_eeprom)

    # 清理中间 bin
    for tmp in [bin_mcu, bin_pad, bin_fpga, bin_sii, bin_eeprom]:
        if os.path.exists(tmp):
            os.remove(tmp)

    print("已成功在 tests/vectors/ 下生成全部固件测试向量。")


if __name__ == '__main__':
    generate_all()
