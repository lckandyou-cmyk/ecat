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
4. 已有非空 build 目录保护测试：验证 build/user.txt 在 make build 与 make clean 后均完好保留，
   证明 clean 绝不递归删除未证明归属的目录或用户文件。
5. 已有同名输出防覆盖测试：验证 make vectors 与 make build 在遇未登记的已有文件时主动报错拒绝，
   仅在显式传入 OVERWRITE=1 时允许更新普通文件。
6. 内核级原子符号链接防御测试：验证即使传入 --overwrite，O_NOFOLLOW 依然在内核级原子拒绝符号链接穿透，
   外部目标文件不受篡改。
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


def test_existing_non_empty_build(repo_dir: str):
    """测试 6: 验证已有非空 build 目录及其内部用户文件在 make build 与 make clean 后均完好保留"""
    print("\n[TEST 6] 验证已有非空 build 目录及内部用户文件保护...")
    build_dir = os.path.join(repo_dir, "build")
    os.makedirs(build_dir, exist_ok=True)
    user_file = os.path.join(build_dir, "user.txt")
    user_content = b"USER_IMPORTANT_DATA_MUST_NEVER_BE_DELETED"
    with open(user_file, "wb") as f:
        f.write(user_content)

    # 1. 执行 make build
    cmd_build = ["make", "build", "CC=clang"]
    res_b = subprocess.run(cmd_build, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b.returncode == 0, f"make build 失败 (rc={res_b.returncode}):\n{res_b.stderr}"
    assert os.path.isfile(user_file), "错误: make build 破坏了 user.txt"
    with open(user_file, "rb") as f:
        assert f.read() == user_content, "错误: user.txt 内容在 make build 后被篡改"

    # 2. 执行 make clean
    cmd_clean = ["make", "clean"]
    res_c = subprocess.run(cmd_clean, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c.returncode == 0, f"make clean 失败 (rc={res_c.returncode}):\n{res_c.stderr}"

    # 核心断言: user.txt 必须依然存在，且 build 目录不得被递归删除
    assert os.path.isdir(build_dir), "错误: 非空 build 目录被 make clean 误删"
    assert os.path.isfile(user_file), "错误: build/user.txt 被 make clean 误删"
    with open(user_file, "rb") as f:
        assert f.read() == user_content, "错误: build/user.txt 内容在 make clean 后被篡改"

    # 清理测试用的 user.txt 并移除 build 目录
    os.remove(user_file)
    os.rmdir(build_dir)
    print("  [PASS] 已有非空 build 目录保护验证通过：build/user.txt 完好保留，未发生递归误删")


def test_existing_identical_output_defense(repo_dir: str):
    """测试 7: 验证已存在同名输出文件时主动拒绝静默覆盖"""
    print("\n[TEST 7] 验证已有同名输出文件防覆写保护...")

    # 1. 验证 make vectors 遇存量向量文件时拒绝覆盖
    vec_dir = os.path.join(repo_dir, "tests", "vectors")
    os.makedirs(vec_dir, exist_ok=True)
    sentinel_vec = os.path.join(vec_dir, "positive_mcu.efw")
    preset_vec_bytes = b"PRESET_USER_VECTOR_BYTES_DO_NOT_TOUCH"
    with open(sentinel_vec, "wb") as f:
        f.write(preset_vec_bytes)

    cmd_v_fail = ["make", "vectors"]
    res_v1 = subprocess.run(cmd_v_fail, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_v1.returncode != 0, "错误: make vectors 在文件已存在时未报错拒绝！"
    with open(sentinel_vec, "rb") as f:
        assert f.read() == preset_vec_bytes, "错误: positive_mcu.efw 被静默覆盖！"
    print("  [PASS] make vectors 遇同名文件主动安全拒绝，未发生覆盖")

    # 2. 验证显式 OVERWRITE=1 时允许更新普通文件
    cmd_v_ok = ["make", "vectors", "OVERWRITE=1"]
    res_v2 = subprocess.run(cmd_v_ok, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_v2.returncode == 0, f"make vectors OVERWRITE=1 失败: {res_v2.stderr}"
    with open(sentinel_vec, "rb") as f:
        assert f.read() != preset_vec_bytes, "OVERWRITE=1 应已更新文件"
    print("  [PASS] make vectors OVERWRITE=1 显式授权下成功更新向量")

    # 3. 验证 make build 遇存量未登记文件时拒绝覆盖
    build_dir = os.path.join(repo_dir, "build")
    os.makedirs(build_dir, exist_ok=True)
    sentinel_bin = os.path.join(build_dir, "test_runner_efw")
    preset_bin_bytes = b"PRESET_USER_BINARY_BYTES_DO_NOT_TOUCH"
    with open(sentinel_bin, "wb") as f:
        f.write(preset_bin_bytes)

    # 确保无 manifest
    manifest_file = os.path.join(build_dir, ".foe_manifest.json")
    if os.path.exists(manifest_file):
        os.remove(manifest_file)

    cmd_b_fail = ["make", "build", "CC=clang"]
    res_b1 = subprocess.run(cmd_b_fail, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b1.returncode != 0, "错误: make build 在目标未登记时未报错拒绝！"
    with open(sentinel_bin, "rb") as f:
        assert f.read() == preset_bin_bytes, "错误: test_runner_efw 被静默覆盖！"
    print("  [PASS] make build 遇未登记同名文件主动安全拒绝，未发生覆盖")

    # 4. 验证显式 OVERWRITE=1 时允许更新普通文件并清理
    cmd_b_ok = ["make", "build", "CC=clang", "OVERWRITE=1"]
    res_b2 = subprocess.run(cmd_b_ok, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b2.returncode == 0, f"make build OVERWRITE=1 失败: {res_b2.stderr}"
    subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    print("  [PASS] make build OVERWRITE=1 显式授权下成功生成并安全清理")


def test_atomic_symlink_defense(repo_dir: str):
    """测试 8: 验证内核级原子符号链接防御 (O_NOFOLLOW)，即使传入 --overwrite 亦绝不穿透"""
    print("\n[TEST 8] 验证内核级原子符号链接防御 (即使指定 overwrite 亦安全拦截)...")
    with tempfile.TemporaryDirectory(prefix="foe_symlink_ext_") as ext_tmp:
        target_file = os.path.join(ext_tmp, "sensitive_external.bin")
        original_secret = b"CRITICAL_SENSITIVE_SECRET_DATA"
        with open(target_file, "wb") as f:
            f.write(original_secret)

        symlink_path = os.path.join(repo_dir, "tests", "vectors", "symlink_test.efw")
        if os.path.exists(symlink_path) or os.path.islink(symlink_path):
            os.remove(symlink_path)
        os.symlink(target_file, symlink_path)

        pack_tool = os.path.join(repo_dir, "tools", "efw_pack.py")
        raw_bin = os.path.join(ext_tmp, "raw.bin")
        with open(raw_bin, "wb") as f:
            f.write(b"\x00" * 16)

        # 即使显式传入 --overwrite，O_NOFOLLOW 依然在 open 调用时由内核抛出 ELOOP
        cmd = [
            sys.executable, pack_tool, "pack",
            "--bin", raw_bin,
            "--project-id", "0x10010001",
            "--project-name", "SERVO_DRV",
            "--hardware-id", "0x00000001",
            "--target", "MCU",
            "--fw-version", "1.0.0.0",
            "--out", symlink_path,
            "--overwrite"
        ]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert res.returncode != 0, "错误: efw_pack.py 在 --overwrite 模式下未能拒绝符号链接！"
        assert "符号链接" in res.stderr or "符号链接" in res.stdout, "未输出符号链接拦截信息"

        # 核心断言: 外部敏感文件内容完全不受影响
        with open(target_file, "rb") as f:
            assert f.read() == original_secret, "错误: 外部敏感文件在符号链接写入下被篡改！"

        os.remove(symlink_path)
    print("  [PASS] 内核级原子符号链接防御验证通过：O_NOFOLLOW 有效阻断符号链接穿透")


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

        print("\n[TEST 3] 运行隔离仓库副本下的 make test-asan (全套用例验证)...")
        run_make_target(tmp_repo, "test-asan")
        verify_sentinels(tmp_repo, expected_hashes, "make test-asan 执行后")

        print("\n[TEST 4] 运行隔离仓库副本下的 make clean...")
        run_make_target(tmp_repo, "clean")
        verify_sentinels(tmp_repo, expected_hashes, "make clean 执行后")

        print("\n[TEST 5] 运行隔离仓库副本下的并发构建调用...")
        test_concurrent_make(tmp_repo, "test-examples", expected_hashes, "CONCURRENT 1")
        test_concurrent_make(tmp_repo, "test", expected_hashes, "CONCURRENT 2")

        test_existing_non_empty_build(tmp_repo)
        test_existing_identical_output_defense(tmp_repo)
        test_atomic_symlink_defense(tmp_repo)

    print("\n>>> 隔离仓库副本哨兵完整性、非空目录保护、同名防覆盖与全套测试验证全部通过！<<<")


if __name__ == "__main__":
    main()
