#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/gen_pdf_cite_table.py
------------------------------------------------------------
根据 docs/pdf_quotes_manifest.json 自动生成/核验
docs/architecture/soes-integration-options.md 中的
<!-- pdf-table-begin --> 与 <!-- pdf-table-end --> 引文盘点表格。

使用方式:
  python3 tools/gen_pdf_cite_table.py --check
    以非侵入方式比对文档现有表格是否与清单生成结果完全一致 (吻合返回 0，不吻合返回 1)
  python3 tools/gen_pdf_cite_table.py --update
    生成最新表格并回写至文档，同时刷新清单中的 doc_line 与 line_text_hash。
    注意：--update 会回写 manifest 的 doc_line 和 line_text_hash，防漂移依赖 --check 与 PDF 校验。
"""

import os
import sys
import json
import hashlib
import argparse
from typing import List, Dict, Any, Tuple

BEGIN_MARKER = "<!-- pdf-table-begin -->"
END_MARKER = "<!-- pdf-table-end -->"

TABLE_HEADER = [
    "| 官方章节 (Section) / 寄存器名称 | PDF 物理页 | 底部印刷页 | 英文逐字原文 (English Verbatim Text) | 自动化核验状态与依据 |",
    "| :--- | :--- | :--- | :--- | :--- |"
]

def compute_hash(text: str) -> str:
    return hashlib.sha256(text.strip().encode("utf-8")).hexdigest()

def build_table_rows(manifest_data: Dict[str, Any]) -> Tuple[List[str], List[Dict[str, Any]]]:
    """生成表格正文各行，并记录各 quote 对应生成的行索引"""
    rows: List[str] = []
    quote_row_indices: List[Dict[str, Any]] = []

    # 1. 已登记且验证的 Quote 列表
    quotes = manifest_data.get("quotes", [])
    for q in quotes:
        t = q.get("table_row", {})
        sec_cell = t.get("section_cell", f"Section {q.get('section_number', '')}")
        pdf_page = t.get("pdf_page_cell", f"Page {q.get('pdf_page', '')}")
        prn_page = t.get("printed_page_cell", f"Page {q.get('printed_page', '')}")
        snippet_cell = t.get("verbatim_cell", f"`{q.get('snippet', '')}`")
        status_cell = t.get("status_cell", "**【PDF 工具逐页与章节归属核验通过】**")

        row_str = f"| {sec_cell} | {pdf_page} | {prn_page} | {snippet_cell} | {status_cell} |"
        quote_row_indices.append({
            "id": q.get("id"),
            "row_index": len(rows),
            "row_text": row_str
        })
        rows.append(row_str)

    # 2. 未核实条目列表
    unverified = manifest_data.get("unverified_citations", [])
    for u in unverified:
        sec_cell = u.get("section_cell", "")
        pdf_page = u.get("pdf_page_cell", "")
        prn_page = u.get("printed_page_cell", "")
        snippet_cell = u.get("verbatim_cell", "")
        status_cell = u.get("status_cell", "未核实")
        row_str = f"| {sec_cell} | {pdf_page} | {prn_page} | {snippet_cell} | {status_cell} |"
        rows.append(row_str)

    return rows, quote_row_indices

def generate_full_block(manifest_data: Dict[str, Any]) -> Tuple[str, List[Dict[str, Any]]]:
    table_rows, quote_row_indices = build_table_rows(manifest_data)
    all_lines = [BEGIN_MARKER] + TABLE_HEADER + table_rows + [END_MARKER]
    return "\n".join(all_lines), quote_row_indices

def main():
    parser = argparse.ArgumentParser(description="PDF 引文盘点表生成与校验工具")
    parser.add_argument("--manifest", default="docs/pdf_quotes_manifest.json", help="PDF 清单文件路径")
    parser.add_argument("--doc", default="docs/architecture/soes-integration-options.md", help="文档目标路径")
    parser.add_argument("--check", action="store_true", help="比对文档中表格与清单生成内容是否一致")
    parser.add_argument("--update", action="store_true", help="回写文档并更新清单行号与哈希（注意：--update 会回写 manifest 的 doc_line 和 line_text_hash，防漂移依赖 --check 与 PDF 校验）")
    args = parser.parse_args()

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    manifest_path = os.path.join(repo_root, args.manifest) if not os.path.isabs(args.manifest) else args.manifest
    doc_path = os.path.join(repo_root, args.doc) if not os.path.isabs(args.doc) else args.doc

    if not os.path.isfile(manifest_path):
        print(f"[ERROR] 清单文件未找到: {manifest_path}", file=sys.stderr)
        return 1
    if not os.path.isfile(doc_path):
        print(f"[ERROR] 文档文件未找到: {doc_path}", file=sys.stderr)
        return 1

    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest_data = json.load(f)

    with open(doc_path, "r", encoding="utf-8") as f:
        doc_content = f.read()

    begin_idx = doc_content.find(BEGIN_MARKER)
    end_idx = doc_content.find(END_MARKER)

    if begin_idx == -1 or end_idx == -1 or begin_idx >= end_idx:
        print(f"[ERROR] 文档中未找到合法配对的 {BEGIN_MARKER} 和 {END_MARKER}", file=sys.stderr)
        return 1

    generated_block, quote_row_indices = generate_full_block(manifest_data)
    existing_block = doc_content[begin_idx : end_idx + len(END_MARKER)].strip()

    if args.check:
        if existing_block != generated_block.strip():
            print("[FAIL] PDF 引文表格与清单生成结果不一致！", file=sys.stderr)
            print("--- 文档当前内容 ---", file=sys.stderr)
            print(existing_block, file=sys.stderr)
            print("--- 期望生成内容 ---", file=sys.stderr)
            print(generated_block, file=sys.stderr)
            return 1
        print("  [OK] PDF 引文盘点表格与清单自动生成结果完全一致。")
        return 0

    if args.update:
        # 回写文档
        new_doc_content = doc_content[:begin_idx] + generated_block + doc_content[end_idx + len(END_MARKER):]
        with open(doc_path, "w", encoding="utf-8") as f:
            f.write(new_doc_content)

        # 计算并更新 manifest 中的 doc_line 与 line_text_hash
        # 首先重新读取文档各行行号
        with open(doc_path, "r", encoding="utf-8") as f:
            lines = f.readlines()

        # 定位 BEGIN_MARKER 行号 (1-based)
        table_start_line = None
        for lno, line in enumerate(lines, 1):
            if BEGIN_MARKER in line:
                table_start_line = lno
                break

        assert table_start_line is not None

        # 表头占 3 行 (BEGIN_MARKER 为 1 行, TABLE_HEADER 占 2 行)
        data_first_line = table_start_line + 1 + len(TABLE_HEADER)

        quotes = manifest_data.get("quotes", [])
        for item in quote_row_indices:
            qid = item["id"]
            row_idx = item["row_index"]
            actual_lno = data_first_line + row_idx
            actual_text = lines[actual_lno - 1].rstrip("\r\n").strip()
            actual_hash = compute_hash(actual_text)

            for q in quotes:
                if q.get("id") == qid:
                    q["doc_line"] = actual_lno
                    q["line_text_hash"] = actual_hash
                    break

        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(manifest_data, f, indent=2, ensure_ascii=False)
            f.write("\n")

        print(f"[OK] 成功更新文档表格并同步清单 doc_line 与 line_text_hash: {manifest_path}")
        return 0

    # 默认输出生成的内容
    print(generated_block)
    return 0

if __name__ == "__main__":
    sys.exit(main())
