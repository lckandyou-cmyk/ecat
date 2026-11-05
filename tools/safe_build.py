#!/usr/bin/env python3
"""
tools/safe_build.py: 本地构建与安全清理管理工具
实现文件级归属登记与安全清理，杜绝删除用户文件或清理非空目录。
"""

import os
import sys
import json
import hashlib
import argparse
import errno

MANIFEST_NAME = ".foe_manifest.json"


def sha256_file(filepath: str) -> str:
    h = hashlib.sha256()
    with open(filepath, 'rb') as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def check_before_build(build_dir: str, target_files: list, overwrite: bool = False):
    """构建前核验：若目标输出文件已存在且未在 manifest 中登记，拒绝静默覆盖"""
    manifest_path = os.path.join(build_dir, MANIFEST_NAME)
    recorded = {}
    if os.path.isfile(manifest_path):
        try:
            with open(manifest_path, 'r', encoding='utf-8') as f:
                data = json.load(f)
                recorded = data.get("artifacts", {})
        except Exception:
            pass

    for fname in target_files:
        fpath = os.path.join(build_dir, fname)
        if os.path.islink(fpath):
            raise ValueError(f"安全拒绝：构建目标为符号链接: {fpath}")
        if os.path.exists(fpath):
            if fname not in recorded and not overwrite:
                raise FileExistsError(f"安全拒绝：构建输出文件已存在且无归属证明: {fpath}")


def record_build(build_dir: str, target_files: list):
    """构建后登记：记录本次构建生成的二进制哈希与对应调试符号目录"""
    manifest_path = os.path.join(build_dir, MANIFEST_NAME)
    artifacts = {}
    dsym_dirs = []

    for fname in target_files:
        fpath = os.path.join(build_dir, fname)
        if os.path.isfile(fpath) and not os.path.islink(fpath):
            artifacts[fname] = sha256_file(fpath)
            dsym_path = fpath + ".dSYM"
            if os.path.isdir(dsym_path) and not os.path.islink(dsym_path):
                dsym_dirs.append(fname + ".dSYM")

    manifest_data = {
        "version": 1,
        "created_by": "foe_build_system",
        "artifacts": artifacts,
        "dsym_dirs": dsym_dirs
    }
    with open(manifest_path, 'w', encoding='utf-8') as f:
        json.dump(manifest_data, f, indent=2)


def clean_build(build_dir: str):
    """安全清理：仅删除经哈希比对确认为构建产物的文件，不删除未登记的用户文件，非空目录不删除"""
    manifest_path = os.path.join(build_dir, MANIFEST_NAME)
    if not os.path.isfile(manifest_path):
        print(f"  [CLEAN] 未发现构建清单 {manifest_path}，跳过清理以保护存量文件。")
        return

    try:
        with open(manifest_path, 'r', encoding='utf-8') as f:
            manifest_data = json.load(f)
    except Exception as e:
        print(f"  [CLEAN] 读取构建清单失败: {e}，跳过清理以防误删。")
        return

    artifacts = manifest_data.get("artifacts", {})
    dsym_dirs = manifest_data.get("dsym_dirs", [])

    # 1. 逐一删除有证明的二进制文件
    for fname, expected_hash in artifacts.items():
        fpath = os.path.join(build_dir, fname)
        if os.path.islink(fpath):
            print(f"  [WARN] {fpath} 为符号链接，跳过删除。")
            continue
        if os.path.isfile(fpath):
            actual_hash = sha256_file(fpath)
            if actual_hash == expected_hash:
                os.remove(fpath)
                print(f"  [CLEAN] 已清理构建产物: {fname}")
            else:
                print(f"  [WARN] {fname} 内容已被修改（哈希不吻合），保留该文件不予删除。")

    # 2. 逐一删除对应的 .dSYM 目录
    for dsym in dsym_dirs:
        dsym_path = os.path.join(build_dir, dsym)
        if os.path.isdir(dsym_path) and not os.path.islink(dsym_path):
            import shutil
            shutil.rmtree(dsym_path, ignore_errors=True)
            print(f"  [CLEAN] 已清理调试符号目录: {dsym}")

    # 3. 移除清单文件本身
    if os.path.isfile(manifest_path):
        os.remove(manifest_path)

    # 4. 尝试 rmdir：如果目录内仍有 user.txt 等文件，rmdir 会安全报错被捕获，目录完好保留
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
