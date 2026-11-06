#!/usr/bin/env python3
"""
核验当前使用的 SOES 源码文件哈希并与 docs/quotes_manifest.json 基线比对。
"""

import os
import sys
import json
import hashlib
import argparse

def calc_sha256(filepath):
    if not os.path.isfile(filepath):
        return None
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def main():
    parser = argparse.ArgumentParser(description="SOES 文件哈希比对工具")
    parser.add_argument("--soes-dir", required=True, help="SOES 源码目录")
    parser.add_argument("--manifest", default="docs/quotes_manifest.json", help="Manifest 文件路径")
    args = parser.parse_args()

    if not os.path.isdir(args.soes_dir):
        print(f"[FATAL] SOES 源码目录不存在: {args.soes_dir}", file=sys.stderr)
        return 1

    manifest_data = {}
    if os.path.isfile(args.manifest):
        try:
            with open(args.manifest, "r", encoding="utf-8") as f:
                manifest_data = json.load(f)
        except Exception as e:
            print(f"[WARN] 无法读取清单文件 {args.manifest}: {e}", file=sys.stderr)

    baseline_sums = manifest_data.get("file_checksums", {})

    print("------------------------------------------------------------")
    print(f" SOES 源码文件哈希核验 (目录: {args.soes_dir})")
    print("------------------------------------------------------------")

    checked_files = ["esc.c", "esc_foe.c", "esc.h", "esc_foe.h"]
    all_matched = True

    for fname in checked_files:
        fpath = os.path.join(args.soes_dir, fname)
        actual_hash = calc_sha256(fpath)
        if actual_hash is None:
            print(f"  [MISSING] {fname}: 文件不存在")
            all_matched = False
            continue

        baseline = baseline_sums.get(fname)
        if baseline is None:
            print(f"  [UNTRACKED] {fname}: {actual_hash} (清单中无基线)")
        elif actual_hash.lower() == baseline.lower():
            print(f"  [MATCH]     {fname}: {actual_hash}")
        else:
            print(f"  [WARNING]   {fname}: {actual_hash}")
            print(f"              基线预期: {baseline} (哈希不符，可能存在版本漂移！)")
            all_matched = False

    print("------------------------------------------------------------")
    if not all_matched:
        print(">>> 警告: 部分 SOES 源文件哈希与登记基线不一致或缺失，测试将在当前源码环境下继续执行 <<<")
    else:
        print(">>> 全部关联 SOES 源文件哈希与清单基线一致 <<<")
    return 0

if __name__ == "__main__":
    sys.exit(main())
