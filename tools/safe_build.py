#!/usr/bin/env python3
"""
tools/safe_build.py: 本地构建与安全清理管理工具
实现文件级归属登记、哈希比对与安全清理，严禁递归删除未确认归属的目录或误删用户文件。
默认非破坏原则：不能可靠证明归属时完整保留，严格限制可信路径且不跟随父路径符号链接。
"""

import os
import sys
import json
import hashlib
import argparse
import errno

MANIFEST_NAME = ".foe_manifest.json"
MANIFEST_MAGIC = "ECAT_FOE_BUILD_MANIFEST_V1"
LOCK_NAME = ".foe_build.lock"

# 严格受控构建目标名称白名单（纯文件名，严禁包含路径分隔符）
ALLOWED_TARGET_NAMES = frozenset({"test_runner_efw", "test_runner_core"})


def sha256_file(filepath: str) -> str:
    """计算常规文件的 SHA-256 哈希值"""
    h = hashlib.sha256()
    with open(filepath, 'rb') as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def compute_manifest_checksum(data: dict) -> str:
    """计算清单核心数据结构的哈希指纹，防止清单被篡改"""
    core_data = {
        "magic": data.get("magic"),
        "version": data.get("version"),
        "created_by": data.get("created_by"),
        "artifacts": data.get("artifacts"),
        "dsym_files": data.get("dsym_files"),
        "dsym_dirs": data.get("dsym_dirs"),
    }
    raw = json.dumps(core_data, sort_keys=True, separators=(',', ':')).encode('utf-8')
    return hashlib.sha256(raw).hexdigest()


def is_safe_artifact_name(name: str) -> bool:
    """核验是否为白名单内的安全产物文件名（纯文件名，严禁包含路径分隔符、.. 或以根路径开头）"""
    if not isinstance(name, str) or not name:
        return False
    if os.path.basename(name) != name:
        return False
    if "/" in name or "\\" in name or ".." in name:
        return False
    return name in ALLOWED_TARGET_NAMES


def is_safe_dsym_relpath(rel_path: str) -> bool:
    """核验是否为合法的受控 dSYM 内部文件相对路径"""
    if not isinstance(rel_path, str) or not rel_path:
        return False
    if os.path.isabs(rel_path):
        return False
    norm = os.path.normpath(rel_path)
    parts = norm.split(os.sep)
    if any(p in ("..", ".", "") for p in parts):
        return False
    top_dir = parts[0]
    if not (top_dir.endswith(".dSYM") and top_dir[:-5] in ALLOWED_TARGET_NAMES):
        return False
    is_std = (
        norm.endswith(f"Contents{os.sep}Info.plist") or
        f"{os.sep}Contents{os.sep}Resources{os.sep}DWARF{os.sep}" in norm or
        f"{os.sep}Contents{os.sep}Resources{os.sep}Relocations{os.sep}" in norm
    )
    return is_std


def is_safe_dsym_dir_relpath(rel_dir: str) -> bool:
    """核验是否为合法的受控 dSYM 目录相对路径（严禁根路径、.. 及非受控顶层目录）"""
    if not isinstance(rel_dir, str) or not rel_dir:
        return False
    if os.path.isabs(rel_dir):
        return False
    norm = os.path.normpath(rel_dir)
    parts = norm.split(os.sep)
    if any(p in ("..", ".", "") for p in parts):
        return False
    top_dir = parts[0]
    if not (top_dir.endswith(".dSYM") and top_dir[:-5] in ALLOWED_TARGET_NAMES):
        return False
    # 子目录结构核验：允许顶层 .dSYM 目录本身或内部标准子目录层级 (Contents, Resources, DWARF, Relocations 等)
    allowed_subdirs = {"Contents", "Resources", "DWARF", "Relocations"}
    for sub in parts[1:]:
        if sub not in allowed_subdirs and not any(sub.startswith(arch) for arch in ("arm", "x86", "aarch")):
            return False
    return True


def verify_strictly_inside_build_dir(build_dir: str, rel_path: str):
    """严格核验目标路径位于 build_dir 内部，且无符号链接穿透或路径逃逸"""
    if not isinstance(rel_path, str) or not rel_path or os.path.isabs(rel_path):
        return None
    norm = os.path.normpath(rel_path)
    parts = norm.split(os.sep)
    if any(p in ("..", ".", "") for p in parts):
        return None

    if os.path.islink(build_dir):
        return None
    real_build_dir = os.path.realpath(build_dir)

    full_path = os.path.join(build_dir, norm)

    # 检查从 build_dir 到 full_path 的每一级父目录，严禁符号链接
    cur = build_dir
    for p in parts[:-1]:
        cur = os.path.join(cur, p)
        if os.path.islink(cur):
            return None

    # 检查真实父目录是否位于 real_build_dir 内部
    parent_dir = os.path.dirname(full_path)
    if os.path.exists(parent_dir):
        real_parent = os.path.realpath(parent_dir)
        try:
            if os.path.commonpath([real_build_dir, real_parent]) != real_build_dir:
                return None
        except ValueError:
            return None

    # 检查目标自身如果是符号链接，则拒绝
    if os.path.islink(full_path):
        return None

    return full_path


def validate_manifest_integrity_and_paths(build_dir: str, manifest_data: dict) -> bool:
    """统一安全核验构建清单：凡格式异常、指纹不符、含越界或非受控路径者一律判定为不可信"""
    if not isinstance(manifest_data, dict):
        return False

    if manifest_data.get("magic") != MANIFEST_MAGIC:
        return False

    expected_csum = manifest_data.get("checksum")
    actual_csum = compute_manifest_checksum(manifest_data)
    if not expected_csum or expected_csum != actual_csum:
        return False

    raw_artifacts = manifest_data.get("artifacts", {})
    raw_dsym_files = manifest_data.get("dsym_files", {})
    raw_dsym_dirs = manifest_data.get("dsym_dirs", [])

    if not isinstance(raw_artifacts, dict) or not isinstance(raw_dsym_files, dict) or not isinstance(raw_dsym_dirs, list):
        return False

    # 1. 统一核验产物文件路径
    for fname in raw_artifacts:
        if not is_safe_artifact_name(fname):
            return False
        if not verify_strictly_inside_build_dir(build_dir, fname):
            return False

    # 2. 统一核验 dSYM 文件路径
    for rel_f in raw_dsym_files:
        if not is_safe_dsym_relpath(rel_f):
            return False
        if not verify_strictly_inside_build_dir(build_dir, rel_f):
            return False

    # 3. 统一核验 dSYM 目录路径（严防通过 dsym_dirs 逃逸越界删除外部目录）
    for rel_d in raw_dsym_dirs:
        if not is_safe_dsym_dir_relpath(rel_d):
            return False
        if not verify_strictly_inside_build_dir(build_dir, rel_d):
            return False

    return True


def acquire_build_lock(build_dir: str):
    """获取构建排他标记，防范并发构建竞态。采用父进程与进程组生命周期校验"""
    lock_file = os.path.join(build_dir, LOCK_NAME)
    if os.path.islink(lock_file):
        raise ValueError(f"安全拒绝：锁文件为符号链接: {lock_file}")

    current_ppid = os.getppid()
    current_pgrp = os.getpgrp()

    if os.path.exists(lock_file):
        lock_info = {}
        try:
            with open(lock_file, "r", encoding="utf-8") as f:
                lock_info = json.load(f)
        except Exception:
            pass

        old_ppid = lock_info.get("ppid")
        old_pgrp = lock_info.get("pgrp")

        # 若原持锁进程属于当前父进程或同属当前进程组，视为同一次构建流程内部流转
        if old_ppid == current_ppid or (old_pgrp is not None and old_pgrp == current_pgrp):
            return

        # 检查原持锁父进程是否仍然存活
        is_alive = False
        if old_ppid is not None:
            try:
                os.kill(old_ppid, 0)
                is_alive = True
            except OSError:
                is_alive = False

        if is_alive:
            raise RuntimeError(f"安全拒绝：检测到并发构建正在运行 (父进程 PID {old_ppid})，锁文件已锁定")

    # 写入新锁信息（带当前会话与父进程标识）
    lock_data = {
        "ppid": current_ppid,
        "pgrp": current_pgrp,
        "pid": os.getpid()
    }
    tmp_lock = os.path.join(build_dir, f".lock_tmp_{os.getpid()}")
    fd = os.open(tmp_lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0), 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(lock_data, f)
    os.replace(tmp_lock, lock_file)


def release_build_lock(build_dir: str):
    """释放构建排他标记，核验会话归属后安全清理"""
    lock_file = os.path.join(build_dir, LOCK_NAME)
    if os.path.isfile(lock_file) and not os.path.islink(lock_file):
        try:
            with open(lock_file, "r", encoding="utf-8") as f:
                lock_info = json.load(f)
            # 确认系当前父进程或进程组持有的锁才予以删除
            if lock_info.get("ppid") == os.getppid() or lock_info.get("pgrp") == os.getpgrp():
                os.remove(lock_file)
        except Exception:
            pass


def check_before_build(build_dir: str, target_files: list, overwrite: bool = False):
    """构建前核验：保护已有清单、符号链接以及已修改的目标文件与 dSYM 用户文件"""
    if os.path.islink(build_dir):
        raise ValueError(f"安全拒绝：构建目录为符号链接: {build_dir}")

    # 获取构建排他标记，防止并发竞态
    acquire_build_lock(build_dir)

    manifest_path = os.path.join(build_dir, MANIFEST_NAME)
    if os.path.islink(manifest_path):
        raise ValueError(f"安全拒绝：清单路径为符号链接: {manifest_path}")

    # 白名单检验待构建目标
    for fname in target_files:
        if not is_safe_artifact_name(fname):
            raise ValueError(f"安全拒绝：非白名单构建目标: {fname}")

    recorded_artifacts = {}
    recorded_dsym_dirs = set()
    recorded_dsym_files = {}

    if os.path.isfile(manifest_path):
        try:
            with open(manifest_path, 'r', encoding='utf-8') as f:
                mdata = json.load(f)
        except Exception as e:
            if not overwrite:
                raise FileExistsError(f"安全拒绝：清单文件已存在但无法读取: {manifest_path}") from e
            mdata = {}

        # 若已有清单缺少归属魔数，说明系用户预置或外来文件，非授权时严禁覆盖
        if mdata.get("magic") != MANIFEST_MAGIC and not overwrite:
            raise FileExistsError(f"安全拒绝：清单文件 {manifest_path} 已存在但缺少合法归属魔数，疑似用户文件")

        # 若清单内容被篡改（校验和不匹配）或包含非法非白名单条目，非授权时严禁覆盖
        if mdata.get("magic") == MANIFEST_MAGIC and not overwrite:
            expected_csum = mdata.get("checksum")
            actual_csum = compute_manifest_checksum(mdata)
            if expected_csum != actual_csum:
                raise FileExistsError(f"安全拒绝：已有清单指纹不吻合（疑似被手动修改或非标准清单），拒绝覆盖")

            for afname in mdata.get("artifacts", {}):
                if not is_safe_artifact_name(afname):
                    raise FileExistsError(f"安全拒绝：已有清单中包含非受控路径 {afname}，疑似用户文件")

        recorded_artifacts = mdata.get("artifacts", {})
        recorded_dsym_dirs = set(mdata.get("dsym_dirs", []))
        recorded_dsym_files = mdata.get("dsym_files", {})

    for fname in target_files:
        fpath = os.path.join(build_dir, fname)
        if os.path.islink(fpath):
            raise ValueError(f"安全拒绝：构建目标为符号链接: {fpath}")

        dsym_name = fname + ".dSYM"
        dsym_path = fpath + ".dSYM"
        if os.path.islink(dsym_path):
            raise ValueError(f"安全拒绝：构建目标符号目录为符号链接: {dsym_path}")

        # 检查二进制目标文件
        if os.path.exists(fpath):
            if fname not in recorded_artifacts:
                if not overwrite:
                    raise FileExistsError(f"安全拒绝：构建目标文件已存在且未在清单中登记: {fpath}")
            else:
                # 已登记文件：进一步检查当前哈希是否与上次记录吻合；若已被修改且未授权覆盖，主动报错拒绝
                rec = recorded_artifacts[fname]
                expected_hash = rec.get("hash") if isinstance(rec, dict) else rec
                current_hash = sha256_file(fpath)
                if current_hash != expected_hash and not overwrite:
                    raise FileExistsError(f"安全拒绝：构建目标文件 {fpath} 已被用户修改（与清单哈希不符），拒绝覆盖")

        # 检查调试符号目录：若既存但在清单中未登记，或内部含有未登记/已修改文件，主动报错拒绝
        if os.path.exists(dsym_path):
            if dsym_name not in recorded_dsym_dirs:
                if not overwrite:
                    raise FileExistsError(f"安全拒绝：调试符号目录已存在且未在清单中登记: {dsym_path}")
            else:
                for root, _, files in os.walk(dsym_path):
                    for f in files:
                        fp = os.path.join(root, f)
                        rel = os.path.relpath(fp, build_dir)
                        if rel not in recorded_dsym_files and not overwrite:
                            raise FileExistsError(f"安全拒绝：调试符号目录中存在未登记文件 {rel}，疑似用户文件")
                        if rel in recorded_dsym_files:
                            expected_fhash = recorded_dsym_files[rel]
                            if sha256_file(fp) != expected_fhash and not overwrite:
                                raise FileExistsError(f"安全拒绝：调试符号文件 {rel} 已被修改，拒绝覆盖")


def record_build(build_dir: str, target_files: list):
    """构建后登记：记录本次构建生成的二进制及调试符号文件清单，并原子写入清单"""
    try:
        if os.path.islink(build_dir):
            raise ValueError(f"安全拒绝：构建目录为符号链接: {build_dir}")

        manifest_path = os.path.join(build_dir, MANIFEST_NAME)
        if os.path.islink(manifest_path):
            raise ValueError(f"安全拒绝：清单路径为符号链接: {manifest_path}")

        artifacts = {}
        dsym_files = {}
        dsym_dirs = []

        for fname in target_files:
            if not is_safe_artifact_name(fname):
                raise ValueError(f"安全拒绝：非白名单构建目标: {fname}")

            fpath = os.path.join(build_dir, fname)
            if os.path.islink(fpath):
                raise ValueError(f"安全拒绝：构建目标为符号链接: {fpath}")

            if os.path.isfile(fpath):
                artifacts[fname] = {
                    "hash": sha256_file(fpath),
                    "size": os.path.getsize(fpath)
                }

            dsym_path = os.path.join(build_dir, fname + ".dSYM")
            if os.path.islink(dsym_path):
                raise ValueError(f"安全拒绝：调试符号目录为符号链接: {dsym_path}")

            if os.path.isdir(dsym_path):
                has_valid_dsym_content = False
                for root, dirs, files in os.walk(dsym_path, topdown=False):
                    for f in files:
                        fp = os.path.join(root, f)
                        if not os.path.islink(fp) and os.path.isfile(fp):
                            rel = os.path.relpath(fp, build_dir)
                            if is_safe_dsym_relpath(rel):
                                dsym_files[rel] = sha256_file(fp)
                                has_valid_dsym_content = True
                    for d in dirs:
                        dp = os.path.join(root, d)
                        if not os.path.islink(dp) and os.path.isdir(dp):
                            rel_d = os.path.relpath(dp, build_dir)
                            if is_safe_dsym_dir_relpath(rel_d):
                                dsym_dirs.append(rel_d)
                if has_valid_dsym_content:
                    dsym_dirs.append(fname + ".dSYM")

        manifest_data = {
            "magic": MANIFEST_MAGIC,
            "version": 1,
            "created_by": "foe_build_system",
            "artifacts": artifacts,
            "dsym_files": dsym_files,
            "dsym_dirs": dsym_dirs
        }
        manifest_data["checksum"] = compute_manifest_checksum(manifest_data)

        tmp_manifest = os.path.join(build_dir, f".manifest_tmp_{os.getpid()}")
        fd = os.open(tmp_manifest, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0), 0o600)
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            json.dump(manifest_data, f, indent=2)
        os.replace(tmp_manifest, manifest_path)
    finally:
        release_build_lock(build_dir)


def clean_build(build_dir: str):
    """安全清理：仅删除经核验确由本次构建生成的具体文件与目录，不能可靠证明归属时一律保留"""
    if not os.path.isdir(build_dir) or os.path.islink(build_dir):
        return

    manifest_path = os.path.join(build_dir, MANIFEST_NAME)
    if not os.path.isfile(manifest_path) or os.path.islink(manifest_path):
        print("  [CLEAN] 未发现合法构建清单，跳过清理以保护存量文件。")
        return

    try:
        with open(manifest_path, 'r', encoding='utf-8') as f:
            manifest_data = json.load(f)
    except Exception as e:
        print(f"  [CLEAN] 读取构建清单失败: {e}，跳过清理以防误删。")
        return

    # 全量统一安全校验：核验魔数、防伪指纹、产物文件、dSYM 文件及 dSYM 目录路径
    # 凡存在未校验通过的非受控路径、越界路径或指纹不符者，一律判定不可信并终止清理
    if not validate_manifest_integrity_and_paths(build_dir, manifest_data):
        print("  [CLEAN] 清单存在未校验通过的非受控路径、越界路径或指纹不符，跳过清理以保护存量文件与目录。")
        return

    raw_artifacts = manifest_data.get("artifacts", {})
    raw_dsym_files = manifest_data.get("dsym_files", {})
    raw_dsym_dirs = manifest_data.get("dsym_dirs", [])

    # 检查 dSYM 目录中是否存在未登记用户文件
    # 若 dSYM 目录内含有用户数据，整棵 dSYM 目录全部保留不删
    protected_dsym_dirs = set()
    for dsym in raw_dsym_dirs:
        dsym_path = os.path.join(build_dir, dsym)
        if os.path.isdir(dsym_path) and not os.path.islink(dsym_path):
            has_unknown_file = False
            for root, _, files in os.walk(dsym_path):
                for f in files:
                    fp = os.path.join(root, f)
                    rel = os.path.relpath(fp, build_dir)
                    if rel not in raw_dsym_files:
                        has_unknown_file = True
                        break
                if has_unknown_file:
                    break
            if has_unknown_file:
                print(f"  [CLEAN] 调试符号目录 {dsym} 内检测到用户存量文件，整棵目录予以保留不删。")
                protected_dsym_dirs.add(dsym)

    # 1. 逐一删除经哈希校验的二进制文件
    for fname, finfo in raw_artifacts.items():
        fpath = os.path.join(build_dir, fname)
        if os.path.isfile(fpath) and not os.path.islink(fpath):
            expected_hash = finfo.get("hash") if isinstance(finfo, dict) else finfo
            actual_hash = sha256_file(fpath)
            if actual_hash == expected_hash:
                os.remove(fpath)
                print(f"  [CLEAN] 已安全清理构建产物: {fname}")
            else:
                print(f"  [WARN] {fname} 内容已被修改，保留文件不予删除。")

    # 2. 逐一删除经哈希校验的 dSYM 具体文件（非受保护目录下的）
    for rel_file, expected_hash in raw_dsym_files.items():
        top_dsym = rel_file.split(os.sep)[0]
        if top_dsym in protected_dsym_dirs:
            continue

        fpath = os.path.join(build_dir, rel_file)
        if os.path.isfile(fpath) and not os.path.islink(fpath):
            if sha256_file(fpath) == expected_hash:
                os.remove(fpath)
                print(f"  [CLEAN] 已安全清理调试符号文件: {rel_file}")
            else:
                print(f"  [WARN] {rel_file} 内容已被修改，保留文件不予删除。")

    # 3. 逐层尝试 rmdir 移除空的 dSYM 目录层级（运行时再核验路径必须严格位于构建目录内部）
    for rel_dir in sorted(raw_dsym_dirs, key=len, reverse=True):
        top_dsym = rel_dir.split(os.sep)[0]
        if top_dsym in protected_dsym_dirs:
            continue
        dpath = verify_strictly_inside_build_dir(build_dir, rel_dir)
        if not dpath:
            continue
        if os.path.isdir(dpath) and not os.path.islink(dpath):
            try:
                os.rmdir(dpath)
            except OSError as e:
                if e.errno in (errno.ENOTEMPTY, errno.EEXIST):
                    print(f"  [CLEAN] 目录 {rel_dir} 内仍有存量文件，保留目录不予删除。")
                else:
                    raise

    # 4. 移除清单本身
    if os.path.isfile(manifest_path) and not os.path.islink(manifest_path):
        os.remove(manifest_path)

    # 5. 尝试 rmdir 构建根目录：仅当目录已完全为空时安全移除
    try:
        os.rmdir(build_dir)
        print(f"  [CLEAN] 构建目录 {build_dir} 已为空，已安全移除。")
    except OSError as e:
        if e.errno in (errno.ENOTEMPTY, errno.EEXIST):
            print(f"  [CLEAN] 构建目录 {build_dir} 内仍有存量文件，保留目录不予删除。")
        else:
            raise


def main():
    parser = argparse.ArgumentParser(description="安全构建与清理管理工具")
    subparsers = parser.add_subparsers(dest="action")

    # check
    p_check = subparsers.add_parser("check")
    p_check.add_argument("--build-dir", required=True)
    p_check.add_argument("--files", nargs="+", required=True)
    p_check.add_argument("--overwrite", action="store_true", default=False)

    # record
    p_rec = subparsers.add_parser("record")
    p_rec.add_argument("--build-dir", required=True)
    p_rec.add_argument("--files", nargs="+", required=True)

    # clean
    p_clean = subparsers.add_parser("clean")
    p_clean.add_argument("--build-dir", required=True)

    args = parser.parse_args()
    if args.action == "check":
        check_before_build(args.build_dir, args.files, args.overwrite)
    elif args.action == "record":
        record_build(args.build_dir, args.files)
    elif args.action == "clean":
        clean_build(args.build_dir)


if __name__ == "__main__":
    main()
