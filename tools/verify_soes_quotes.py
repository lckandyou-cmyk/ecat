#!/usr/bin/env python3
"""
tools/verify_soes_quotes.py

三方严格核验 SOES 源码引用工具 (Three-way Quote Verification Tool)。
比对三方数据源：
1. 外部清单文件 (docs/quotes_manifest.json)；
2. 项目技术文档内标记代码块 (<!-- quote-begin: ID --> ... <!-- quote-end: ID -->)；
3. 本地真实 SOES 源码文件 (按照行号范围提取的逐字源码)。

【覆盖范围与承诺边界】：
本工具仅针对外部清单文件显式登记的条目执行核验并作出真实性承诺，
不声称亦不覆盖任何未登记的文本段落。
若任何一方缺失、标记不匹配、行号越界或文本字符不一致，均非零退出。
"""

import os
import sys
import json
import re
import argparse
from typing import List, Dict, Any, Tuple

def extract_quote_from_doc(doc_path: str, qid: str) -> Tuple[bool, List[str], str]:
    """从 markdown 文档中根据 HTML 注释标记提取代码块中的引文字符串列表"""
    if not os.path.isfile(doc_path):
        return False, [], f"文档文件不存在: {doc_path}"

    with open(doc_path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    begin_tag = f"<!-- quote-begin: {qid} -->"
    end_tag = f"<!-- quote-end: {qid} -->"

    begin_pos = content.find(begin_tag)
    if begin_pos == -1:
        return False, [], f"文档中未找到起始标记: {begin_tag}"

    end_pos = content.find(end_tag, begin_pos + len(begin_tag))
    if end_pos == -1:
        return False, [], f"文档中未找到结束标记: {end_tag}"

    block = content[begin_pos + len(begin_tag) : end_pos].strip()

    # 提取 fenced code block 内部内容
    lines = block.splitlines()
    snippet_lines = []
    in_code = False
    for line in lines:
        line_clean = line.rstrip("\r\n")
        if line_clean.startswith("```"):
            if not in_code:
                in_code = True
                continue
            else:
                in_code = False
                break
        if in_code:
            snippet_lines.append(line_clean)

    if not snippet_lines:
        return False, [], f"标记 {qid} 内部未找到有效的代码块内容"

    return True, snippet_lines, ""

def extract_quote_from_source(source_path: str, start_line: int, end_line: int) -> Tuple[bool, List[str], str]:
    """从真实 SOES 源码中提取指定行号范围的文本"""
    if not os.path.isfile(source_path):
        return False, [], f"源码文件不存在: {source_path}"

    with open(source_path, "r", encoding="utf-8", errors="replace") as f:
        all_lines = [l.rstrip("\r\n") for l in f]

    if start_line < 1 or end_line > len(all_lines) or start_line > end_line:
        return False, [], f"行号范围越界: {start_line}-{end_line} (源文件总行数: {len(all_lines)})"

    snippet = all_lines[start_line - 1 : end_line]
    return True, snippet, ""

def verify_three_way(manifest_path: str, repo_root: str, soes_dir: str) -> int:
    print("============================================================")
    print(" SOES 源码引用三方严格核验工具 (Three-way Quote Verifier)")
    print(f" 外部清单路径: {manifest_path}")
    print(f" 仓库根目录:   {repo_root}")
    print(f" SOES 源码目录: {soes_dir}")
    print("============================================================")

    if not os.path.isfile(manifest_path):
        print(f"[FATAL] 外部清单文件不存在: {manifest_path}")
        return 2

    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest_data = json.load(f)
    except Exception as e:
        print(f"[FATAL] 解析清单 JSON 失败: {e}")
        return 2

    if not isinstance(manifest_data, list):
        print(f"[FATAL] 清单根结构必须为 JSON Array")
        return 2

    seen_ids = set()
    failures = 0
    checked_count = 0

    print(f"【登记清单核验范围说明】：仅对清单中登记的 {len(manifest_data)} 处引用进行三方比对。")
    print("------------------------------------------------------------")

    for item in manifest_data:
        checked_count += 1
        qid = item.get("id")
        doc_rel = item.get("doc_path")
        source_rel = item.get("source_file")
        start_line = item.get("start_line")
        end_line = item.get("end_line")
        expected_snippet = item.get("expected_snippet")
        desc = item.get("description", "")

        # 1. 清单条目完整性与 ID 唯一性检查
        if not qid or not doc_rel or not source_rel or start_line is None or end_line is None or expected_snippet is None:
            print(f"[{qid or 'UNKNOWN'}] [FAIL] 清单字段缺失或非法")
            failures += 1
            continue

        if qid in seen_ids:
            print(f"[{qid}] [FAIL] 清单中存在重复的引用 ID")
            failures += 1
            continue
        seen_ids.add(qid)

        # 2. 从文档中提取引文 (Party 2)
        doc_full = os.path.join(repo_root, doc_rel)
        doc_ok, doc_lines, doc_err = extract_quote_from_doc(doc_full, qid)
        if not doc_ok:
            print(f"[{qid}] [FAIL] 文档引文提取失败: {doc_err}")
            failures += 1
            continue

        # 3. 从 SOES 源码中提取引文 (Party 3)
        src_full = os.path.join(soes_dir, source_rel)
        src_ok, src_lines, src_err = extract_quote_from_source(src_full, start_line, end_line)
        if not src_ok:
            print(f"[{qid}] [FAIL] 源码引文提取失败: {src_err}")
            failures += 1
            continue

        # 4. 三方逐行一致性严格核验 (Manifest vs Doc vs Source)
        manifest_vs_doc = (expected_snippet == doc_lines)
        manifest_vs_src = (expected_snippet == src_lines)

        if manifest_vs_doc and manifest_vs_src:
            print(f"  [PASS] {qid:28s} -> {source_rel}:{start_line}-{end_line} ({desc})")
        else:
            failures += 1
            print(f"  [FAIL] {qid:28s} -> 三方比对不一致！")
            if not manifest_vs_doc:
                print(f"         [差异: 清单 vs 文档]")
                print(f"           - 清单行数: {len(expected_snippet)} | 文档行数: {len(doc_lines)}")
                for i in range(max(len(expected_snippet), len(doc_lines))):
                    e_l = expected_snippet[i] if i < len(expected_snippet) else "<EOF>"
                    d_l = doc_lines[i] if i < len(doc_lines) else "<EOF>"
                    if e_l != d_l:
                        print(f"           Line +{i}: 清单: {repr(e_l)} vs 文档: {repr(d_l)}")
            if not manifest_vs_src:
                print(f"         [差异: 清单 vs 源码]")
                print(f"           - 清单行数: {len(expected_snippet)} | 源码行数: {len(src_lines)}")
                for i in range(max(len(expected_snippet), len(src_lines))):
                    e_l = expected_snippet[i] if i < len(expected_snippet) else "<EOF>"
                    s_l = src_lines[i] if i < len(src_lines) else "<EOF>"
                    if e_l != s_l:
                        line_no = start_line + i
                        print(f"           Line {line_no}: 清单: {repr(e_l)} vs 源码: {repr(s_l)}")

    print("------------------------------------------------------------")
    print(f" 三方核验统计: 检查 {checked_count} 条 | 通过: {checked_count - failures} 条 | 失败: {failures} 条")
    if failures == 0:
        print(">>> 全部三方比对 100% 吻合！清单、文档与 SOES 源码逐字一致！<<<")
        return 0
    else:
        print(">>> 存在比对不一致项，三方核验失败！<<<")
        return 1

def main():
    parser = argparse.ArgumentParser(description="SOES 源码引用三方严格核验工具")
    parser.add_argument(
        "--manifest",
        default="docs/quotes_manifest.json",
        help="外部清单 JSON 路径 (默认: docs/quotes_manifest.json)",
    )
    parser.add_argument(
        "--repo-root",
        default=".",
        help="仓库根目录 (默认: .)",
    )
    parser.add_argument(
        "--soes-dir",
        default="/tmp/soes_src/soes",
        help="SOES 源码本地路径 (默认: /tmp/soes_src/soes)",
    )
    args = parser.parse_args()
    ret = verify_three_way(args.manifest, args.repo_root, args.soes_dir)
    sys.exit(ret)

if __name__ == "__main__":
    main()
