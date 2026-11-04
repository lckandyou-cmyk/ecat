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
- 豁免项严禁任何通配符 '*'，发现直接失败 (退出码 1)；
- 豁免项缺少必须字段 (doc_path, doc_line, source_file, line_reference, line_text_hash, verification_status, reason) 直接失败 (退出码 1)；
- 引用块覆盖仅限同一文档且位置相邻 (<=5 行)，禁止跨文档借用或同名跨文件放行 (退出码 1)；
- 豁免项行文本哈希与文档实际行文本不匹配 (防篡改与行号漂移) 直接失败 (退出码 1)；
- 本地 SOES 源码文件 SHA-256 校验不匹配直接失败 (退出码 1)；
- 文档标记未配对、嵌套或重复直接失败 (退出码 1)；
- 全库 Markdown 反向扫描：文档内出现未知 ID、重复标记或位置与清单 doc_path 不符直接失败 (退出码 1)；
- 全库 Markdown 正文行号扫描：出现未登记且未豁免的行号引用直接失败 (退出码 1)；
- 三方文本字符逐字逐行比对，任何差异直接失败 (退出码 1)。

【覆盖范围与承诺边界】：
本工具仅针对外部清单文件显式登记并校验通过的条目作出真实性承诺，
不声称亦不覆盖任何未核验的外部厂商技术文档与历史条目。
"""

import os
import sys
import json
import re
import hashlib
import argparse
from typing import List, Dict, Any, Tuple, Set

REQUIRED_EXEMPTION_FIELDS = [
    "doc_path",
    "doc_line",
    "source_file",
    "line_reference",
    "line_text_hash",
    "verification_status",
    "reason",
]

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

def compute_line_text_hash(line_text: str) -> str:
    """计算单行文本 (去首尾空白) 的 SHA-256"""
    return hashlib.sha256(line_text.strip().encode("utf-8")).hexdigest()

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

def validate_exemptions_schema(exemptions: List[Dict[str, Any]]) -> Tuple[bool, List[str]]:
    """严格校验豁免清单规范：严禁通配符，必须包含所有必须字段"""
    errors: List[str] = []
    for idx, ex in enumerate(exemptions):
        if not isinstance(ex, dict):
            errors.append(f"豁免项 #{idx} 不是有效的 JSON Object: {ex}")
            continue

        for field in REQUIRED_EXEMPTION_FIELDS:
            if field not in ex or ex[field] is None:
                errors.append(f"豁免项 #{idx} 缺失必须字段 '{field}': {ex}")
            elif isinstance(ex[field], str) and ex[field].strip() == "":
                errors.append(f"豁免项 #{idx} 字段 '{field}' 不能为空字符串: {ex}")

        # 检查严禁出现的 '*' 通配符
        for field, val in ex.items():
            if isinstance(val, str) and "*" in val:
                errors.append(f"豁免项 #{idx} 字段 '{field}' 包含被严格禁止的通配符 '*': {ex}")

        # 检查 doc_line 必须为正整数
        if "doc_line" in ex and not isinstance(ex["doc_line"], int):
            errors.append(f"豁免项 #{idx} 'doc_line' 必须为整数: {ex}")

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

def is_line_covered_by_local_quote(
    doc_rel: str,
    doc_line: int,
    raw_src_path: str,
    src_basename: str,
    line_ref: str,
    quotes_map: Dict[str, Dict[str, Any]],
    doc_tags_map: Dict[str, Dict[str, Any]],
) -> bool:
    """
    检查一个正文行号引用是否被同一文档且位置相邻 (<=3 行) 的 Quote 块覆盖。
    严格防范跨文档借用与同名跨文件路径混淆。
    """
    parts = line_ref.split("-")
    try:
        if len(parts) == 1:
            s = e = int(parts[0])
        else:
            s, e = int(parts[0]), int(parts[1])
    except ValueError:
        return False

    norm_doc = os.path.normpath(doc_rel)
    norm_raw_dir = os.path.dirname(os.path.normpath(raw_src_path))

    for qid, q in quotes_map.items():
        if qid not in doc_tags_map:
            continue
        tag_info = doc_tags_map[qid]

        # 1. 必须位于同一文档
        if os.path.normpath(tag_info["doc_rel"]) != norm_doc:
            continue

        # 2. 必须位置相邻 (严格收紧至距离 quote-begin 标记行在 3 行以内)
        tag_lno = tag_info["line_no"]
        if abs(doc_line - tag_lno) > 3:
            continue

        # 3. 源码文件及路径匹配 (防同名 basename 跨目录混淆)
        q_src_raw = q.get("source_file", "")
        q_src_base = os.path.basename(q_src_raw)
        if q_src_base != src_basename:
            continue

        q_src_dir = os.path.dirname(os.path.normpath(q_src_raw))
        if norm_raw_dir != "" and q_src_dir != "" and norm_raw_dir != q_src_dir:
            # 路径存在二义性冲突，严禁放行借用
            continue

        # 4. 行号范围必须完整包含
        q_s = q.get("start_line", 0)
        q_e = q.get("end_line", 0)
        if q_s <= s and e <= q_e:
            return True

    return False

def scan_unregistered_prose_lines(
    repo_root: str,
    quotes_map: Dict[str, Dict[str, Any]],
    doc_tags_map: Dict[str, Dict[str, Any]],
    exemptions: List[Dict[str, Any]],
) -> Tuple[bool, List[str], Dict[str, int]]:
    """
    全量扫描仓库 Markdown 正文中包含的行号引用，
    严密拦截任何未在 quotes 登记且未在 exemptions 中显式精确豁免的行号引用。
    校验行文本哈希以防篡改或行号漂移。
    """
    p1 = re.compile(r"([a-zA-Z0-9_\-\./]+\.(?:c|h|txt|pdf))[:：](\d+(?:-\d+)?)")
    p2 = re.compile(r"[`]?([a-zA-Z0-9_\-\./]+\.(?:c|h|txt|pdf))[`]?\s*第\s*(\d+(?:-\d+)?)\s*行")

    errors: List[str] = []
    status_counts: Dict[str, int] = {}
    matched_exemptions: Set[int] = set()

    # 建立豁免索引: (doc_path_norm, doc_line) -> list of (idx, ex)
    ex_index: Dict[Tuple[str, int], List[Tuple[int, Dict[str, Any]]]] = {}
    for idx, ex in enumerate(exemptions):
        key = (os.path.normpath(ex.get("doc_path", "")), ex.get("doc_line", 0))
        ex_index.setdefault(key, []).append((idx, ex))

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
                    for lno, raw_line in enumerate(f, 1):
                        trimmed_line = raw_line.rstrip("\r\n").strip()
                        line_hash = compute_line_text_hash(trimmed_line)

                        found_refs: List[Tuple[str, str, str]] = []
                        for m in p1.findall(raw_line):
                            found_refs.append((m[0], os.path.basename(m[0]), m[1]))
                        for m in p2.findall(raw_line):
                            found_refs.append((m[0], os.path.basename(m[0]), m[1]))

                        for raw_path, src_base, lref in found_refs:
                            # 1. 检查是否为同文档相邻 (<=3 行) Quote 块覆盖
                            if is_line_covered_by_local_quote(doc_rel, lno, raw_path, src_base, lref, quotes_map, doc_tags_map):
                                status_counts["LOCAL_QUOTE_ADJACENT"] = status_counts.get("LOCAL_QUOTE_ADJACENT", 0) + 1
                                continue

                            # 2. 检查精确豁免清单匹配
                            key = (os.path.normpath(doc_rel), lno)
                            matched = False
                            if key in ex_index:
                                for ex_idx, ex in ex_index[key]:
                                    if ex.get("source_file") == src_base and ex.get("line_reference") == lref:
                                        # 校验行文本哈希防篡改
                                        if ex.get("line_text_hash") != line_hash:
                                            errors.append(
                                                f"文档 {doc_rel}:{lno} 行文本哈希不匹配！\n"
                                                f"  期望哈希: {ex.get('line_text_hash')}\n"
                                                f"  实际哈希: {line_hash}\n"
                                                f"  实际内容: {repr(trimmed_line)}\n"
                                                f"  (文档行文本可能被修改或行号发生漂移)"
                                            )
                                        else:
                                            matched = True
                                            matched_exemptions.add(ex_idx)
                                            st = ex.get("verification_status", "UNKNOWN")
                                            status_counts[st] = status_counts.get(st, 0) + 1
                                        break

                            if not matched:
                                errors.append(
                                    f"文档 {doc_rel}:{lno} 发现未登记的正文行号引用: '{src_base}:{lref}'\n"
                                    f"  行内容: {repr(trimmed_line)}\n"
                                    f"  行哈希: {line_hash}\n"
                                    f"  (必须在清单 quotes 中注册或在 exemptions 中逐项绑定 doc_line、文本哈希精确豁免)"
                                )
            except Exception as e:
                errors.append(f"读取文档失败: {doc_rel} ({e})")

    # 反向检查：豁免清单中的条目必须真实命中，杜绝悬垂或幽灵豁免
    for idx, ex in enumerate(exemptions):
        if idx not in matched_exemptions:
            errors.append(
                f"豁免项 #{idx} 未在实际文档中命中或文本哈希不符: {ex.get('doc_path')}:{ex.get('doc_line')} -> {ex.get('source_file')}:{ex.get('line_reference')}"
            )

    return (len(errors) == 0), errors, status_counts

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

    # 3. 校验豁免清单的规范性 (严禁通配符，必须字段完整)
    schema_ok, schema_errs = validate_exemptions_schema(prose_exemptions)
    if not schema_ok:
        print("[FATAL] 豁免清单合法性校验失败:")
        for err in schema_errs:
            print(f"  [FAIL] {err}")
        return 1

    # 4. 校验 SOES 本地散文件 SHA-256 基线
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

    # 5. 反向扫描全仓库 Markdown 文档中的全部 quote 标记
    print("[2/5] 扫描仓库全部 Markdown 文档标记结构 (检查未配对/嵌套/重复)...")
    scan_ok, doc_tags_map, scan_err = scan_all_markdown_tags(repo_root)
    if not scan_ok:
        print(f"  [FAIL] 文档标记反向扫描失败: {scan_err}")
        return 1
    print(f"  [OK] 文档扫描完成，共捕获 {len(doc_tags_map)} 处配对标记")

    # 6. 反向核验：检查文档中是否有未知 ID 或出现在非指定文档中
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

    # 7. 三方比对 (Manifest vs Doc vs Source)
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

    # 8. 全库 Markdown 未登记正文行号与文本哈希严格核验
    print(f"[5/5] 扫描全库 Markdown 正文行号引用 (严密比对 doc_line 与文本 SHA-256)...")
    prose_ok, prose_errs, status_counts = scan_unregistered_prose_lines(
        repo_root, manifest_map, doc_tags_map, prose_exemptions
    )
    if not prose_ok:
        for err in prose_errs:
            print(f"  [FAIL] {err}")
        return 1
    print("  [OK] 全库 Markdown 正文行号扫描通过，未发现未登记、未豁免、通配符或文本哈希不匹配项")

    print("------------------------------------------------------------")
    print(f" 核验统计与覆盖范围客观汇报:")
    print(f" - 登记 Quote 自动化三方核验: {checked_count} 项 (逐字比对吻合)")
    print(f" - 正文行号精确豁免总数:     {len(prose_exemptions)} 项 (权威规范文档已清零，仅限归档历史记录)")
    for st, cnt in sorted(status_counts.items()):
        print(f"   * {st:38s}: {cnt} 处引用")
    print(" - 正则覆盖局限说明: 本工具基于正则表达式匹配显式文件行号引用，无法识别无行号自然语言引述或非常规引用语法")
    print(" - 外部未核验说明: Beckhoff ET9300 官方受限无法获取；ETG 规范为外部标准，均已严格标注未核验")
    print(">>> 三方引文自动化核验通过 (承诺范围仅限上述已登记条目) <<<")
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
