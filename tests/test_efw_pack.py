#!/usr/bin/env python3
"""
tests/test_efw_pack.py: Python efw_pack.py 打包与交叉验证测试
"""

import os
import sys
import tempfile
import subprocess
import binascii

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK_TOOL = os.path.join(ROOT_DIR, 'tools', 'efw_pack.py')


def test_positive_cross_check():
    """验证 Python 打包输出与 efw-format 8.1 规范转储逐字节一致"""
    with tempfile.TemporaryDirectory() as tmpdir:
        payload = bytes(range(16))
        bin_path = os.path.join(tmpdir, 'payload.bin')
        with open(bin_path, 'wb') as f:
            f.write(payload)

        out_path = os.path.join(tmpdir, 'out.efw')
        cmd = [
            sys.executable, PACK_TOOL, 'pack',
            '--bin', bin_path,
            '--project-id', '0x10010001',
            '--project-name', 'SERVO_DRV',
            '--hardware-id', '0x00000001',
            '--hw-ver-min', '1',
            '--hw-ver-max', '1',
            '--target', 'MCU',
            '--fw-version', '1.2.3.4',
            '--min-allow-version', '1.0.0.0',
            '--out', out_path
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode == 0, f"打包失败: {res.stderr}"

        # 比对 64 字节头部十六进制
        with open(out_path, 'rb') as f:
            data = f.read()

        expected_header_hex = (
            '45434657010040000100011001000000'
            '01000100010000000403020100000001'
            '1000000088E2CECE0000000000000000'
            '000000000000000000000000E352C261'
        )
        actual_header_hex = data[:64].hex().upper()
        assert actual_header_hex == expected_header_hex, "打包头部十六进制与规范不一致"
        assert data[64:] == payload, "有效负载数据不一致"

        # 运行 inspect 验证
        res_insp = subprocess.run([sys.executable, PACK_TOOL, 'inspect', out_path], capture_output=True, text=True)
        assert res_insp.returncode == 0, f"inspect 验证失败: {res_insp.stderr}"
    print("  [PASS] Python 打包正例与规范 8.1 逐字节交叉比对")


def test_filename_generator():
    """验证第 7 节文件名规范生成"""
    with tempfile.TemporaryDirectory() as tmpdir:
        bin_path = os.path.join(tmpdir, 'test.bin')
        with open(bin_path, 'wb') as f:
            f.write(b'\x00' * 10)

        # 切换工作目录以便测试默认文件名生成
        orig_cwd = os.getcwd()
        os.chdir(tmpdir)
        try:
            cmd = [
                sys.executable, PACK_TOOL, 'pack',
                '--bin', bin_path,
                '--project-id', '0x10010001',
                '--project-name', 'SERVO_DRV',
                '--hardware-id', '0x00000001',
                '--target', 'MCU',
                '--fw-version', '1.2.3.4'
            ]
            res = subprocess.run(cmd, capture_output=True, text=True)
            assert res.returncode == 0
            expected_name = 'SERVO_DRV_MCU_V1.2.3.4.efw'
            assert os.path.exists(expected_name), f"期望生成文件名 {expected_name} 未找到"
        finally:
            os.chdir(orig_cwd)
    print("  [PASS] 第 7 节规范文件名生成格式 SERVO_DRV_MCU_V1.2.3.4.efw")


def test_foe_packet_size_padding():
    """验证 --foe-packet-size N 尾部填充规避空包 (第 2.2 节)"""
    with tempfile.TemporaryDirectory() as tmpdir:
        # N = 512
        # header_length = 64
        # 若 payload = 512 - 64 = 448 字节，则 total = 512，为 512 整数倍
        bin_448 = os.path.join(tmpdir, 'p448.bin')
        with open(bin_448, 'wb') as f:
            f.write(b'\xAA' * 448)

        # 1. MCU 目标 (非 SII) -> 补 1 字节 0xFF，总长变为 513
        out_mcu = os.path.join(tmpdir, 'mcu.efw')
        cmd1 = [
            sys.executable, PACK_TOOL, 'pack',
            '--bin', bin_448,
            '--project-id', '0x10010001',
            '--hardware-id', '0x00000001',
            '--target', 'MCU',
            '--fw-version', '1.0.0.0',
            '--foe-packet-size', '512',
            '--out', out_mcu
        ]
        res1 = subprocess.run(cmd1, capture_output=True, text=True)
        assert res1.returncode == 0
        with open(out_mcu, 'rb') as f:
            data_mcu = f.read()
        assert len(data_mcu) == 64 + 449, f"MCU 填充后总长应为 513 (64+449)，实际 {len(data_mcu)}"
        assert data_mcu[-1] == 0xFF, "末尾填充字节应当为 0xFF"

        # 2. SII 目标 -> 必须补 2 的整数倍 (2 字节 0xFF)，总长变为 514
        out_sii = os.path.join(tmpdir, 'sii.efw')
        cmd2 = [
            sys.executable, PACK_TOOL, 'pack',
            '--bin', bin_448,
            '--project-id', '0x10010001',
            '--hardware-id', '0x00000001',
            '--target', 'SII',
            '--fw-version', '1.0.0.0',
            '--foe-packet-size', '512',
            '--out', out_sii
        ]
        res2 = subprocess.run(cmd2, capture_output=True, text=True)
        assert res2.returncode == 0
        with open(out_sii, 'rb') as f:
            data_sii = f.read()
        assert len(data_sii) == 64 + 450, f"SII 填充后总长应为 514 (64+450)，实际 {len(data_sii)}"
        assert data_sii[-2:] == b'\xFF\xFF', "SII 末尾必须补 2 字节 0xFF"
    print("  [PASS] --foe-packet-size 512 规避填充 (MCU 补 1B / SII 偶数补 2B)")


if __name__ == '__main__':
    print("--- 运行 Python efw_pack 工具测试 ---")
    test_positive_cross_check()
    test_filename_generator()
    test_foe_packet_size_padding()
    print("--- Python efw_pack 工具测试全部通过 ---")
