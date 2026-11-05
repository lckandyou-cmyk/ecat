#!/usr/bin/env python3
"""
tests/test_repo_sentinels.py:
在独立隔离仓库副本中验证入口安全：
1. 预置同名哨兵文件保护：在 examples/host_demo 与 examples/driver_template 下预置
   raw_payload.bin、packed_padded.efw、run_output.tmp、host_demo、
   host_flash_data.bin、host_flash_meta.bin、driver_template.o 7 大同名哨兵。
2. 分别运行 make test-examples 与 make test，验证上述哨兵字节完整不变。
3. 测试并发调用：同时并发运行两个 make test-examples 实例，验证彼此构建独立隔离且哨兵不受影响。
"""

import os
import sys
import shutil
import tempfile
import subprocess
import hashlib
import concurrent.futures

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SENTINELS_CONFIG = {
    os.path.join("examples", "host_demo", "raw_payload.bin"): b"SENTINEL_RAW_PAYLOAD_ORIGINAL_BYTES_12345",
    os.path.join("examples", "host_demo", "packed_padded.efw"): b"SENTINEL_PACKED_EFW_ORIGINAL_BYTES_67890",
    os.path.join("examples", "host_demo", "run_output.tmp"): b"SENTINEL_RUN_OUTPUT_ORIGINAL_BYTES_ABCDE",
    os.path.join("examples", "host_demo", "host_demo"): b"SENTINEL_HOST_DEMO_BINARY_STUB_BYTES_FGHIJ",
    os.path.join("examples", "host_demo", "host_flash_data.bin"): b"SENTINEL_FLASH_DATA_ORIGINAL_BYTES_KLMNO",
    os.path.join("examples", "host_demo", "host_flash_meta.bin"): b"SENTINEL_FLASH_META_ORIGINAL_BYTES_PQRST",
    os.path.join("examples", "driver_template", "driver_template.o"): b"SENTINEL_DRIVER_TEMPLATE_OBJ_BYTES_UVWXY",
}


def copy_repo_to_isolated_dir(target_dir: str):
    """仅复制源码树关键目录与文件到独立隔离测试副本，不触碰原工作区"""
    items_to_copy = ["src", "include", "tests", "tools", "examples", "docs", "Makefile"]
    for item in items_to_copy:
        src_path = os.path.join(ROOT_DIR, item)
        dst_path = os.path.join(target_dir, item)
        if os.path.isdir(src_path):
            shutil.copytree(src_path, dst_path, ignore=shutil.ignore_patterns("*.o", "*.dSYM", "*.tmp", "__pycache__"))
        elif os.path.isfile(src_path):
            shutil.copy2(src_path, dst_path)


def plant_sentinels(target_repo_dir: str):
    """预置同名哨兵文件并记录期望哈希"""
    expected_hashes = {}
    for rel_path, content in SENTINELS_CONFIG.items():
        full_path = os.path.join(target_repo_dir, rel_path)
        os.makedirs(os.path.dirname(full_path), exist_ok=True)
        with open(full_path, "wb") as f:
            f.write(content)
        expected_hashes[rel_path] = (hashlib.sha256(content).hexdigest(), len(content), content)
    return expected_hashes


def verify_sentinels(target_repo_dir: str, expected_hashes: dict, stage_name: str):
    """核查所有同名哨兵文件的存在性与字节完整性"""
    for rel_path, (exp_hash, exp_len, exp_content) in expected_hashes.items():
        full_path = os.path.join(target_repo_dir, rel_path)
        assert os.path.isfile(full_path), f"[{stage_name}] 错误: 哨兵文件不存在或被误删: {rel_path}"

        with open(full_path, "rb") as f:
            actual_content = f.read()

        assert len(actual_content) == exp_len, f"[{stage_name}] 错误: 哨兵文件长度改变: {rel_path}"
        actual_hash = hashlib.sha256(actual_content).hexdigest()
        assert actual_hash == exp_hash, f"[{stage_name}] 错误: 哨兵文件内容被覆盖篡改: {rel_path}"
        assert actual_content == exp_content, f"[{stage_name}] 错误: 哨兵文件字节不符: {rel_path}"

    print(f"  [PASS] [{stage_name}] 所有 7 个预存同名哨兵文件均完好无损，预存字节完整不变")


def run_make_target(repo_dir: str, target: str):
    """在隔离副本目录运行 make 目标"""
    cmd = ["make", target, "CC=clang"]
    res = subprocess.run(cmd, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res.returncode == 0, f"make {target} 失败 (rc={res.returncode}):\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}"


def test_concurrent_make_examples(repo_dir: str, expected_hashes: dict):
    """验证并发调用 make test-examples 互不冲突且哨兵完整"""
    print("[TEST 3] 验证并发调用 make test-examples 的隔离性...")

    def make_task():
        cmd = ["make", "test-examples", "CC=clang"]
        res = subprocess.run(cmd, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return res.returncode, res.stdout, res.stderr

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        fut1 = executor.submit(make_task)
        fut2 = executor.submit(make_task)

        rc1, out1, err1 = fut1.result()
        rc2, out2, err2 = fut2.result()

        assert rc1 == 0, f"并发任务 1 执行失败 (rc={rc1}):\n{err1}"
        assert rc2 == 0, f"并发任务 2 执行失败 (rc={rc2}):\n{err2}"

    verify_sentinels(repo_dir, expected_hashes, "并发 make test-examples 执行后")
    print("  [PASS] 两个并发实例同时构建运行成功，独立临时目录互不冲突，哨兵完整无损")


def main():
    with tempfile.TemporaryDirectory(prefix="foe_isolated_repo_") as tmp_repo:
        print(f"=== 搭建独立隔离仓库副本环境: {tmp_repo} ===")
        copy_repo_to_isolated_dir(tmp_repo)

        print("=== 预置 7 大同名哨兵文件 ===")
        expected_hashes = plant_sentinels(tmp_repo)
        verify_sentinels(tmp_repo, expected_hashes, "预置初始化校验")

        print("\n[TEST 1] 运行隔离仓库副本下的 make test-examples...")
        run_make_target(tmp_repo, "test-examples")
        verify_sentinels(tmp_repo, expected_hashes, "make test-examples 执行后")

        print("\n[TEST 2] 运行隔离仓库副本下的 make test...")
        run_make_target(tmp_repo, "test")
        verify_sentinels(tmp_repo, expected_hashes, "make test 执行后")

        print("\n[TEST 3] 运行隔离仓库副本下的并发构建调用...")
        test_concurrent_make_examples(tmp_repo, expected_hashes)

    print("\n>>> 隔离仓库副本哨兵完整性与并发调用验证全部通过！<<<")


if __name__ == "__main__":
    main()
