#!/usr/bin/env python3
"""
tests/test_efw_pack.py: Python efw_pack.py 打包与交叉验证测试
"""

import os
import sys
import tempfile
import subprocess
import binascii
import signal

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK_TOOL = os.path.join(ROOT_DIR, 'tools', 'efw_pack.py')


def test_overwrite_write_failure_preserves_output():
    """A failed pack or extract must not truncate an existing output."""
    if os.name != 'posix' or not hasattr(signal, 'SIGXFSZ'):
        return
    import resource
    with tempfile.TemporaryDirectory() as tmpdir:
        source = os.path.join(tmpdir, 'input.bin')
        package = os.path.join(tmpdir, 'source.efw')
        output = os.path.join(tmpdir, 'output.bin')
        with open(source, 'wb') as f:
            f.write(b'A' * 4096)
        pack = [sys.executable, PACK_TOOL, 'pack', '-i', source, '-p', '1', '-d', '1',
                '-t', 'MCU', '-v', '1.0.0.0', '-o']
        assert subprocess.run(pack + [package], capture_output=True).returncode == 0

        def limit_output():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

        for args in (pack + [output, '--overwrite'],
                     [sys.executable, PACK_TOOL, 'unpack', package,
                      '--extract-bin', output, '--overwrite']):
            with open(output, 'wb') as f:
                f.write(b'original output')
            result = subprocess.run(args, capture_output=True, preexec_fn=limit_output)
            assert result.returncode != 0
            with open(output, 'rb') as f:
                assert f.read() == b'original output'
            assert not any(name.startswith('.ecat-efw-') for name in os.listdir(tmpdir))
        assert subprocess.run(pack + [output, '--overwrite'], capture_output=True).returncode == 0
        assert subprocess.run([sys.executable, PACK_TOOL, 'unpack', package,
                               '--extract-bin', output, '--overwrite'], capture_output=True).returncode == 0
        with open(output, 'rb') as f:
            assert f.read() == b'A' * 4096
    print('  [PASS] pack/unpack 写入失败保留原输出，成功后原子替换')


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

        with open(out_path, 'rb') as f:
            data = f.read()
        assert len(data) == 148 + len(payload)
        assert data[:4] == b'ECFW'
        assert int.from_bytes(data[4:6], 'little') == 2
        assert int.from_bytes(data[6:8], 'little') == 148
        assert data[60:64] == b'SIG2'
        assert data[64:68] == bytes((1, 0, 0, 0))
        assert data[68:80] == bytes(12)
        assert data[80:144] == bytes(64)
        assert int.from_bytes(data[144:148], 'little') == binascii.crc32(data[:144])
        assert data[148:] == payload, "有效负载数据不一致"

        # 运行 inspect 验证
        res_insp = subprocess.run([sys.executable, PACK_TOOL, 'inspect', out_path], capture_output=True, text=True)
        assert res_insp.returncode == 0, f"inspect 验证失败: {res_insp.stderr}"
        renamed_path = os.path.join(tmpdir, 'customer-file.bin')
        os.rename(out_path, renamed_path)
        res_info = subprocess.run(
            [sys.executable, PACK_TOOL, 'info', renamed_path], capture_output=True, text=True)
        assert res_info.returncode == 0, f"info 验证失败: {res_info.stderr}"
        assert 'V1.2.3.4' in res_info.stdout
        assert '0x10010001' in res_info.stdout
        assert 'Header CRC32' in res_info.stdout and 'Payload CRC32' in res_info.stdout
        assert '包内打包日期:     未提供' in res_info.stdout
        assert '文件路径:' not in res_info.stdout, "info 应保持简洁，不应混入 inspect 详细报告"

        # Historical headers are rejected: only the current format is supported.
        legacy = bytearray(64 + len(payload))
        legacy[:40] = data[:40]
        legacy[4:6] = (1).to_bytes(2, 'little')
        legacy[6:8] = (64).to_bytes(2, 'little')
        legacy[64:] = payload
        legacy[60:64] = (binascii.crc32(legacy[:60]) & 0xffffffff).to_bytes(4, 'little')
        legacy_path = os.path.join(tmpdir, 'legacy-v1.efw')
        with open(legacy_path, 'wb') as f:
            f.write(legacy)
        legacy_info = subprocess.run([sys.executable, PACK_TOOL, 'info', legacy_path],
                                     capture_output=True, text=True)
        assert legacy_info.returncode != 0, "旧格式必须拒绝"

        corrupted_path = os.path.join(tmpdir, 'corrupt.anything')
        with open(renamed_path, 'rb') as f:
            corrupt_data = bytearray(f.read())
        corrupt_data[-1] ^= 1
        with open(corrupted_path, 'wb') as f:
            f.write(corrupt_data)
        res_bad_info = subprocess.run(
            [sys.executable, PACK_TOOL, 'info', corrupted_path], capture_output=True, text=True)
        assert res_bad_info.returncode != 0, "info 不应把 CRC 错误的固件包判为有效"
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


def test_empty_payload_rejected_without_output():
    """拒绝空固件，且不创建输出文件。"""
    with tempfile.TemporaryDirectory() as tmpdir:
        bin_path = os.path.join(tmpdir, 'empty.bin')
        out_path = os.path.join(tmpdir, 'empty.efw')
        with open(bin_path, 'wb'):
            pass
        cmd = [
            sys.executable, PACK_TOOL, 'pack',
            '--bin', bin_path,
            '--project-id', '0x10010001',
            '--hardware-id', '0x00000001',
            '--target', 'MCU',
            '--fw-version', '1.0.0.0',
            '--out', out_path
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode != 0, "空载荷不应生成可分发的固件包"
        assert '有效负载不能为空' in res.stderr, res.stderr
        assert not os.path.exists(out_path), "拒绝空固件时不应创建输出包"

        # Also reject a hand-crafted, CRC-correct empty container at inspect time;
        # otherwise users could bypass pack's validation and extract it as valid.
        empty_efw = os.path.join(tmpdir, 'crafted-empty.efw')
        header = bytearray(148)
        header[:4] = b'ECFW'
        header[4:8] = (2).to_bytes(2, 'little') + (148).to_bytes(2, 'little')
        header[8:16] = bytes.fromhex('0100011001000000')
        header[16:24] = bytes.fromhex('0100010001000000')
        header[24:32] = bytes.fromhex('0000000000000000')
        header[60:68] = b'SIG2' + bytes((1, 0, 0, 0))
        header[32:40] = bytes(8)  # payload length and CRC of the empty payload
        header[144:148] = binascii.crc32(header[:144]).to_bytes(4, 'little')
        with open(empty_efw, 'wb') as f:
            f.write(header)
        extracted = os.path.join(tmpdir, 'should-not-extract.bin')
        inspected = subprocess.run(
            [sys.executable, PACK_TOOL, 'inspect', empty_efw,
             '--extract-bin', extracted], capture_output=True, text=True)
        assert inspected.returncode != 0, "inspect 不应接受手工构造的空固件"
        assert '有效负载不能为空' in inspected.stderr, inspected.stderr
        assert not os.path.exists(extracted), "拒绝空固件时不得创建提取文件"
    print("  [PASS] 空固件打包被拒绝且未创建输出文件")


def test_foe_packet_size_padding():
    """验证 --foe-packet-size N 尾部填充规避空包 (第 2.2 节)"""
    with tempfile.TemporaryDirectory() as tmpdir:
        # N = 512
        # header_length = 148；若 payload = 512 - 148 = 364 字节，总长恰为 512。
        bin_364 = os.path.join(tmpdir, 'p364.bin')
        with open(bin_364, 'wb') as f:
            f.write(b'\xAA' * 364)

        # 1. MCU 目标 (非 SII) -> 补 1 字节 0xFF，总长变为 513
        out_mcu = os.path.join(tmpdir, 'mcu.efw')
        cmd1 = [
            sys.executable, PACK_TOOL, 'pack',
            '--bin', bin_364,
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
        assert len(data_mcu) == 148 + 365, f"MCU 填充后总长应为 513，实际 {len(data_mcu)}"
        assert data_mcu[-1] == 0xFF, "末尾填充字节应当为 0xFF"

        # 2. SII 目标 -> 必须补 2 的整数倍 (2 字节 0xFF)，总长变为 514
        out_sii = os.path.join(tmpdir, 'sii.efw')
        cmd2 = [
            sys.executable, PACK_TOOL, 'pack',
            '--bin', bin_364,
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
        assert len(data_sii) == 148 + 366, f"SII 填充后总长应为 514，实际 {len(data_sii)}"
        assert data_sii[-2:] == b'\xFF\xFF', "SII 末尾必须补 2 字节 0xFF"
        for target, size in [('MCU', 1), ('SII', 2)]:
            output = os.path.join(tmpdir, f'invalid-{target}.efw')
            cmd = [sys.executable, PACK_TOOL, 'pack', '--bin', bin_364,
                   '--project-id', '0x10010001', '--hardware-id', '1',
                   '--target', target, '--fw-version', '1.0.0.0',
                   '--foe-packet-size', str(size), '--out', output]
            result = subprocess.run(cmd, capture_output=True, text=True)
            assert result.returncode != 0 and not os.path.exists(output)
            assert '无法通过允许的填充' in result.stderr
    print("  [PASS] --foe-packet-size 512 规避填充 (MCU 补 1B / SII 偶数补 2B)")


def test_signed_package_round_trip():
    """确定性 Ed25519ph 向量：生成、检查、错误密钥和篡改拒绝。"""
    try:
        from nacl import bindings
        required = ('crypto_sign_seed_keypair', 'crypto_sign_ed25519ph_state',
                    'crypto_sign_ed25519ph_update', 'crypto_sign_ed25519ph_final_create')
        if not all(hasattr(bindings, name) for name in required):
            raise ImportError
    except ImportError:
        print("  [SKIP] PyNaCl/librariesodium Ed25519ph 未安装；CI 打包任务会安装并运行此测试")
        return

    with tempfile.TemporaryDirectory() as tmpdir:
        seed = bytes(range(32))
        wrong_seed = bytes(range(1, 33))
        seed_path = os.path.join(tmpdir, 'private-seed.bin')
        wrong_seed_path = os.path.join(tmpdir, 'wrong-seed.bin')
        pub_path = os.path.join(tmpdir, 'trusted-public.bin')
        payload_path = os.path.join(tmpdir, 'payload.bin')
        signed_path = os.path.join(tmpdir, 'signed.efw')
        with open(seed_path, 'wb') as f:
            f.write(seed)
        with open(wrong_seed_path, 'wb') as f:
            f.write(wrong_seed)
        with open(payload_path, 'wb') as f:
            f.write(bytes((i * 13 + 7) & 0xff for i in range(257)))

        cmd = [sys.executable, PACK_TOOL, 'pack', '--bin', payload_path,
               '--project-id', '0x10010001', '--hardware-id', '1', '--target', 'MCU',
               '--fw-version', '2.3.4.5', '--min-allow-version', '1.0.0.0',
               '--signing-key', seed_path, '--public-key-out', pub_path, '--out', signed_path]
        packed = subprocess.run(cmd, capture_output=True, text=True)
        assert packed.returncode == 0, packed.stderr
        assert seed.hex() not in packed.stdout + packed.stderr
        assert os.path.getsize(pub_path) == 32
        fixture_path = os.path.join(ROOT_DIR, 'tests', 'fixtures',
                                    'efw-v2-ed25519ph-monocypher.efw')
        with open(fixture_path, 'rb') as f:
            reference_vector = f.read()
        with open(signed_path, 'rb') as f:
            cli_vector = f.read()
        assert cli_vector == reference_vector, \
            "PyNaCl CLI output must byte-match the Monocypher Ed25519ph reference vector"

        info = subprocess.run([sys.executable, PACK_TOOL, 'info', signed_path],
                              capture_output=True, text=True)
        assert info.returncode == 0 and '存在，未验证' in info.stdout
        verified = subprocess.run([sys.executable, PACK_TOOL, 'info', signed_path,
                                   '--public-key', pub_path], capture_output=True, text=True)
        assert verified.returncode == 0, verified.stderr
        assert 'Ed25519ph 已验证' in verified.stdout
        original_public_key = open(pub_path, 'rb').read()
        if hasattr(os, 'O_NOFOLLOW'):
            for output_path in (pub_path, os.path.join(tmpdir, 'public-hardlink')):
                if output_path != pub_path:
                    try:
                        os.link(pub_path, output_path)
                    except OSError:
                        continue
                attempted = subprocess.run(
                    [sys.executable, PACK_TOOL, 'unpack', signed_path,
                     '--public-key', pub_path, '--extract-bin', output_path, '--overwrite'],
                    capture_output=True, text=True)
                assert attempted.returncode != 0, '提取不得覆盖验签公钥或其硬链接'
                assert open(pub_path, 'rb').read() == original_public_key
                if output_path != pub_path:
                    os.unlink(output_path)

        # A public-key output failure must not create or overwrite the package.
        failed_package = os.path.join(tmpdir, 'failed-package.efw')
        missing_pub = os.path.join(tmpdir, 'missing-directory', 'public.bin')
        fail_cmd = cmd.copy()
        fail_cmd[fail_cmd.index(pub_path)] = missing_pub
        fail_cmd[fail_cmd.index(signed_path)] = failed_package
        failed = subprocess.run(fail_cmd, capture_output=True, text=True)
        assert failed.returncode != 0 and not os.path.exists(failed_package)
        existing_package = os.path.join(tmpdir, 'existing-package.efw')
        with open(existing_package, 'wb') as f:
            f.write(b'original package')
        if hasattr(os, 'O_NOFOLLOW'):
            overwrite_cmd = fail_cmd.copy()
            overwrite_cmd[overwrite_cmd.index(failed_package)] = existing_package
            overwrite_cmd.append('--overwrite')
            failed = subprocess.run(overwrite_cmd, capture_output=True, text=True)
            assert failed.returncode != 0
            assert open(existing_package, 'rb').read() == b'original package'
        os.mkdir(os.path.dirname(missing_pub))
        retried = subprocess.run(fail_cmd, capture_output=True, text=True)
        assert retried.returncode == 0, retried.stderr

        # If package creation fails, remove only the public key created by this call.
        orphan_pub = os.path.join(tmpdir, 'must-not-remain.pub')
        fail_cmd[fail_cmd.index(missing_pub)] = orphan_pub
        fail_cmd[fail_cmd.index(failed_package)] = signed_path
        failed = subprocess.run(fail_cmd, capture_output=True, text=True)
        assert failed.returncode != 0 and not os.path.exists(orphan_pub)
        wrong_pub, _ = bindings.crypto_sign_seed_keypair(wrong_seed)
        wrong_pub_path = os.path.join(tmpdir, 'wrong-public.bin')
        with open(wrong_pub_path, 'wb') as f:
            f.write(wrong_pub)
        wrong = subprocess.run([sys.executable, PACK_TOOL, 'inspect', signed_path,
                                '--public-key', wrong_pub_path], capture_output=True, text=True)
        assert wrong.returncode != 0 and ('Key ID' in wrong.stderr or 'Key ID' in wrong.stdout)

        original = bytearray(open(signed_path, 'rb').read())
        mutations = (('payload', 148, None), ('target', 20, 2), ('version', 24, None))
        for name, offset, explicit_value in mutations:
            altered = bytearray(original)
            altered[offset] = explicit_value if explicit_value is not None else altered[offset] ^ 1
            if name != 'payload':
                altered[144:148] = binascii.crc32(altered[:144]).to_bytes(4, 'little')
            bad_path = os.path.join(tmpdir, f'altered-{name}.efw')
            with open(bad_path, 'wb') as f:
                f.write(altered)
            result = subprocess.run([sys.executable, PACK_TOOL, 'info', bad_path,
                                     '--public-key', pub_path], capture_output=True, text=True)
            assert result.returncode != 0, f'{name} tampering must fail signature verification'

        truncated_path = os.path.join(tmpdir, 'truncated.efw')
        with open(truncated_path, 'wb') as f:
            f.write(original[:-1])
        truncated = subprocess.run([sys.executable, PACK_TOOL, 'info', truncated_path,
                                    '--public-key', pub_path], capture_output=True, text=True)
        assert truncated.returncode != 0
        malformed = bytearray(original)
        malformed[66:68] = (99).to_bytes(2, 'little')
        malformed[144:148] = binascii.crc32(malformed[:144]).to_bytes(4, 'little')
        malformed_path = os.path.join(tmpdir, 'malformed.efw')
        with open(malformed_path, 'wb') as f:
            f.write(malformed)
        bad_format = subprocess.run([sys.executable, PACK_TOOL, 'info', malformed_path],
                                    capture_output=True, text=True)
        assert bad_format.returncode != 0

        no_signing_key = subprocess.run(
            [sys.executable, PACK_TOOL, 'pack', '--bin', payload_path,
             '--project-id', '1', '--hardware-id', '1', '--target', 'MCU',
             '--fw-version', '1.0.0.0', '--public-key-out', os.path.join(tmpdir, 'orphan.pub')],
            capture_output=True, text=True)
        assert no_signing_key.returncode != 0
    print("  [PASS] Ed25519ph signed package vector, signature verification and tamper rejection")


if __name__ == '__main__':
    print("--- 运行 Python efw_pack 工具测试 ---")
    test_overwrite_write_failure_preserves_output()
    version = subprocess.run([sys.executable, PACK_TOOL, '--version'], capture_output=True, text=True)
    assert version.returncode == 0 and 'development' in version.stdout and 'unavailable' in version.stdout
    help_result = subprocess.run([sys.executable, PACK_TOOL, '--help'], capture_output=True, text=True)
    assert help_result.returncode == 0 and 'info' in help_result.stdout
    test_positive_cross_check()
    test_filename_generator()
    test_empty_payload_rejected_without_output()
    test_foe_packet_size_padding()
    test_signed_package_round_trip()
    print("--- Python efw_pack 工具测试全部通过 ---")
