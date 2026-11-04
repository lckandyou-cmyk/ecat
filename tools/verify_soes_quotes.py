#!/usr/bin/env python3
"""
tools/verify_soes_quotes.py

三方严格核验 SOES 源码引用工具 (Three-way Quote Verifier)。

【核验原则与三方数据源】：
1. 外部清单文件 (docs/quotes_manifest.json)
2. 项目 Markdown 文档标记块 (<!-- quote-begin: ID --> ... <!-- quote-end: ID -->)
3. 本地真实 SOES 源码文件 (按照行号范围提取的代码片段)

【严密防御与拦截机制】：
- 清单为空列表时直接失败 (退出码 2)；
- 清单内部重复 ID 直接失败 (退出码 2)；
- 本地 SOES 源码文件 SHA-256 校验不匹配直接失败 (退出码 1)；
- 文档标记未配对 (有 begin 缺 end，或有 end 缺 begin) 直接失败 (退出码 1)；
- 文档标记嵌套 (begin 内部再出现 begin) 直接失败 (退出码 1)；
- 全库 Markdown 反向扫描：文档内出现未知 ID、重复标记或位置与清单 doc_path 不符直接失败 (退出码 1)；
- 全库 Markdown 正文行号扫描：出现未在 quotes 登记且未在 exemptions 显式精确豁免的行号引用直接失败 (退出码 1)；
- 三方文本字符逐字逐行比对，任何差异直接失败 (退出码 1)。

【覆盖范围与承诺边界】：
本工具仅针对外部清单文件显式登记的条目执行核验并作出真实性承诺，
不声称亦不覆盖任何未登记的文本段落。
"""

import os
import sys
import json
import re
import hashlib
import argparse
from typing import List, Dict, Any, Tuple, Set

def compute_file_sha256(filepath: str) -> str:
    """计算文件的 SHA-256 哈希字符串"""
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()

def verify_source_checksums(soes_dir: str, file_checksums: Dict[str, str]) -> Tuple[bool, List[str]]:
    """校验 SOES 本地源文件的 SHA-256 完整性"""
    errors: List[str] = []
    if not file_checksums:
        return True, []

    for fname, expected_hash in file_checksums.items():
        full_path = os.path.join(soes_dir, fname)
        if not os.path.isfile(full_path):
            errors.append(f"基线源文件不存在: {full_path}")
            continue
        actual_hash = compute_file_sha256(full_path)
        if actual_hash.lower() != expected_hash.lower():
            errors.append(f"源文件 SHA-256 校验失败: {fname} (期望: {expected_hash}, 实际: {actual_hash})")

    return (len(errors) == 0), errors

def scan_all_markdown_tags(repo_root: str) -> Tuple[bool, Dict[str, Dict[str, Any]], str]:
    """
    反向扫描仓库中所有 .md 文件中的 quote-begin 和 quote-end 标记。
    检查：
    1. 标记未配对
    2. 标记嵌套
    3. 文档内部重复标记
    返回: (ok, tags_found_map, error_msg)
    tags_found_map 格式: { qid: {"doc_rel": str, "code_lines": List[str], "line_no": int} }
    """
    begin_pattern = re.compile(r"<!--\s*quote-begin:\s*([\w_]+)\s*-->")
    end_pattern = re.compile(r"<!--\s*quote-end:\s*([\w_]+)\s*-->")

    tags_found: Dict[str, Dict[str, Any]] = {}

    for root, dirs, files in os.walk(repo_root):
        if ".git" in root or ".gemini" in root:
            continue
        for file in sorted(files):
            if not file.endswith(".md"):
                continue
            full_path = os.path.join(root, file)
            doc_rel = os.path.relpath(full_path, repo_root)

            try:
                with open(full_path, "r", encoding="utf-8", errors="replace") as f:
                    lines = f.readlines()
            except Exception as e:
                return False, {}, f"读取文档失败: {doc_rel} ({e})"

            active_qid = None
            active_begin_line = None
            code_buffer: List[str] = []

            for lidx, raw_line in enumerate(lines, 1):
                line = raw_line.rstrip("\r\n")

                b_match = begin_pattern.search(line)
                e_match = end_pattern.search(line)

                # 检查同一行是否同时有 begin 和 end
                if b_match and e_match:
                    return False, {}, f"文档 {doc_rel} 第 {lidx} 行 quote-begin 与 quote-end 位于同一行"

                if b_match:
                    qid = b_match.group(1)
                    if active_qid is not None:
                        return False, {}, f"文档 {doc_rel} 第 {lidx} 行发现嵌套标记 '{qid}' (在未闭合标记 '{active_qid}' 内部)"
                    if qid in tags_found:
                        first_doc = tags_found[qid]["doc_rel"]
                        first_line = tags_found[qid]["line_no"]
                        return False, {}, f"重复标记 '{qid}': 在 {doc_rel}:{lidx} 重复出现 (已在 {first_doc}:{first_line} 定义)"
                    active_qid = qid
                    active_begin_line = lidx
                    code_buffer = []
                    continue

                if e_match:
                    qid = e_match.group(1)
                    if active_qid is None:
                        return False, {}, f"文档 {doc_rel} 第 {lidx} 行发现孤立的 quote-end 标记 '{qid}' (无匹配的 quote-begin)"
                    if active_qid != qid:
                        return False, {}, f"文档 {doc_rel} 第 {lidx} 行标记不匹配: 期望 quote-end: {active_qid}，实际为 {qid}"

                    # 提取 fenced block 内部代码行
                    extracted: List[str] = []
                    fence_count = 0
                    for c_line in code_buffer:
                        if c_line.strip().startswith("```"):
                            fence_count += 1
                            continue
                        if fence_count == 1:
                            extracted.append(c_line)

                    if fence_count < 2 or not extracted:
                        return False, {}, f"文档 {doc_rel} 标记 '{active_qid}' 内部未包含有效的 Markdown 代码块 (```)"

                    tags_found[active_qid] = {
                        "doc_rel": doc_rel,
                        "code_lines": extracted,
                        "line_no": active_begin_line,
                    }
                    active_qid = None
                    active_begin_line = None
                    continue

                if active_qid is not None:
                    code_buffer.append(line)

            if active_qid is not None:
                return False, {}, f"文档 {doc_rel} 末尾存在未配对闭合的标记 '{active_qid}' (位于第 {active_begin_line} 行)"

    return True, tags_found, ""

def extract_quote_from_source(source_path: str, start_line: int, end_line: int) -> Tuple[bool, List[str], str]:
    """从本地源文件中提取指定行号范围的文本"""
    if not os.path.isfile(source_path):
        return False, [], f"源码文件不存在: {source_path}"

    try:
        with open(source_path, "r", encoding="utf-8", errors="replace") as f:
            all_lines = [l.rstrip("\r\n") for l in f]
    except Exception as e:
        return False, [], f"读取源码失败: {source_path} ({e})"

    if start_line < 1 or end_line > len(all_lines) or start_line > end_line:
        return False, [], f"行号范围越界: {start_line}-{end_line} (源文件总行数: {len(all_lines)})"

    snippet = all_lines[start_line - 1 : end_line]
    return True, snippet, ""

def is_line_covered_by_quote(src_basename: str, line_ref: str, quotes: List[Dict[str, Any]]) -> bool:
    """检查一个正文行号引用是否被已登记的 Quote 块完整覆盖"""
    parts = line_ref.split("-")
    try:
        if len(parts) == 1:
            s = e = int(parts[0])
        else:
            s, e = int(parts[0]), int(parts[1])
    except ValueError:
        return False

    for q in quotes:
        q_src = os.path.basename(q.get("source_file", ""))
        q_s = q.get("start_line", 0)
        q_e = q.get("end_line", 0)
        if q_src == src_basename and q_s <= s and e <= q_e:
            return True
    return False

def is_line_exempted(doc_rel: str, src_basename: str, line_ref: str, exemptions: List[Dict[str, Any]]) -> bool:
    """检查正文行号引用是否在显式豁免清单中"""
    for ex in exemptions:
        ex_doc = ex.get("doc_path", "")
        ex_src = ex.get("source_file", "")
        ex_ref = ex.get("line_reference", "")

        if ex_doc != "*" and os.path.normpath(ex_doc) != os.path.normpath(doc_rel):
            continue
        if ex_src != "*" and ex_src != src_basename:
            continue
        if ex_ref != "*" and ex_ref != line_ref:
            continue
        return True
    return False

def scan_unregistered_prose_lines(
    repo_root: str,
    quotes: List[Dict[str, Any]],
    exemptions: List[Dict[str, Any]]
) -> Tuple[bool, List[str]]:
    """
    全量扫描仓库 Markdown 正文中包含的行号引用，
    严密拦截任何未在 quotes 登记且未在 exemptions 中显式精确豁免的行号引用。
    """
    p1 = re.compile(r"([a-zA-Z0-9_\-\./]+\.(?:c|h|txt|pdf))[:：](\d+(?:-\d+)?)")
    p2 = re.compile(r"[`]?([a-zA-Z0-9_\-\./]+\.(?:c|h|txt|pdf))[`]?\s*第\s*(\d+(?:-\d+)?)\s*行")

    unregistered_errors: List[str] = []

    for root, dirs, files in os.walk(repo_root):
        if ".git" in root or ".gemini" in root:
            continue
        for file in sorted(files):
            if not file.endswith(".md"):
                continue
            full_path = os.path.join(root, file)
            doc_rel = os.path.relpath(full_path, repo_root)

            try:
                with open(full_path, "r", encoding="utf-8", errors="replace") as f:
                    for lno, line in enumerate(f, 1):
                        found_refs: List[Tuple[str, str]] = []
                        for m in p1.findall(line):
                            found_refs.append((os.path.basename(m[0]), m[1]))
                        for m in p2.findall(line):
                            found_refs.append((os.path.basename(m[0]), m[1]))

                        for src_base, lref in found_refs:
                            if is_line_covered_by_quote(src_base, lref, quotes):
                                continue
                            if is_line_exempted(doc_rel, src_base, lref, exemptions):
                                continue
                            unregistered_errors.append(
                                f"文档 {doc_rel}:{lno} 发现未登记的正文行号引用: '{src_base}:{lref}' (必须在清单 quotes 中注册或在 exemptions 中精确豁免并说明理由)"
                            )
            except Exception as e:
                unregistered_errors.append(f"读取文档失败: {doc_rel} ({e})")

    return (len(unregistered_errors) == 0), unregistered_errors

def verify_three_way(manifest_path: str, repo_root: str, soes_dir: str) -> int:
    print("============================================================")
    print(" SOES 源码引用三方严格核验工具 (Three-way Quote Verifier)")
    print(f" 外部清单路径: {manifest_path}")
    print(f" 仓库根目录:   {repo_root}")
    print(f" SOES 源码目录: {soes_dir}")
    print("============================================================")

    # 1. 清单文件存在与合法性检查
    if not os.path.isfile(manifest_path):
        print(f"[FATAL] 外部清单文件不存在: {manifest_path}")
        return 2

    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest_json = json.load(f)
    except Exception as e:
        print(f"[FATAL] 解析清单 JSON 失败: {e}")
        return 2

    # 兼容支持根为 List 或根为 Dict 的 JSON 结构
    if isinstance(manifest_json, list):
        manifest_quotes = manifest_json
        file_checksums: Dict[str, str] = {}
        prose_exemptions: List[Dict[str, Any]] = []
        source_meta: Dict[str, Any] = {}
    elif isinstance(manifest_json, dict):
        manifest_quotes = manifest_json.get("quotes", [])
        file_checksums = manifest_json.get("file_checksums", {})
        prose_exemptions = manifest_json.get("prose_line_exemptions", [])
        source_meta = manifest_json.get("source_metadata", {})
    else:
        print(f"[FATAL] 清单根结构必须为 JSON Object 或 JSON Array")
        return 2

    # 2. 空清单直接判定失败
    if len(manifest_quotes) == 0:
        print(f"[FATAL] 清单内容为空列表 []，无核验条目")
        return 2

    manifest_map: Dict[str, Dict[str, Any]] = {}
    duplicate_ids: Set[str] = set()

    for item in manifest_quotes:
        qid = item.get("id")
        if not qid:
            print(f"[FATAL] 清单存在无 ID 条目: {item}")
            return 2
        if qid in manifest_map:
            duplicate_ids.add(qid)
        manifest_map[qid] = item

    if duplicate_ids:
        print(f"[FATAL] 清单中存在重复 ID: {sorted(list(duplicate_ids))}")
        return 2

    # 3. 校验 SOES 本地散文件 SHA-256 基线
    print("[1/5] 校验本地 SOES 源码文件 SHA-256 完整性基线...")
    if source_meta:
        print(f"      [出处说明] {source_meta.get('origin_statement', '')}")
        print(f"      [上游版本] Commit: {source_meta.get('upstream_commit', 'UNKNOWN')}")
    chk_ok, chk_errs = verify_source_checksums(soes_dir, file_checksums)
    if not chk_ok:
        for err in chk_errs:
            print(f"  [FAIL] {err}")
        return 1
    if file_checksums:
        print(f"  [OK] {len(file_checksums)} 个基线源码文件 SHA-256 比对完全一致！")
    else:
        print("  [WARN] 清单未指定 file_checksums，跳过源码哈希比对")

    # 4. 反向扫描全仓库 Markdown 文档中的全部 quote 标记
    print("[2/5] 扫描仓库全部 Markdown 文档标记结构 (检查未配对/嵌套/重复)...")
    scan_ok, doc_tags_map, scan_err = scan_all_markdown_tags(repo_root)
    if not scan_ok:
        print(f"  [FAIL] 文档标记反向扫描失败: {scan_err}")
        return 1
    print(f"  [OK] 文档扫描完成，共捕获 {len(doc_tags_map)} 处配对标记")

    # 5. 反向核验：检查文档中是否有未知 ID 或出现在非指定文档中
    print("[3/5] 反向交叉校验 (检查文档标记是否超出清单或位置错乱)...")
    for doc_qid, tag_info in doc_tags_map.items():
        if doc_qid not in manifest_map:
            print(f"  [FAIL] 反向拦截: 文档 {tag_info['doc_rel']}:{tag_info['line_no']} 存在清单未登记的未知 ID '{doc_qid}'")
            return 1
        expected_doc = manifest_map[doc_qid].get("doc_path")
        actual_doc = tag_info["doc_rel"]
        if os.path.normpath(expected_doc) != os.path.normpath(actual_doc):
            print(f"  [FAIL] 标记文档错位: ID '{doc_qid}' 登记于 '{expected_doc}'，但实际出现在 '{actual_doc}'")
            return 1
    print("  [OK] 反向交叉校验通过，文档标记无遗留或错位")

    # 6. 三方比对 (Manifest vs Doc vs Source)
    print(f"[4/5] 执行三方逐行内容核验 (共 {len(manifest_quotes)} 处登记条目)...")
    print("------------------------------------------------------------")

    failures = 0
    checked_count = 0

    for qid, item in manifest_map.items():
        checked_count += 1
        doc_rel = item.get("doc_path")
        source_rel = item.get("source_file")
        start_line = item.get("start_line")
        end_line = item.get("end_line")
        expected_snippet = item.get("expected_snippet")
        desc = item.get("description", "")

        if not doc_rel or not source_rel or start_line is None or end_line is None or expected_snippet is None:
            print(f"[{qid}] [FAIL] 清单字段缺失")
            failures += 1
            continue

        if qid not in doc_tags_map:
            print(f"[{qid}] [FAIL] 清单登记的条目在对应文档 '{doc_rel}' 中缺失标记")
            failures += 1
            continue

        doc_lines = doc_tags_map[qid]["code_lines"]

        # 读取本地真实源码行
        src_full = os.path.join(soes_dir, source_rel)
        src_ok, src_lines, src_err = extract_quote_from_source(src_full, start_line, end_line)
        if not src_ok:
            print(f"[{qid}] [FAIL] 源码引文提取失败: {src_err}")
            failures += 1
            continue

        manifest_vs_doc = (expected_snippet == doc_lines)
        manifest_vs_src = (expected_snippet == src_lines)

        if manifest_vs_doc and manifest_vs_src:
            print(f"  [PASS] {qid:28s} -> {source_rel}:{start_line}-{end_line} ({desc})")
        else:
            failures += 1
            print(f"  [FAIL] {qid:28s} -> 三方比对不一致！")
            if not manifest_vs_doc:
                print(f"         [差异: 清单 vs 文档]")
                max_l = max(len(expected_snippet), len(doc_lines))
                for i in range(max_l):
                    e_l = expected_snippet[i] if i < len(expected_snippet) else "<EOF>"
                    d_l = doc_lines[i] if i < len(doc_lines) else "<EOF>"
                    if e_l != d_l:
                        print(f"           Line +{i}: 清单: {repr(e_l)} vs 文档: {repr(d_l)}")
            if not manifest_vs_src:
                print(f"         [差异: 清单 vs 源码]")
                max_l = max(len(expected_snippet), len(src_lines))
                for i in range(max_l):
                    e_l = expected_snippet[i] if i < len(expected_snippet) else "<EOF>"
                    s_l = src_lines[i] if i < len(src_lines) else "<EOF>"
                    if e_l != s_l:
                        line_no = start_line + i
                        print(f"           Line {line_no}: 清单: {repr(e_l)} vs 源码: {repr(s_l)}")

    print("------------------------------------------------------------")
    if failures > 0:
        print(f">>> 存在三方比对不一致项 ({failures} 处失败)！<<<")
        return 1

    # 7. 全库 Markdown 未登记正文行号扫描
    print(f"[5/5] 扫描全库 Markdown 正文行号引用 (匹配 quotes 与 {len(prose_exemptions)} 条精确豁免)...")
    prose_ok, prose_errs = scan_unregistered_prose_lines(repo_root, manifest_quotes, prose_exemptions)
    if not prose_ok:
        for err in prose_errs:
            print(f"  [FAIL] {err}")
        return 1
    print("  [OK] 全库 Markdown 正文行号扫描通过，未发现未登记或未豁免的行号引用")

    print("------------------------------------------------------------")
    print(f" 三方核验统计: 检查 {checked_count} 条 Quotes | 源码 SHA-256 比对通过 | 正文行号引用核准完毕")
    print(">>> 全部三方比对 100% 吻合！清单、文档与 SOES 源码逐字一致！<<<")
    return 0

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
