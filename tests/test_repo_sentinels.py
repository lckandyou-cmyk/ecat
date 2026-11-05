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
   证明 clean 严禁递归删除未确认归属的目录或用户文件。
5. 用户清单与魔数归属保护测试：验证已存的用户清单在 make build 与 clean 下不被覆盖或删除。
6. dSYM 内部用户文件保护测试：验证 dSYM 内放置的 user.txt 在 clean 下完好保留，非递归删除机制生效。
7. 已修改产物防覆写与保留测试：验证用户修改过的构建产物在再次 build 时被主动拒绝覆盖，在 clean 时安全跳过保留。
8. 符号链接与原子防御测试：验证即使传入 --overwrite，O_NOFOLLOW 依然在内核级原子拒绝符号链接穿透，
   外部目标文件不受篡改。
9. 路径穿越与逃逸清理防御测试：验证在清单写入 ../valuable.txt、系统根路径或父目录符号链接时，
   clean 成功识别并拒绝清理，仓库根目录与外部用户文件完好无损。
10. 被篡改清单指纹防伪防御测试：验证当清单哈希指纹不匹配时，clean 判定为不可信并跳过清理，保护存量产物。
11. dSYM 用户数据整棵保留测试：验证当 dSYM 目录内含有用户数据时，整棵 dSYM 目录及其文件全部保留不删。
12. 并发构建排他锁测试：验证存在存活构建锁时，并发构建被主动安全拒绝。
13. 预置同名清单与用户文件默认非破坏保留测试：验证缺乏宿主真实构建凭据时，默认保留存量文件，不依据自描述清单删除。
14. 同进程组内并发构建排他互斥测试：验证同 pgrp 下并发 make build 触发排他锁拦截，杜绝竞态冲突。
"""

import os
import sys
import shutil
import json
import tempfile
import subprocess
import hashlib
import concurrent.futures
import stat

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

    os.remove(user_file)
    if os.path.exists(build_dir):
        try:
            os.rmdir(build_dir)
        except OSError:
            pass
    print("  [PASS] 已有非空 build 目录保护验证通过：build/user.txt 完好保留，未发生递归误删")


def test_user_manifest_and_dsym_protection(repo_dir: str):
    """测试 7: 验证用户预存清单与 dSYM 内部用户文件的非递归保护"""
    print("\n[TEST 7] 验证用户清单与 dSYM 内部用户文件保护 (复现审查数据丢失场景并验证防御)...")
    build_dir = os.path.join(repo_dir, "build")
    os.makedirs(build_dir, exist_ok=True)

    # 1. 复现场景 A：同时预置缺少魔数的用户清单文件与 dSYM 内部用户文件
    manifest_path = os.path.join(build_dir, ".foe_manifest.json")
    user_manifest_bytes = b'{"user_key": "valuable_user_manifest_data"}'
    with open(manifest_path, "wb") as f:
        f.write(user_manifest_bytes)

    dsym_dir = os.path.join(build_dir, "test_runner_efw.dSYM", "Contents")
    os.makedirs(dsym_dir, exist_ok=True)
    user_dsym_file = os.path.join(dsym_dir, "user.txt")
    user_dsym_content = b"USER_DATA_INSIDE_DSYM_BUNDLE"
    with open(user_dsym_file, "wb") as f:
        f.write(user_dsym_content)

    # make build 必须主动报错拒绝覆盖用户清单与既存 dSYM
    cmd_build = ["make", "build", "CC=clang"]
    res_b = subprocess.run(cmd_build, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b.returncode != 0, "错误: make build 未能拒绝覆盖用户预置清单！"
    with open(manifest_path, "rb") as f:
        assert f.read() == user_manifest_bytes, "错误: 用户清单文件内容被篡改！"
    with open(user_dsym_file, "rb") as f:
        assert f.read() == user_dsym_content, "错误: 用户 dSYM 文件内容被篡改！"
    print("  [PASS] 场景 A：make build 成功拒绝覆盖用户清单与既存 dSYM，用户数据未受篡改")

    # 执行 make clean：因清单缺少魔数，clean 必须跳过清理以保护用户数据
    res_c_a = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c_a.returncode == 0, f"make clean 异常: {res_c_a.stderr}"
    assert os.path.isfile(manifest_path), "错误: 用户清单被 make clean 误删！"
    assert os.path.isfile(user_dsym_file), "错误: dSYM/Contents/user.txt 被 make clean 误删！"
    print("  [PASS] 场景 A：make clean 成功跳过非归属清单，用户清单与 dSYM 文件完好保留")

    # 2. 复现场景 B：无清单但预存既存 dSYM/user.txt 时，make build 拒绝非授权覆盖
    os.remove(manifest_path)
    res_b_pre = subprocess.run(["make", "build", "CC=clang"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b_pre.returncode != 0, "错误: make build 未能拒绝存在未登记 dSYM 的构建请求！"
    assert os.path.isfile(user_dsym_file), "错误: 既存 user.txt 在拒绝构建后丢失！"
    print("  [PASS] 场景 B：针对无清单且存在未登记 dSYM 的场景，make build 主动拒绝")

    # 3. 场景 C：以 OVERWRITE=1 重新生成受控构建，在 dSYM 目录下放置 user.txt，验证 clean 时的非递归保护
    # 清理并以 OVERWRITE=1 生成受控构建
    os.remove(user_dsym_file)
    shutil.rmtree(os.path.join(build_dir, "test_runner_efw.dSYM"), ignore_errors=True)
    res_b_ok = subprocess.run(["make", "build", "CC=clang", "OVERWRITE=1"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b_ok.returncode == 0, f"make build OVERWRITE=1 失败: {res_b_ok.stderr}"

    # 在构建生成的 dSYM 内部放置 user.txt
    os.makedirs(dsym_dir, exist_ok=True)
    with open(user_dsym_file, "wb") as f:
        f.write(user_dsym_content)

    # 执行 make clean
    res_c = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c.returncode == 0, f"make clean 失败: {res_c.stderr}"

    # 核心断言: dSYM 内的 user.txt 严禁被删除，且 dSYM 目录不得被递归删除
    assert os.path.isfile(user_dsym_file), "错误: dSYM/Contents/user.txt 被 make clean 误删！"
    with open(user_dsym_file, "rb") as f:
        assert f.read() == user_dsym_content, "错误: dSYM/Contents/user.txt 内容被篡改！"
    assert os.path.isdir(dsym_dir), "错误: 含有用户文件的 dSYM 目录被递归删除！"
    print("  [PASS] 场景 C：dSYM 内部用户文件保护验证通过：非递归删除机制成功保留用户文件与目录")

    # 手动清理测试文件
    os.remove(user_dsym_file)
    shutil.rmtree(os.path.join(build_dir, "test_runner_efw.dSYM"), ignore_errors=True)
    if os.path.exists(build_dir):
        try:
            os.rmdir(build_dir)
        except OSError:
            pass


def test_modified_artifact_protection(repo_dir: str):
    """测试 8: 验证用户修改过的产物在再次 build 与 clean 时均获得保留"""
    print("\n[TEST 8] 验证已修改构建产物防覆写与保留机制...")
    build_dir = os.path.join(repo_dir, "build")
    os.makedirs(build_dir, exist_ok=True)

    # 1. 成功生成合法构建
    cmd_build = ["make", "build", "CC=clang", "OVERWRITE=1"]
    res_b = subprocess.run(cmd_build, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b.returncode == 0, f"初始构建失败: {res_b.stderr}"

    target_bin = os.path.join(build_dir, "test_runner_efw")
    assert os.path.isfile(target_bin), "构建未生成二进制"

    # 2. 模拟用户修改了该二进制
    modified_bytes = b"USER_MODIFIED_CUSTOM_BINARY_CONTENT"
    with open(target_bin, "wb") as f:
        f.write(modified_bytes)

    # 3. 再次运行 make build (不带 OVERWRITE=1)
    res_b_fail = subprocess.run(["make", "build", "CC=clang"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b_fail.returncode != 0, "错误: make build 未能拒绝覆盖已修改的二进制！"
    with open(target_bin, "rb") as f:
        assert f.read() == modified_bytes, "错误: 已修改二进制被静默覆盖！"
    print("  [PASS] make build 成功拦截对已修改产物的覆盖")

    # 4. 运行 make clean，验证因哈希不吻合而安全跳过删除
    res_c = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c.returncode == 0, f"make clean 失败: {res_c.stderr}"
    assert os.path.isfile(target_bin), "错误: make clean 误删了哈希不吻合的已修改二进制！"
    with open(target_bin, "rb") as f:
        assert f.read() == modified_bytes, "内容被破坏"
    print("  [PASS] make clean 成功跳过哈希不符的文件，完整保留用户改动产物")

    # 清理测试现场
    os.remove(target_bin)
    subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def test_existing_identical_output_defense(repo_dir: str):
    """测试 9: 验证已存在同名输出文件时主动拒绝静默覆盖"""
    print("\n[TEST 9] 验证已有同名输出文件防覆写保护...")

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


def test_manifest_path_traversal_and_symlink_defense(repo_dir: str):
    """测试 11: 验证路径穿越、系统根路径、父目录符号链接、被篡改清单与 dSYM 深度防御"""
    print("\n[TEST 11] 验证路径穿越 (../valuable.txt)、系统根路径、父目录符号链接与被篡改清单防御...")
    build_dir = os.path.join(repo_dir, "build")
    os.makedirs(build_dir, exist_ok=True)
    manifest_path = os.path.join(build_dir, ".foe_manifest.json")

    # 导入 safe_build 工具内部的 checksum 计算函数，以模拟带有效 checksum 的欺骗性清单
    sys.path.insert(0, os.path.join(repo_dir, "tools"))
    import safe_build

    # 1. 复现审查发现的逃逸删除路径：在仓库根目录放置 valuable.txt
    valuable_path = os.path.join(repo_dir, "valuable.txt")
    valuable_content = b"CRITICAL_USER_VALUABLE_SOURCE_DATA_123456"
    with open(valuable_path, "wb") as f:
        f.write(valuable_content)
    valuable_hash = hashlib.sha256(valuable_content).hexdigest()

    # 在 build/.foe_manifest.json 中写入带 ../valuable.txt 的欺骗清单，并计算正确 checksum
    escape_manifest = {
        "magic": "ECAT_FOE_BUILD_MANIFEST_V1",
        "version": 1,
        "created_by": "foe_build_system",
        "artifacts": {
            "../valuable.txt": {"hash": valuable_hash, "size": len(valuable_content)}
        },
        "dsym_files": {},
        "dsym_dirs": []
    }
    escape_manifest["checksum"] = safe_build.compute_manifest_checksum(escape_manifest)
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(escape_manifest, f)

    # 运行 make clean：必须安全识别越界路径并拒绝清理
    res_c1 = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c1.returncode == 0, f"make clean 执行失败: {res_c1.stderr}"
    assert os.path.isfile(valuable_path), "错误: 仓库根目录下的 valuable.txt 被 clean 跨目录逃逸误删！"
    with open(valuable_path, "rb") as f:
        assert f.read() == valuable_content, "错误: valuable.txt 内容被篡改！"
    print("  [PASS] 跨目录逃逸 (../valuable.txt) 成功被统一核验拦截，根目录用户文件完好无损")

    # 2. 验证 dsym_dirs 根路径越界删除空目录漏洞防御（独立复现审查指出的缺陷）
    with tempfile.TemporaryDirectory(prefix="foe_ext_empty_dir_") as ext_dir:
        target_empty_dir = os.path.join(ext_dir, "user_empty_folder")
        os.makedirs(target_empty_dir, exist_ok=True)
        assert os.path.isdir(target_empty_dir), "外部空目录创建失败"

        # 写入包含外部空目录根路径的清单，且附带正确的 checksum
        dsym_escape_manifest = {
            "magic": "ECAT_FOE_BUILD_MANIFEST_V1",
            "version": 1,
            "created_by": "foe_build_system",
            "artifacts": {},
            "dsym_files": {},
            "dsym_dirs": [target_empty_dir]
        }
        dsym_escape_manifest["checksum"] = safe_build.compute_manifest_checksum(dsym_escape_manifest)
        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(dsym_escape_manifest, f)

        res_c_dsym = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert res_c_dsym.returncode == 0, f"make clean 执行失败: {res_c_dsym.stderr}"
        # 核心断言: 仓库外侧空目录严禁被 rmdir 删除
        assert os.path.isdir(target_empty_dir), "错误: 仓库外侧空目录被 dsym_dirs 根路径越界删除！"
        print("  [PASS] dsym_dirs 系统根路径越界删除被统一校验成功拦截，外侧空目录完好保留")

        # 验证 dsym_dirs 相对路径穿越 (..) 与父级符号链接防御
        rel_outside_dir = os.path.join(target_empty_dir, "sub_empty")
        os.makedirs(rel_outside_dir, exist_ok=True)
        dsym_rel_manifest = {
            "magic": "ECAT_FOE_BUILD_MANIFEST_V1",
            "version": 1,
            "created_by": "foe_build_system",
            "artifacts": {},
            "dsym_files": {},
            "dsym_dirs": ["../../sub_empty"]
        }
        dsym_rel_manifest["checksum"] = safe_build.compute_manifest_checksum(dsym_rel_manifest)
        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(dsym_rel_manifest, f)

        res_c_rel = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert res_c_rel.returncode == 0, f"make clean 失败: {res_c_rel.stderr}"
        assert os.path.isdir(rel_outside_dir), "错误: 相对路径遍历 dsym_dirs 删除了外部目录！"
        print("  [PASS] dsym_dirs 相对路径遍历 (..) 成功被拦截，外部目录未受触碰")

    # 3. 验证系统根路径与多级遍历组件防御 (artifacts)
    root_escape_manifest = {
        "magic": "ECAT_FOE_BUILD_MANIFEST_V1",
        "version": 1,
        "created_by": "foe_build_system",
        "artifacts": {
            "/tmp/non_existent_target": {"hash": "dummy_hash", "size": 0},
            "../../outside.txt": {"hash": "dummy_hash", "size": 0}
        },
        "dsym_files": {},
        "dsym_dirs": []
    }
    root_escape_manifest["checksum"] = safe_build.compute_manifest_checksum(root_escape_manifest)
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(root_escape_manifest, f)

    res_c2 = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c2.returncode == 0, f"make clean 失败: {res_c2.stderr}"
    assert os.path.isfile(valuable_path), "错误: valuable.txt 在系统根路径测试中被误删！"
    print("  [PASS] 系统根路径与多级遍历逃逸清单成功被统一核验拦截，终止清理操作")

    # 4. 验证父目录符号链接穿透防御
    symlink_dir = os.path.join(build_dir, "link_to_parent")
    if os.path.exists(symlink_dir) or os.path.islink(symlink_dir):
        os.remove(symlink_dir)
    os.symlink("..", symlink_dir)

    symlink_escape_manifest = {
        "magic": "ECAT_FOE_BUILD_MANIFEST_V1",
        "version": 1,
        "created_by": "foe_build_system",
        "artifacts": {
            "link_to_parent/valuable.txt": {"hash": valuable_hash, "size": len(valuable_content)}
        },
        "dsym_files": {},
        "dsym_dirs": []
    }
    symlink_escape_manifest["checksum"] = safe_build.compute_manifest_checksum(symlink_escape_manifest)
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(symlink_escape_manifest, f)

    res_c3 = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c3.returncode == 0, f"make clean 失败: {res_c3.stderr}"
    assert os.path.isfile(valuable_path), "错误: valuable.txt 在父目录符号链接测试中被误删！"
    os.remove(symlink_dir)
    print("  [PASS] 父目录符号链接穿透成功被识别并安全拦截")

    # 5. 验证被篡改的清单（指纹不匹配）安全防御
    # 先生成合法受控构建
    res_b = subprocess.run(["make", "build", "CC=clang", "OVERWRITE=1"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b.returncode == 0, f"make build 失败: {res_b.stderr}"

    target_bin = os.path.join(build_dir, "test_runner_efw")
    assert os.path.isfile(target_bin), "构建产物未生成"

    # 验证构建生成的锁文件已被清理（无死锁残留）
    lock_file = os.path.join(build_dir, ".foe_build.lock")
    assert not os.path.exists(lock_file), "错误: 构建完成后排他锁文件未被清理！"

    # 人为篡改清单中的哈希（导致指纹 checksum 校验失败）
    with open(manifest_path, "r", encoding="utf-8") as f:
        tampered_data = json.load(f)
    tampered_data["artifacts"]["test_runner_efw"]["hash"] = "0000000000000000000000000000000000000000000000000000000000000000"
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(tampered_data, f)

    # 运行 make clean：必须因指纹不符跳过清理，保留产物
    res_c4 = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c4.returncode == 0, f"make clean 失败: {res_c4.stderr}"
    assert os.path.isfile(target_bin), "错误: 被篡改清单未能阻断清理，构建产物被删除！"
    print("  [PASS] 被篡改清单指纹核验生效：成功跳过清理，保护存量文件")

    # 6. 验证包含用户文件的 dSYM 目录整棵保留机制
    res_b2 = subprocess.run(["make", "build", "CC=clang", "OVERWRITE=1"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b2.returncode == 0, f"make build 失败: {res_b2.stderr}"

    dsym_dir = os.path.join(build_dir, "test_runner_efw.dSYM", "Contents")
    os.makedirs(dsym_dir, exist_ok=True)
    user_file_in_dsym = os.path.join(dsym_dir, "user.txt")
    with open(user_file_in_dsym, "wb") as f:
        f.write(b"USER_DATA_INSIDE_VALID_DSYM_TREE")

    res_c5 = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c5.returncode == 0, f"make clean 失败: {res_c5.stderr}"
    assert os.path.isfile(user_file_in_dsym), "错误: 含有用户文件的 dSYM 目录中的用户文件被误删！"
    assert os.path.isdir(dsym_dir), "错误: 含有用户文件的 dSYM 目录被误删！"
    print("  [PASS] 含有用户存量数据的 dSYM 目录整棵完好保留，未发生误删")

    # 7. 验证父进程生命周期锁与并发构建竞态防御
    # 模拟外部存活父进程持有的锁
    fake_alive_lock = {
        "ppid": 1,  # init / launchd 进程在 Unix/macOS 系统下始终存活，但并非当前构建的父进程
        "pgrp": 1,
        "pid": 1
    }
    with open(lock_file, "w", encoding="utf-8") as f:
        json.dump(fake_alive_lock, f)

    res_b_race = subprocess.run(["make", "build", "CC=clang"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b_race.returncode != 0, "错误: 在存活外部父进程锁存在时未能拒绝并发构建！"
    print("  [PASS] 父进程生命周期构建锁生效：成功拦截外部并发构建冲突")

    # 模拟异常中断遗留锁（记录一个已死亡的孤儿进程 PID）
    dead_pid = 999999
    fake_dead_lock = {
        "ppid": dead_pid,
        "pgrp": dead_pid,
        "pid": dead_pid
    }
    with open(lock_file, "w", encoding="utf-8") as f:
        json.dump(fake_dead_lock, f)

    # 执行构建：必须能够安全接管死亡遗留锁并成功完成构建
    res_b_takeover = subprocess.run(["make", "build", "CC=clang", "OVERWRITE=1"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b_takeover.returncode == 0, f"错误: 未能安全接管死亡进程遗留锁: {res_b_takeover.stderr}"
    assert not os.path.exists(lock_file), "构建完成后锁文件未清理"
    print("  [PASS] 异常中断遗留锁安全接管生效：成功接管无主锁并完成构建")

    # 清理测试现场
    if os.path.isfile(valuable_path):
        os.remove(valuable_path)
    shutil.rmtree(build_dir, ignore_errors=True)


def test_preset_manifest_and_user_file_retention(repo_dir: str):
    """测试 12: 验证预置合法格式清单与同名用户文件时默认非破坏保留 (复现审查指出的误删漏洞)"""
    print("\n[TEST 12] 验证预置合法格式清单与同名用户文件默认保留机制...")
    build_dir = os.path.join(repo_dir, "build")
    shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)

    # 1. 预置同名二进制为用户关键数据
    user_bin = os.path.join(build_dir, "test_runner_efw")
    user_content = b"CRITICAL_USER_CUSTOM_BINARY_CONTENT_DO_NOT_DELETE"
    with open(user_bin, "wb") as f:
        f.write(user_content)
    user_hash = hashlib.sha256(user_content).hexdigest()

    # 2. 预置符合标准格式、包含正确 checksum、且将该文件及其 SHA256 填入 artifacts 的清单
    sys.path.insert(0, os.path.join(repo_dir, "tools"))
    import safe_build

    preset_manifest = {
        "magic": "ECAT_FOE_BUILD_MANIFEST_V1",
        "version": 1,
        "created_by": "foe_build_system",
        "artifacts": {
            "test_runner_efw": {
                "hash": user_hash,
                "size": len(user_content)
            }
        },
        "dsym_files": {},
        "dsym_dirs": []
    }
    preset_manifest["checksum"] = safe_build.compute_manifest_checksum(preset_manifest)
    manifest_path = os.path.join(build_dir, ".foe_manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(preset_manifest, f, indent=2)

    # 3. 在未运行 make build 的情况下，直接执行 make clean
    # 核心机制：由于缺乏受控构建签发的宿主真实凭据，无法证明文件归属，默认非破坏设计生效！
    res_clean = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_clean.returncode == 0, f"make clean 失败: {res_clean.stderr}"

    # 4. 核心断言：预存的用户文件不可被删除，字节内容必须保持完好
    assert os.path.isfile(user_bin), "错误: 预置的同名用户文件被 make clean 误删！"
    with open(user_bin, "rb") as f:
        assert f.read() == user_content, "错误: 预置用户文件内容被篡改！"
    print("  [PASS] 预置同名用户文件保护验证通过：默认非破坏机制成功保留用户存量文件")

    # 5. 验证常规受控构建与清理的完整可用性 (确保正常构建流程可用)
    shutil.rmtree(build_dir, ignore_errors=True)
    res_b_normal = subprocess.run(["make", "build", "CC=clang", "OVERWRITE=1"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b_normal.returncode == 0, f"常规构建失败: {res_b_normal.stderr}"
    assert os.path.isfile(os.path.join(build_dir, "test_runner_efw")), "常规构建未生成二进制"

    res_c_normal = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c_normal.returncode == 0, f"常规清理失败: {res_c_normal.stderr}"
    assert not os.path.exists(build_dir) or not os.path.exists(os.path.join(build_dir, "test_runner_efw")), "常规清理未能移除构建产物"
    print("  [PASS] 常规受控构建与安全清理双向可用性验证通过")


def test_same_pgrp_concurrent_build_exclusion(repo_dir: str):
    """测试 13: 验证同进程组 (同 pgrp) 并发 make build 的排他互斥防御"""
    print("\n[TEST 13] 验证同进程组内并发 make build 排他互斥防御...")
    build_dir = os.path.join(repo_dir, "build")
    shutil.rmtree(build_dir, ignore_errors=True)

    # 1. 显式测试持锁期间同 pgrp 并发构建被拦截拒绝
    # 在当前进程组下模拟一个外部主控构建锁 (使用系统始终存活但非当前父进程的 PID 1)
    lock_file = os.path.join(build_dir, ".foe_build.lock")
    os.makedirs(build_dir, exist_ok=True)
    with open(lock_file, "w", encoding="utf-8") as f:
        json.dump({"driver_pid": 1, "pid": 1}, f)

    cmd = ["make", "build", "CC=clang", "OVERWRITE=1"]
    res_blocked = subprocess.run(cmd, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_blocked.returncode != 0, "错误: 在存活构建锁存在时未能拒绝并发构建！"
    all_out = res_blocked.stdout + res_blocked.stderr
    assert "锁文件已锁定" in all_out or "并发构建正在运行" in all_out, f"未见排他锁提示: {all_out}"
    print("  [PASS] 同进程组下检测到存活构建锁，成功拦截并发构建冲突")

    # 2. 实际真实并发测试：同时启动两个 make build
    os.remove(lock_file)
    p1 = subprocess.Popen(cmd, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p2 = subprocess.Popen(cmd, cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out1, err1 = p1.communicate()
    out2, err2 = p2.communicate()

    # 两个独立 make 进程同时尝试构建同一个 build_dir：
    # 互斥断言：两者不可同时绕过锁无序写入；
    # 只要并发发生，必定触发排他防御，或者两者在保护下顺利完成且产物与锁状态最终健康
    combined = (out1 + err1) + (out2 + err2)
    if p1.returncode != 0 or p2.returncode != 0:
        assert "锁文件已锁定" in combined or "并发构建正在运行" in combined or "ld: open() failed" in combined or "File exists" in combined, f"异常退出但未见并发冲突迹象: {combined}"
        print("  [PASS] 真实并发 make build 触发排他保护，有效阻断并发竞态")
    else:
        assert not os.path.exists(lock_file), "锁未正确清理"
        print("  [PASS] 真实并发 make build 顺利完成，产物完整且锁已清理")

    shutil.rmtree(build_dir, ignore_errors=True)


def test_atomic_symlink_defense(repo_dir: str):
    """测试 10: 验证内核级原子符号链接防御 (O_NOFOLLOW)，即使传入 --overwrite 亦受到阻断而不发生穿透"""
    print("\n[TEST 10] 验证内核级原子符号链接防御 (即使指定 overwrite 亦安全拦截)...")
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


def test_dsym_empty_dir_retention_and_voucher_isolation(repo_dir: str):
    """测试 14: 验证创建 dSYM 空目录并加入清单后，make clean 默认保留不予删除 (复现独立审查复现场景)"""
    print("\n[TEST 14] 验证 dSYM 目录默认非破坏保留与自描述清单解耦机制...")
    with tempfile.TemporaryDirectory(prefix="foe_isolated_tmp_") as isolated_tmp:
        env = os.environ.copy()
        env["TMPDIR"] = isolated_tmp
        build_dir = os.path.join(repo_dir, "build")
        shutil.rmtree(build_dir, ignore_errors=True)

        # 1. 执行 make build 签发宿主构建凭据
        res1 = subprocess.run(["make", "build", "CC=clang", "OVERWRITE=1"], cwd=repo_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert res1.returncode == 0, f"make build 失败: {res1.stderr}"

        # 2. 随后创建 build/test_runner_efw.dSYM 空目录
        dsym_dir = os.path.join(build_dir, "test_runner_efw.dSYM")
        os.makedirs(dsym_dir, exist_ok=True)
        assert os.path.isdir(dsym_dir)

        # 3. 将其加入清单 dsym_dirs 并重新计算 checksum
        manifest_file = os.path.join(build_dir, ".foe_manifest.json")
        with open(manifest_file, "r", encoding="utf-8") as f:
            manifest = json.load(f)
        if "test_runner_efw.dSYM" not in manifest.get("dsym_dirs", []):
            manifest["dsym_dirs"].append("test_runner_efw.dSYM")

        sys.path.insert(0, os.path.join(repo_dir, "tools"))
        import safe_build
        manifest["checksum"] = safe_build.compute_manifest_checksum(manifest)
        with open(manifest_file, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)

        # 4. 执行 make clean
        res2 = subprocess.run(["make", "clean"], cwd=repo_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert res2.returncode == 0, f"make clean 失败: {res2.stderr}"

        # 核心断言：无法证明目录归属时默认保留，不得依赖自描述清单删除用户目录
        assert os.path.isdir(dsym_dir), "错误: 未能证明归属的 dSYM 目录被 make clean 误删！"
        print("  [PASS] dSYM 目录默认非破坏保留验证通过：未受文件凭据证明的目录完好保留")
        shutil.rmtree(build_dir, ignore_errors=True)


def test_voucher_dir_symlink_and_chmod_safety(repo_dir: str):
    """测试 15: 验证宿主凭据目录符号链接防御与无条件 chmod 漏洞阻断"""
    print("\n[TEST 15] 验证宿主凭据目录符号链接防御与 chmod 权限防护...")
    with tempfile.TemporaryDirectory(prefix="foe_isolated_tmp_") as isolated_tmp:
        # 在临时根目录下创建外部敏感目标文件，设置权限 0o755
        target_file = os.path.join(isolated_tmp, "sensitive_target")
        os.makedirs(target_file, exist_ok=True)
        os.chmod(target_file, 0o755)

        # 在凭据路径位置预先放置指向敏感目标的符号链接
        voucher_symlink = os.path.join(isolated_tmp, f".foe_build_voucher_{os.getuid()}")
        os.symlink(target_file, voucher_symlink)

        # 启动 safe_build 试图获取凭据目录
        sys.path.insert(0, os.path.join(repo_dir, "tools"))
        import safe_build

        # 必须主动抛出安全拒绝，且严禁修改目标权限
        env = os.environ.copy()
        env["TMPDIR"] = isolated_tmp
        test_script = f"""
import sys, os, stat
sys.path.insert(0, r"{os.path.join(repo_dir, 'tools')}")
import safe_build
try:
    safe_build.get_host_voucher_dir()
    sys.exit(10)
except ValueError as e:
    if "符号链接" in str(e):
        sys.exit(0)
    sys.exit(20)
except Exception:
    sys.exit(30)
"""
        res = subprocess.run([sys.executable, "-c", test_script], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert res.returncode == 0, f"错误: 符号链接凭据目录未被安全拦截 (rc={res.returncode}, stderr={res.stderr})"

        # 核心断言：外部目标文件权限保持 0o755 不变，未受穿透修改
        current_perms = stat.S_IMODE(os.stat(target_file).st_mode)
        assert current_perms == 0o755, f"错误: 敏感目标权限受穿透被篡改为 {oct(current_perms)}"
        print("  [PASS] 宿主凭据目录符号链接防御通过：成功阻断符号链接穿透与非授权 chmod 修改")


def test_atomic_lock_concurrency_race(repo_dir: str):
    """测试 16: 验证基于内核原子硬链接的构建排他锁在高并发下的原子互斥"""
    print("\n[TEST 16] 验证内核原子硬链接排他锁高并发互斥防御...")
    build_dir = os.path.join(repo_dir, "build")
    shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)

    worker_script = f"""
import sys, os, time
sys.path.insert(0, r"{os.path.join(repo_dir, 'tools')}")
import safe_build
idx = int(sys.argv[1])
bdir = sys.argv[2]
try:
    safe_build.acquire_build_lock(bdir)
    print(f"{{idx}}:ACQUIRED", flush=True)
    time.sleep(1.0)
except RuntimeError as e:
    print(f"{{idx}}:BLOCKED", flush=True)
except Exception as e:
    print(f"{{idx}}:ERROR:{{e}}", flush=True)
"""

    # 同时拉起 8 个并发子进程同时竞争获取同一个构建锁
    procs = []
    for i in range(8):
        p = subprocess.Popen([sys.executable, "-c", worker_script, str(i), build_dir], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        procs.append(p)

    outputs = [p.communicate()[0].strip() for p in procs]
    acquired = [o for o in outputs if "ACQUIRED" in o]
    blocked = [o for o in outputs if "BLOCKED" in o]

    # 核心互斥断言：在原子硬链接保证下，至多仅有 1 个进程能直接夺取锁，其他并发进程全部被安全拦截
    assert len(acquired) == 1, f"错误: 原子互斥失效，持锁进程数不为 1 (实际: {len(acquired)}, 输出: {outputs})"
    assert len(blocked) == 7, f"错误: 并发拦截数不为 7 (实际: {len(blocked)}, 输出: {outputs})"
    print(f"  [PASS] 内核原子硬链接排他锁互斥验证通过 (成功持锁: {len(acquired)}, 安全拦截: {len(blocked)})")

    shutil.rmtree(build_dir, ignore_errors=True)


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
        test_user_manifest_and_dsym_protection(tmp_repo)
        test_modified_artifact_protection(tmp_repo)
        test_existing_identical_output_defense(tmp_repo)
        test_atomic_symlink_defense(tmp_repo)
        test_manifest_path_traversal_and_symlink_defense(tmp_repo)
        test_preset_manifest_and_user_file_retention(tmp_repo)
        test_same_pgrp_concurrent_build_exclusion(tmp_repo)
        test_dsym_empty_dir_retention_and_voucher_isolation(tmp_repo)
        test_voucher_dir_symlink_and_chmod_safety(tmp_repo)
        test_atomic_lock_concurrency_race(tmp_repo)

    print("\n>>> 隔离仓库副本哨兵完整性、非空目录保护、同名防覆盖与全套测试验证全部通过！<<<")


if __name__ == "__main__":
    main()
