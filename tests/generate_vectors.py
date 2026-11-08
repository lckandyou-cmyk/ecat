#!/usr/bin/env python3
"""
tests/generate_vectors.py: 生成测试固件向量包存入 tests/vectors/ 或指定目录
"""

import os
import sys
import subprocess
import tempfile
import argparse

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK_TOOL = os.path.join(ROOT_DIR, 'tools', 'efw_pack.py')


def safe_check_not_symlink(path: str):
    """安全核验：拒绝向符号链接写入向量，防止符号链接劫持或越界覆盖"""
    if os.path.islink(path):
        raise RuntimeError(f"安全拒绝：目标路径为符号链接: {path}")


def generate_all(out_dir: str = None, overwrite: bool = False):
    if not out_dir:
        parser = argparse.ArgumentParser(description="生成测试固件向量包")
        parser.add_argument('--out-dir', type=str, default=None, help="向量文件输出目录")
        parser.add_argument('--overwrite', action='store_true', default=False, help="允许覆盖已有普通文件")
        args, _ = parser.parse_known_args()
        if args.out_dir:
            out_dir = os.path.abspath(args.out_dir)
        elif os.environ.get("VECTORS_DIR"):
            out_dir = os.path.abspath(os.environ.get("VECTORS_DIR"))
        else:
            out_dir = os.path.join(ROOT_DIR, 'tests', 'vectors')
        overwrite = overwrite or args.overwrite

    safe_check_not_symlink(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    extra_args = ['--overwrite'] if overwrite else []

    # 使用独立的临时目录存放中间 bin 文件，不污染目标向量目录
    with tempfile.TemporaryDirectory(prefix="foe_vec_tmp_") as tmp_bin_dir:
        # 1. 规范正例 (MCU 目标, 16 字节线性测试序列)
        p_mcu = bytes(range(16))
        bin_mcu = os.path.join(tmp_bin_dir, 'tmp_mcu.bin')
        with open(bin_mcu, 'wb') as f:
            f.write(p_mcu)

        out_pos = os.path.join(out_dir, 'positive_mcu.efw')
        safe_check_not_symlink(out_pos)
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
            '--out', out_pos
        ] + extra_args
        subprocess.check_call(cmd_pos)

        # 2. 尾部填充例 (MCU 目标, 原始 364 字节, 加上头 148 字节恰好 512 字节)
        p_pad = bytes([i % 256 for i in range(364)])
        bin_pad = os.path.join(tmp_bin_dir, 'tmp_pad.bin')
        with open(bin_pad, 'wb') as f:
            f.write(p_pad)

        out_pad = os.path.join(out_dir, 'padded_mcu.efw')
        safe_check_not_symlink(out_pad)
        cmd_pad = [
            sys.executable, PACK_TOOL, 'pack',
            '--bin', bin_pad,
            '--project-id', '0x10010001',
            '--project-name', 'SERVO_DRV',
            '--hardware-id', '0x00000001',
            '--target', 'MCU',
            '--fw-version', '1.2.3.4',
            '--foe-packet-size', '512',
            '--out', out_pad
        ] + extra_args
        subprocess.check_call(cmd_pad)

        # 3. SII 偶数填充例 (SII 目标, 原始 364 字节, 加上头 148 字节恰好 512 字节)
        out_sii_pad = os.path.join(out_dir, 'padded_sii.efw')
        safe_check_not_symlink(out_sii_pad)
        cmd_sii_pad = [
            sys.executable, PACK_TOOL, 'pack',
            '--bin', bin_pad,
            '--project-id', '0x10010001',
            '--project-name', 'SERVO_DRV',
            '--hardware-id', '0x00000001',
            '--target', 'SII',
            '--fw-version', '1.0.0.0',
            '--foe-packet-size', '512',
            '--out', out_sii_pad
        ] + extra_args
        subprocess.check_call(cmd_sii_pad)

        # 4. FPGA 目标 (Target 2)
        p_fpga = bytes([(i * 3) % 256 for i in range(64)])
        bin_fpga = os.path.join(tmp_bin_dir, 'tmp_fpga.bin')
        with open(bin_fpga, 'wb') as f:
            f.write(p_fpga)
        out_fpga = os.path.join(out_dir, 'target_fpga.efw')
        safe_check_not_symlink(out_fpga)
        cmd_fpga = [
            sys.executable, PACK_TOOL, 'pack',
            '--bin', bin_fpga,
            '--project-id', '0x10010001',
            '--project-name', 'SERVO_DRV',
            '--hardware-id', '0x00000001',
            '--target', 'FPGA',
            '--fw-version', '2.0.0.0',
            '--out', out_fpga
        ] + extra_args
        subprocess.check_call(cmd_fpga)

        # 5. SII 目标无填充常规例 (Target 3, 偶数 32 字节)
        p_sii = bytes([(i * 7) % 256 for i in range(32)])
        bin_sii = os.path.join(tmp_bin_dir, 'tmp_sii.bin')
        with open(bin_sii, 'wb') as f:
            f.write(p_sii)
        out_sii = os.path.join(out_dir, 'target_sii.efw')
        safe_check_not_symlink(out_sii)
        cmd_sii = [
            sys.executable, PACK_TOOL, 'pack',
            '--bin', bin_sii,
            '--project-id', '0x10010001',
            '--project-name', 'SERVO_DRV',
            '--hardware-id', '0x00000001',
            '--target', 'SII',
            '--fw-version', '1.0.0.0',
            '--out', out_sii
        ] + extra_args
        subprocess.check_call(cmd_sii)

        # 6. EEPROM 目标 (Target 4, 48 字节)
        p_eeprom = bytes([(i * 11) % 256 for i in range(48)])
        bin_eeprom = os.path.join(tmp_bin_dir, 'tmp_eeprom.bin')
        with open(bin_eeprom, 'wb') as f:
            f.write(p_eeprom)
        out_eeprom = os.path.join(out_dir, 'target_eeprom.efw')
        safe_check_not_symlink(out_eeprom)
        cmd_eeprom = [
            sys.executable, PACK_TOOL, 'pack',
            '--bin', bin_eeprom,
            '--project-id', '0x10010001',
            '--project-name', 'SERVO_DRV',
            '--hardware-id', '0x00000001',
            '--target', 'EEPROM',
            '--fw-version', '1.0.1.0',
            '--out', out_eeprom
        ] + extra_args
        subprocess.check_call(cmd_eeprom)

    print(f"已在 {out_dir} 下生成固件测试向量。")


if __name__ == '__main__':
    generate_all()
