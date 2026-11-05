#!/usr/bin/env python3
"""
tests/test_host_demo_isolation.py:
外部隔离测试：验证 host_demo 在隔离工作目录下的无副作用特性与并发隔离性

测试目标：
1. 预存同名哨兵保护：在隔离工作目录预先放置 host_flash_data.bin 与 host_flash_meta.bin，
   验证 host_demo 执行后预存字节完整不变，示例未在 CWD 创建、截断或删除同名文件。
2. 并发运行彼此隔离：同时启动两个并发执行的 host_demo 实例，验证两者各自的独立临时目录
   与 Flash 模拟介质互不干扰，且各工作目录下的预存哨兵文件完好无损。
"""

import os
import sys
import tempfile
import subprocess
import hashlib
import concurrent.futures

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK_TOOL = os.path.join(ROOT_DIR, 'tools', 'efw_pack.py')
EXPECTED_OUTPUT_FILE = os.path.join(ROOT_DIR, 'examples', 'host_demo', 'expected_output.txt')


def build_test_padded_efw(out_efw_path: str):
    """使用真实 efw_pack.py 生成测试用补零固件 (.efw)"""
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f_bin:
        f_bin.write(bytes([0x55] * 64))
        raw_bin_path = f_bin.name

    try:
        cmd = [
            sys.executable, PACK_TOOL, 'pack',
            '--bin', raw_bin_path,
            '--project-id', '0x10010001',
            '--project-name', 'SERVO_DRV',
            '--hardware-id', '0x00000001',
            '--hw-ver-min', '1',
            '--hw-ver-max', '1',
            '--target', 'MCU',
            '--fw-version', '1.0.0.0',
            '--foe-packet-size', '64',
            '--out', out_efw_path
        ]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert res.returncode == 0, f"打包固件失败: {res.stderr}"
    finally:
        if os.path.exists(raw_bin_path):
            os.remove(raw_bin_path)


def test_sentinel_preservation_in_isolated_cwd(host_demo_bin: str, test_efw_path: str):
    """测试 1: 验证隔离工作目录下的同名哨兵文件字节完整不变"""
    print("[TEST 1] 运行隔离工作目录下的同名哨兵保护验证...")

    with tempfile.TemporaryDirectory(prefix="foe_test_cwd_") as tmp_cwd:
        sentinel_data_path = os.path.join(tmp_cwd, 'host_flash_data.bin')
        sentinel_meta_path = os.path.join(tmp_cwd, 'host_flash_meta.bin')

        # 写入预存用户数据
        preset_data_bytes = b"PRESET_USER_FLASH_DATA_DO_NOT_TOUCH_1234567890" * 8
        preset_meta_bytes = b"\xAA\xBB\xCC\xDD\xEE\xFF\x00\x11\x22\x33"

        with open(sentinel_data_path, 'wb') as f:
            f.write(preset_data_bytes)
        with open(sentinel_meta_path, 'wb') as f:
            f.write(preset_meta_bytes)

        data_hash_before = hashlib.sha256(preset_data_bytes).hexdigest()
        meta_hash_before = hashlib.sha256(preset_meta_bytes).hexdigest()

        # 在隔离工作目录内执行 host_demo
        res = subprocess.run([host_demo_bin, test_efw_path], cwd=tmp_cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert res.returncode == 0, f"host_demo 执行失败 (rc={res.returncode}):\n{res.stderr}"

        # 检查输出文本与预期完全一致
        with open(EXPECTED_OUTPUT_FILE, 'r', encoding='utf-8') as f:
            expected_text = f.read()
        assert res.stdout == expected_text, "host_demo 运行输出与 expected_output.txt 不吻合"

        # 核心断言: 检查哨兵文件是否完好无损
        assert os.path.isfile(sentinel_data_path), "错误: host_flash_data.bin 哨兵文件被误删"
        assert os.path.isfile(sentinel_meta_path), "错误: host_flash_meta.bin 哨兵文件被误删"

        with open(sentinel_data_path, 'rb') as f:
            actual_data_bytes = f.read()
        with open(sentinel_meta_path, 'rb') as f:
            actual_meta_bytes = f.read()

        assert hashlib.sha256(actual_data_bytes).hexdigest() == data_hash_before, "错误: host_flash_data.bin 预存字节被篡改或截断"
        assert hashlib.sha256(actual_meta_bytes).hexdigest() == meta_hash_before, "错误: host_flash_meta.bin 预存字节被篡改或截断"
        assert actual_data_bytes == preset_data_bytes, "数据内容字节不符"
        assert actual_meta_bytes == preset_meta_bytes, "元数据内容字节不符"

    print("  [PASS] 隔离工作目录下的同名哨兵文件完好无损，预存字节无任何改动")


def run_single_instance(host_demo_bin: str, test_efw_path: str, cwd_path: str, preset_data: bytes, preset_meta: bytes):
    """单个实例的执行与哨兵比对逻辑"""
    sentinel_data_path = os.path.join(cwd_path, 'host_flash_data.bin')
    sentinel_meta_path = os.path.join(cwd_path, 'host_flash_meta.bin')

    with open(sentinel_data_path, 'wb') as f:
        f.write(preset_data)
    with open(sentinel_meta_path, 'wb') as f:
        f.write(preset_meta)

    res = subprocess.run([host_demo_bin, test_efw_path], cwd=cwd_path, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        return False, f"执行失败 rc={res.returncode}"

    with open(sentinel_data_path, 'rb') as f:
        cur_data = f.read()
    with open(sentinel_meta_path, 'rb') as f:
        cur_meta = f.read()

    if cur_data != preset_data:
        return False, "哨兵数据文件字节被篡改"
    if cur_meta != preset_meta:
        return False, "哨兵元数据文件字节被篡改"

    return True, "OK"


def test_concurrent_isolation(host_demo_bin: str, test_efw_path: str):
    """测试 2: 验证两个并发运行的实例彼此隔离"""
    print("[TEST 2] 运行两个并发 host_demo 实例彼此隔离验证...")

    with tempfile.TemporaryDirectory(prefix="foe_concur1_") as cwd1, \
         tempfile.TemporaryDirectory(prefix="foe_concur2_") as cwd2:

        data1 = b"CONCURRENT_INSTANCE_1_DATA_PAYLOAD_STRING" * 6
        meta1 = b"\x11\x22\x33\x44\x55"

        data2 = b"CONCURRENT_INSTANCE_2_DATA_PAYLOAD_STRING" * 6
        meta2 = b"\x66\x77\x88\x99\xAA"

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            fut1 = executor.submit(run_single_instance, host_demo_bin, test_efw_path, cwd1, data1, meta1)
            fut2 = executor.submit(run_single_instance, host_demo_bin, test_efw_path, cwd2, data2, meta2)

            ok1, msg1 = fut1.result()
            ok2, msg2 = fut2.result()

            assert ok1, f"并发实例 1 失败: {msg1}"
            assert ok2, f"并发实例 2 失败: {msg2}"

    print("  [PASS] 两个并发实例均正常运行且彼此独立隔离，未发生任何相互干扰或数据污染")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <path_to_host_demo_binary> [path_to_test_efw]")
        sys.exit(1)

    host_demo_bin = os.path.abspath(sys.argv[1])
    assert os.path.isfile(host_demo_bin), f"二进制不存在: {host_demo_bin}"

    with tempfile.TemporaryDirectory(prefix="foe_efw_test_") as efw_dir:
        test_efw_path = os.path.join(efw_dir, 'packed_padded.efw')
        if len(sys.argv) >= 3 and os.path.isfile(sys.argv[2]):
            test_efw_path = os.path.abspath(sys.argv[2])
        else:
            build_test_padded_efw(test_efw_path)

        test_sentinel_preservation_in_isolated_cwd(host_demo_bin, test_efw_path)
        test_concurrent_isolation(host_demo_bin, test_efw_path)

    print("\n>>> host_demo 外部隔离与并发测试全部通过！<<<")


if __name__ == '__main__':
    main()
