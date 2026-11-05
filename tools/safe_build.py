#!/usr/bin/env python3
"""
tools/safe_build.py: 本地构建与安全清理管理工具
实现文件级归属登记、哈希比对与安全清理，严禁递归删除未确认归属的目录或误删用户文件。
"""

import os
import sys
import json
import hashlib
import argparse
import errno

MANIFEST_NAME = ".foe_manifest.json"
MANIFEST_MAGIC = "ECAT_FOE_BUILD_MANIFEST_V1"


def sha256_file(filepath: str) -> str:
    """计算常规文件的 SHA-256 哈希值"""
    h = hashlib.sha256()
    with open(filepath, 'rb') as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def check_before_build(build_dir: str, target_files: list, overwrite: bool = False):
    """构建前核验：保护已有清单、符号链接以及已修改的目标文件"""
    if os.path.islink(build_dir):
        raise ValueError(f"安全拒绝：构建目录为符号链接: {build_dir}")

    manifest_path = os.path.join(build_dir, MANIFEST_NAME)
    if os.path.islink(manifest_path):
        raise ValueError(f"安全拒绝：清单路径为符号链接: {manifest_path}")

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
                # 已登记 dSYM 目录：逐一核查内部文件是否都在清单中且哈希吻合
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
    if os.path.islink(build_dir):
        raise ValueError(f"安全拒绝：构建目录为符号链接: {build_dir}")

    manifest_path = os.path.join(build_dir, MANIFEST_NAME)
    if os.path.islink(manifest_path):
        raise ValueError(f"安全拒绝：清单路径为符号链接: {manifest_path}")

    artifacts = {}
    dsym_files = {}
    dsym_dirs = []

    for fname in target_files:
        fpath = os.path.join(build_dir, fname)
        if os.path.isfile(fpath) and not os.path.islink(fpath):
            artifacts[fname] = {
                "hash": sha256_file(fpath),
                "size": os.path.getsize(fpath)
            }

        dsym_path = os.path.join(build_dir, fname + ".dSYM")
        if os.path.isdir(dsym_path) and not os.path.islink(dsym_path):
            # 逐一遍历记录 dSYM 目录下的具体文件与目录层级，严禁模糊全删
            # 仅收录符合 macOS 标准调试信息规范的文件，用户文件不作为调试产物登记
            has_valid_dsym_content = False
            for root, dirs, files in os.walk(dsym_path, topdown=False):
                for f in files:
                    fp = os.path.join(root, f)
                    if not os.path.islink(fp) and os.path.isfile(fp):
                        rel = os.path.relpath(fp, build_dir)
                        # 仅收录标准 dSYM 规范产物 (Info.plist, DWARF 二进制, Relocations)
                        # 凡非规范文件 (如 user.txt 等任意用户数据) 均不收录入删除清单
                        is_std_dsym = (
                            rel.endswith("/Contents/Info.plist") or
                            "/Contents/Resources/DWARF/" in rel or
                            "/Contents/Resources/Relocations/" in rel
                        )
                        if is_std_dsym:
                            dsym_files[rel] = sha256_file(fp)
                            has_valid_dsym_content = True
                for d in dirs:
                    dp = os.path.join(root, d)
                    if not os.path.islink(dp) and os.path.isdir(dp):
                        dsym_dirs.append(os.path.relpath(dp, build_dir))
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

    # 原子写入清单文件：先写独立临时文件再原子 replace，杜绝中断与竞态
    tmp_manifest = os.path.join(build_dir, f".manifest_tmp_{os.getpid()}")
    fd = os.open(tmp_manifest, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0), 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8') as f:
        json.dump(manifest_data, f, indent=2)
    os.replace(tmp_manifest, manifest_path)


def clean_build(build_dir: str):
    """安全清理：仅删除经哈希比对证明由本次构建生成的具体文件，非递归删除，非空目录完好保留"""
    if not os.path.isdir(build_dir) or os.path.islink(build_dir):
        return

    manifest_path = os.path.join(build_dir, MANIFEST_NAME)
    if not os.path.isfile(manifest_path) or os.path.islink(manifest_path):
        print(f"  [CLEAN] 未发现合法构建清单，跳过清理以保护存量文件。")
        return

    try:
        with open(manifest_path, 'r', encoding='utf-8') as f:
            manifest_data = json.load(f)
    except Exception as e:
        print(f"  [CLEAN] 读取构建清单失败: {e}，跳过清理以防误删。")
        return

    if manifest_data.get("magic") != MANIFEST_MAGIC:
        print(f"  [CLEAN] 清单文件缺少归属魔数，跳过清理以防误删用户文件。")
        return

    artifacts = manifest_data.get("artifacts", {})
    dsym_files = manifest_data.get("dsym_files", {})
    dsym_dirs = manifest_data.get("dsym_dirs", [])

    # 1. 逐一删除经哈希校验的二进制文件
    for fname, finfo in artifacts.items():
        fpath = os.path.join(build_dir, fname)
        if os.path.islink(fpath):
            print(f"  [WARN] {fpath} 为符号链接，跳过删除。")
            continue
        if os.path.isfile(fpath):
            expected_hash = finfo.get("hash") if isinstance(finfo, dict) else finfo
            actual_hash = sha256_file(fpath)
            if actual_hash == expected_hash:
                os.remove(fpath)
                print(f"  [CLEAN] 已安全清理构建产物: {fname}")
            else:
                print(f"  [WARN] {fname} 内容已被修改，保留文件不予删除。")

    # 2. 逐一删除经哈希校验的 dSYM 具体文件（非递归）
    for rel_file, expected_hash in dsym_files.items():
        fpath = os.path.join(build_dir, rel_file)
        if not os.path.islink(fpath) and os.path.isfile(fpath):
            if sha256_file(fpath) == expected_hash:
                os.remove(fpath)
                print(f"  [CLEAN] 已安全清理调试符号文件: {rel_file}")
            else:
                print(f"  [WARN] {rel_file} 内容已被修改，保留文件不予删除。")

    # 3. 逐层尝试 rmdir 移除空的 dSYM 目录层级（若内部有 user.txt 等文件，rmdir 会安全失败）
    for rel_dir in sorted(dsym_dirs, key=len, reverse=True):
        dpath = os.path.join(build_dir, rel_dir)
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
