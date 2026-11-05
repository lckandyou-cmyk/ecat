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
"""

import os
import sys
import shutil
import json
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

    # 1. 复现审查发现的逃逸删除路径：在仓库根目录放置 valuable.txt
    valuable_path = os.path.join(repo_dir, "valuable.txt")
    valuable_content = b"CRITICAL_USER_VALUABLE_SOURCE_DATA_123456"
    with open(valuable_path, "wb") as f:
        f.write(valuable_content)
    valuable_hash = hashlib.sha256(valuable_content).hexdigest()

    # 在 build/.foe_manifest.json 中写入带 ../valuable.txt 的欺骗清单
    manifest_path = os.path.join(build_dir, ".foe_manifest.json")
    escape_manifest = {
        "magic": "ECAT_FOE_BUILD_MANIFEST_V1",
        "artifacts": {
            "../valuable.txt": valuable_hash
        }
    }
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(escape_manifest, f)

    # 运行 make clean：必须安全识别越界路径并拒绝清理
    res_c1 = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c1.returncode == 0, f"make clean 执行失败: {res_c1.stderr}"
    assert os.path.isfile(valuable_path), "错误: 仓库根目录下的 valuable.txt 被 clean 跨目录逃逸误删！"
    with open(valuable_path, "rb") as f:
        assert f.read() == valuable_content, "错误: valuable.txt 内容被篡改！"
    print("  [PASS] 跨目录逃逸 (../valuable.txt) 成功被拦截，根目录用户文件完好无损")

    # 2. 验证系统根路径与多级遍历组件防御
    root_escape_manifest = {
        "magic": "ECAT_FOE_BUILD_MANIFEST_V1",
        "artifacts": {
            "/tmp/non_existent_target": "dummy_hash",
            "../../outside.txt": "dummy_hash"
        }
    }
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(root_escape_manifest, f)

    res_c2 = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c2.returncode == 0, f"make clean 失败: {res_c2.stderr}"
    assert os.path.isfile(valuable_path), "错误: valuable.txt 在系统根路径测试中被误删！"
    print("  [PASS] 系统根路径与多级遍历逃逸清单成功被拦截，终止清理操作")

    # 3. 验证父目录符号链接穿透防御
    symlink_dir = os.path.join(build_dir, "link_to_parent")
    if os.path.exists(symlink_dir) or os.path.islink(symlink_dir):
        os.remove(symlink_dir)
    os.symlink("..", symlink_dir)

    symlink_escape_manifest = {
        "magic": "ECAT_FOE_BUILD_MANIFEST_V1",
        "artifacts": {
            "link_to_parent/valuable.txt": valuable_hash
        }
    }
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(symlink_escape_manifest, f)

    res_c3 = subprocess.run(["make", "clean"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_c3.returncode == 0, f"make clean 失败: {res_c3.stderr}"
    assert os.path.isfile(valuable_path), "错误: valuable.txt 在父目录符号链接测试中被误删！"
    os.remove(symlink_dir)
    print("  [PASS] 父目录符号链接穿透成功被识别并安全拦截")

    # 4. 验证被篡改的清单（指纹不匹配）安全防御
    # 先生成合法受控构建
    res_b = subprocess.run(["make", "build", "CC=clang", "OVERWRITE=1"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b.returncode == 0, f"make build 失败: {res_b.stderr}"

    target_bin = os.path.join(build_dir, "test_runner_efw")
    assert os.path.isfile(target_bin), "构建产物未生成"

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

    # 5. 验证包含用户文件的 dSYM 目录整棵保留机制
    # 重新生成受控构建并在 dSYM 中写入 user.txt
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

    # 6. 验证并发构建锁防竞态
    lock_file = os.path.join(build_dir, ".foe_build.lock")
    with open(lock_file, "w", encoding="utf-8") as f:
        f.write(str(os.getpid()))  # 写入当前正在运行的存活进程 PID

    res_b_race = subprocess.run(["make", "build", "CC=clang"], cwd=repo_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert res_b_race.returncode != 0, "错误: 在存活构建锁存在时未能拒绝并发构建！"
    os.remove(lock_file)
    print("  [PASS] 并发构建排他锁生效：成功拦截并发竞态构建冲突")

    # 清理测试现场
    if os.path.isfile(valuable_path):
        os.remove(valuable_path)
    shutil.rmtree(build_dir, ignore_errors=True)


def test_atomic_symlink_defense(repo_dir: str):
    """测试 10: 验证内核级原子符号链接防御 (O_NOFOLLOW)，即使传入 --overwrite 亦绝不穿透"""
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

    print("\n>>> 隔离仓库副本哨兵完整性、非空目录保护、同名防覆盖与全套测试验证全部通过！<<<")


if __name__ == "__main__":
    main()
