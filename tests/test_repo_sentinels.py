#!/usr/bin/env python3
"""
tests/test_repo_sentinels.py:
在独立隔离仓库副本中验证全范围入口安全与哨兵保护：
1. 预置 17 大同名哨兵文件/目录保护：
   - examples/host_demo/ 下的 6 个示例生成物与存储介质哨兵
   - examples/driver_template/driver_template.o 目标文件哨兵
   - 仓库根目录与 tests/ 下的 user_firmware.efw 用户固件哨兵
   - tests/ 下的 test_runner_efw、test_runner_core、test_runner_soes_real、test_runner_custom
   - tests/vectors/ 下的 positive_mcu.efw 与 user_vector.efw 向量哨兵
   - 根目录与 tests/ 下的 *.dSYM 调试符号包哨兵
2. 依次运行 make test-examples、make test、make test-asan、make clean，
   核查上述所有哨兵的存在性与原始字节完整性，确认无覆盖、无误删。
3. 测试并发调用：分别并发运行两个 make test-examples 与两个 make test 实例，
   验证彼此构建独立隔离且哨兵不受影响。
4. 符号链接安全防御检验：验证打包工具与向量生成工具对符号链接写入的主动安全拦截。
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
    # 示例目录哨兵
    os.path.join("examples", "host_demo", "raw_payload.bin"): b"SENTINEL_RAW_PAYLOAD_ORIGINAL_BYTES_12345",
    os.path.join("examples", "host_demo", "packed_padded.efw"): b"SENTINEL_PACKED_EFW_ORIGINAL_BYTES_67890",
    os.path.join("examples", "host_demo", "run_output.tmp"): b"SENTINEL_RUN_OUTPUT_ORIGINAL_BYTES_ABCDE",
    os.path.join("examples", "host_demo", "host_demo"): b"SENTINEL_HOST_DEMO_BINARY_STUB_BYTES_FGHIJ",
    os.path.join("examples", "host_demo", "host_flash_data.bin"): b"SENTINEL_FLASH_DATA_ORIGINAL_BYTES_KLMNO",
    os.path.join("examples", "host_demo", "host_flash_meta.bin"): b"SENTINEL_FLASH_META_ORIGINAL_BYTES_PQRST",
    os.path.join("examples", "driver_template", "driver_template.o"): b"SENTINEL_DRIVER_TEMPLATE_OBJ_BYTES_UVWXY",

    # 用户固件包哨兵 (防止 clean 中的 *.efw 或 tests/*.efw 误删)
    "user_firmware.efw": b"SENTINEL_ROOT_USER_FIRMWARE_PACKAGE_BYTES_010203",
    os.path.join("tests", "user_firmware.efw"): b"SENTINEL_TESTS_USER_FIRMWARE_PACKAGE_BYTES_040506",

    # 测试二进制同名与通配符哨兵 (防止 clean 中的 tests/test_runner* 误删或 test 覆盖)
    os.path.join("tests", "test_runner_efw"): b"SENTINEL_TEST_RUNNER_EFW_PRESET_USER_STUB_A",
    os.path.join("tests", "test_runner_core"): b"SENTINEL_TEST_RUNNER_CORE_PRESET_USER_STUB_B",
    os.path.join("tests", "test_runner_soes_real"): b"SENTINEL_TEST_RUNNER_SOES_PRESET_USER_STUB_C",
    os.path.join("tests", "test_runner_custom"): b"SENTINEL_TEST_RUNNER_CUSTOM_USER_SCRIPT_D",

    # 向量目录哨兵 (防止向量生成覆盖或目录清理)
    os.path.join("tests", "vectors", "positive_mcu.efw"): b"SENTINEL_VECTOR_POSITIVE_MCU_USER_PRESET",
    os.path.join("tests", "vectors", "user_vector.efw"): b"SENTINEL_USER_CUSTOM_VECTOR_PRESET",

    # 调试符号包哨兵 (防止 *.dSYM 误删)
    os.path.join("user_app.dSYM", "Contents", "Info.plist"): b"<plist><string>SENTINEL_ROOT_DSYM</string></plist>",
    os.path.join("tests", "test_debug.dSYM", "Contents", "Info.plist"): b"<plist><string>SENTINEL_TESTS_DSYM</string></plist>",
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

    print(f"  [PASS] [{stage_name}] 全部 {len(expected_hashes)} 个预存同名哨兵文件均完好无损，预存字节无变动")


def run_make_target(repo_dir: str, target: str):
    """在隔离副本目录运行 make 目标"""
    cmd = ["make", target, "CC=clang"]
    res = subprocess.run(cmd, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res.returncode == 0, f"make {target} 失败 (rc={res.returncode}):\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}"


def test_concurrent_make(repo_dir: str, target: str, expected_hashes: dict, label: str):
    """验证并发调用 make 目标互不冲突且哨兵完整"""
    print(f"[{label}] 验证并发调用 make {target} 的独立隔离性...")

    def make_task():
        cmd = ["make", target, "CC=clang"]
        res = subprocess.run(cmd, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return res.returncode, res.stdout, res.stderr

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        fut1 = executor.submit(make_task)
        fut2 = executor.submit(make_task)

        rc1, out1, err1 = fut1.result()
        rc2, out2, err2 = fut2.result()

        assert rc1 == 0, f"并发任务 1 执行失败 (rc={rc1}):\n{err1}"
        assert rc2 == 0, f"并发任务 2 执行失败 (rc={rc2}):\n{err2}"

    verify_sentinels(repo_dir, expected_hashes, f"并发 make {target} 执行后")
    print(f"  [PASS] 两个并发 make {target} 实例同时运行成功，独立临时目录无干扰，哨兵完整无损")


def test_symlink_defense(repo_dir: str):
    """测试 6: 验证打包工具与向量生成工具对符号链接写入的主动安全拒绝"""
    print("\n[TEST 6] 验证符号链接安全防御...")
    with tempfile.TemporaryDirectory(prefix="foe_symlink_ext_") as ext_tmp:
        target_file = os.path.join(ext_tmp, "sensitive_external.bin")
        with open(target_file, "wb") as f:
            f.write(b"ORIGINAL_SENSITIVE_CONTENT")

        symlink_path = os.path.join(repo_dir, "tests", "vectors", "symlink_test.efw")
        if os.path.exists(symlink_path) or os.path.islink(symlink_path):
            os.remove(symlink_path)
        os.symlink(target_file, symlink_path)

        # 尝试通过 efw_pack.py 打包写入该符号链接
        pack_tool = os.path.join(repo_dir, "tools", "efw_pack.py")
        raw_bin = os.path.join(ext_tmp, "raw.bin")
        with open(raw_bin, "wb") as f:
            f.write(b"\x00" * 16)

        cmd = [
            sys.executable, pack_tool, "pack",
            "--bin", raw_bin,
            "--project-id", "0x10010001",
            "--project-name", "SERVO_DRV",
            "--hardware-id", "0x00000001",
            "--target", "MCU",
            "--fw-version", "1.0.0.0",
            "--out", symlink_path
        ]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert res.returncode != 0, "错误: efw_pack.py 未能拒绝向符号链接写入！"
        assert "符号链接" in res.stderr or "符号链接" in res.stdout, "未输出符号链接拦截信息"

        # 验证外部被链接的目标文件未被覆盖
        with open(target_file, "rb") as f:
            assert f.read() == b"ORIGINAL_SENSITIVE_CONTENT", "错误: 外部敏感文件内容被符号链接写入篡改！"

        os.remove(symlink_path)
    print("  [PASS] 符号链接安全防御验证通过：主动拒绝向符号链接写入并保护目标内容")


def main():
    with tempfile.TemporaryDirectory(prefix="foe_isolated_repo_") as tmp_repo:
        print(f"=== 搭建独立隔离仓库副本环境: {tmp_repo} ===")
        copy_repo_to_isolated_dir(tmp_repo)

        print(f"=== 预置 {len(SENTINELS_CONFIG)} 大同名哨兵文件/目录 ===")
        expected_hashes = plant_sentinels(tmp_repo)
        verify_sentinels(tmp_repo, expected_hashes, "预置初始化校验")

        print("\n[TEST 1] 运行隔离仓库副本下的 make test-examples...")
        run_make_target(tmp_repo, "test-examples")
        verify_sentinels(tmp_repo, expected_hashes, "make test-examples 执行后")

        print("\n[TEST 2] 运行隔离仓库副本下的 make test...")
        run_make_target(tmp_repo, "test")
        verify_sentinels(tmp_repo, expected_hashes, "make test 执行后")

        print("\n[TEST 3] 运行隔离仓库副本下的 make test-asan...")
        run_make_target(tmp_repo, "test-asan")
        verify_sentinels(tmp_repo, expected_hashes, "make test-asan 执行后")

        print("\n[TEST 4] 运行隔离仓库副本下的 make clean...")
        run_make_target(tmp_repo, "clean")
        verify_sentinels(tmp_repo, expected_hashes, "make clean 执行后")

        print("\n[TEST 5] 运行隔离仓库副本下的并发构建调用...")
        test_concurrent_make(tmp_repo, "test-examples", expected_hashes, "CONCURRENT 1")
        test_concurrent_make(tmp_repo, "test", expected_hashes, "CONCURRENT 2")

        test_symlink_defense(tmp_repo)

    print("\n>>> 隔离仓库副本哨兵完整性、全套构建目标与并发调用验证全部通过！<<<")


if __name__ == "__main__":
    main()
