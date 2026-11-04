#!/usr/bin/env python3
"""
tools/verify_pdf_quotes.py

厂商 PDF 数据手册引文严格核验工具 (Vendor PDF Quote Verifier)。

【核验原则与防御机制】：
1. 外部清单文件登记 PDF 文件路径、SHA-256 哈希基线及引用条目；
2. 逐条包含：id, doc_path, doc_line, pdf_page, section_number, section_title, snippet；
3. 使用 pdftotext 按物理页码精准提取单页文本并缓存；
4. 校验 snippet 必须作为逐字子串出现在指定物理页中；
5. 【严格跨页章节归属核验】：
   解析页面中的章节标题与表格标题定义；
   若当前页为跨页延续表格（如 12.14.34 在 252 页定义，延续至 253 页），
   工具自动向前回溯定位所属前序章节，核验章节号与章节标题；
   【严禁把错误章节通过片段匹配放行】（若章节号或标题不符直接拦截失败）；
6. 严格防篡改与完整性校验：PDF 文件 SHA-256 哈希必须完全吻合。
"""

import os
import sys
import json
import re
import hashlib
import argparse
import subprocess
import shutil
from typing import List, Dict, Any, Tuple, Optional

REQUIRED_QUOTE_FIELDS = [
    "id",
    "doc_path",
    "doc_line",
    "pdf_page",
    "section_number",
    "section_title",
    "snippet",
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

def normalize_whitespace(text: str) -> str:
    """折叠连续空白字符为单个空格，并去除首尾空白"""
    return re.sub(r"\s+", " ", text).strip()

class PdfExtractor:
    """PDF 页面文本提取器，封装 pdftotext 命令并带内存缓存"""
    def __init__(self, pdf_path: str, pdftotext_bin: str = "pdftotext"):
        self.pdf_path = pdf_path
        self.pdftotext_bin = pdftotext_bin
        self.cache: Dict[int, str] = {}

    def get_page_text(self, page_num: int) -> Tuple[bool, str, str]:
        """获取指定页 (1-based) 的原始文本"""
        if page_num in self.cache:
            return True, self.cache[page_num], ""

        if not os.path.isfile(self.pdf_path):
            return False, "", f"PDF 文件不存在: {self.pdf_path}"

        cmd = [self.pdftotext_bin, "-f", str(page_num), "-l", str(page_num), self.pdf_path, "-"]
        try:
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
            if res.returncode != 0:
                return False, "", f"pdftotext 提取第 {page_num} 页失败: {res.stderr.strip()}"
            page_text = res.stdout
            self.cache[page_num] = page_text
            return True, page_text, ""
        except FileNotFoundError:
            return False, "", f"未找到 pdftotext 可执行文件: {self.pdftotext_bin}"
        except Exception as e:
            return False, "", f"执行 pdftotext 异常: {e}"

def extract_sections_from_page(page_text: str) -> List[Tuple[str, str, int]]:
    """
    从单页文本中提取章节或表格定义列表。
    返回: [(section_number, section_title, char_position), ...] 按在页内出现的先后顺序排列
    """
    sections: List[Tuple[str, str, int]] = []

    # 1. 匹配标准章节号：例如 "12.14.20 AL CONTROL REGISTER" 或 "6.2.1.2 RST# Pin Reset"
    sec_pattern = re.compile(
        r"^(?:\x0c|\s)*(\d+(?:\.\d+)+)\s+([A-Za-z0-9_\-#/][A-Za-z0-9_\-#/\s]*?)(?:\r?\n|$)",
        re.MULTILINE
    )
    for m in sec_pattern.finditer(page_text):
        num = m.group(1).strip()
        title = normalize_whitespace(m.group(2))
        # 排除包含 Offset: 等后续非标题行
        if title.startswith("Offset:") or title.startswith("Size:"):
            continue
        sections.append((num, title, m.start()))

    # 2. 匹配表格标题：例如 "TABLE 6-1: RESET SOURCES AND AFFECTED DEVICE FUNCTIONALITY"
    tbl_pattern = re.compile(
        r"^(?:\x0c|\s)*(TABLE\s+\d+-\d+)[:：]?\s*([A-Za-z0-9_\-#/][A-Za-z0-9_\-#/\s]*?)(?:\r?\n|$)",
        re.MULTILINE | re.IGNORECASE
    )
    for m in tbl_pattern.finditer(page_text):
        num = m.group(1).strip()
        title = normalize_whitespace(m.group(2))
        sections.append((num, title, m.start()))

    # 按字符位置排序
    sections.sort(key=lambda x: x[2])
    return sections

def resolve_effective_section(
    extractor: PdfExtractor,
    page_num: int,
    snippet_pos: int,
    max_backtrack: int = 5
) -> Tuple[Optional[str], Optional[str]]:
    """
    解析当前 snippet 实际所属的章节号与章节标题。
    处理当前页内定义以及跨页延续（如 252 页定义延续至 253 页）的场景。
    【核心防御】：若属于前序跨页章节，绝不能凭片段放行，必须精准锁定前序章节！
    """
    # 1. 先检查当前页
    ok, current_page_text, _ = extractor.get_page_text(page_num)
    if not ok:
        return None, None

    current_sections = extract_sections_from_page(current_page_text)

    # 寻找在 snippet_pos 之前出现的最后一个章节定义
    prior_in_page = [s for s in current_sections if s[2] <= snippet_pos]
    if prior_in_page:
        last_sec = prior_in_page[-1]
        return last_sec[0], last_sec[1]

    # 2. 若当前页在 snippet 之前没有章节定义，说明属于前序页跨页延续！
    # 向前回溯前序页面
    for p in range(page_num - 1, max(0, page_num - 1 - max_backtrack), -1):
        ok_prev, prev_text, _ = extractor.get_page_text(p)
        if not ok_prev:
            break
        prev_sections = extract_sections_from_page(prev_text)
        if prev_sections:
            last_sec = prev_sections[-1]
            return last_sec[0], last_sec[1]

    return None, None

def verify_single_quote(
    item: Dict[str, Any],
    extractor: PdfExtractor,
) -> Tuple[bool, str]:
    """严格核验单条 PDF 引用"""
    qid = item.get("id", "UNKNOWN")
    pdf_page = item.get("pdf_page")
    exp_sec_num = item.get("section_number", "").strip()
    exp_sec_title = normalize_whitespace(item.get("section_title", ""))
    snippet = item.get("snippet", "")

    if not isinstance(pdf_page, int) or pdf_page < 1:
        return False, f"[{qid}] pdf_page 必须为正整数 (实际: {pdf_page})"

    ok, page_text, err = extractor.get_page_text(pdf_page)
    if not ok:
        return False, f"[{qid}] 无法读取第 {pdf_page} 页: {err}"

    # 1. 片段字面存在性校验
    norm_page = normalize_whitespace(page_text)
    norm_snippet = normalize_whitespace(snippet)
    if norm_snippet not in norm_page:
        return False, f"[{qid}] 片段未在第 {pdf_page} 页中找到！\n  期望片段: {repr(norm_snippet)}"

    # 定位 snippet 在原始页面中的大致位置
    snippet_pos = 0
    words = norm_snippet.split()
    if words:
        first_word = words[0]
        snippet_pos = page_text.find(first_word)
        if snippet_pos < 0:
            snippet_pos = 0

    # 2. 章节与标题归属核验 (防把错误章节通过片段匹配放行)
    actual_num, actual_title = resolve_effective_section(extractor, pdf_page, snippet_pos)
    if actual_num is None:
        return False, f"[{qid}] 无法解析第 {pdf_page} 页内容所属的有效章节定义！"

    # 统一大小写与空白比对
    norm_exp_num = normalize_whitespace(exp_sec_num).lower()
    norm_act_num = normalize_whitespace(actual_num).lower()
    norm_exp_title = exp_sec_title.lower()
    norm_act_title = (actual_title or "").lower()

    if norm_exp_num != norm_act_num:
        return False, (
            f"[{qid}] 【章节号归属错误拦截】！\n"
            f"  期望章节号: '{exp_sec_num}'\n"
            f"  实际归属章节号: '{actual_num}'\n"
            f"  实际所属标题: '{actual_title}'\n"
            f"  (绝不把错误章节通过片段匹配放行！)"
        )

    if norm_exp_title not in norm_act_title and norm_act_title not in norm_exp_title:
        return False, (
            f"[{qid}] 【章节标题归属错误拦截】！\n"
            f"  期望标题: '{exp_sec_title}'\n"
            f"  实际标题: '{actual_title}'"
        )

    return True, f"  [PASS] {qid:24s} -> Page {pdf_page} | Sec {actual_num}: {actual_title}"

def verify_pdf_manifest(manifest_path: str, override_pdf: Optional[str] = None) -> int:
    """校验 PDF 引文清单主流程"""
    if not os.path.isfile(manifest_path):
        print(f"[FATAL] 清单文件不存在: {manifest_path}")
        return 2

    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:
        print(f"[FATAL] 清单 JSON 解析失败: {e}")
        return 2

    if not isinstance(data, dict):
        print("[FATAL] 清单根结构必须为 JSON Object")
        return 2

    pdf_path = override_pdf or data.get("pdf_path")
    expected_hash = data.get("pdf_sha256")
    quotes = data.get("quotes")

    if not pdf_path or not expected_hash:
        print("[FATAL] 清单必须包含 'pdf_path' 与 'pdf_sha256' 字段")
        return 2

    if not isinstance(quotes, list) or len(quotes) == 0:
        print("[FATAL] 清单 'quotes' 列表不能为空")
        return 2

    # 检查 ID 重复与必须字段
    seen_ids = set()
    for idx, q in enumerate(quotes):
        if not isinstance(q, dict):
            print(f"[FATAL] 条目 #{idx} 不是有效的 JSON Object")
            return 2
        for f in REQUIRED_QUOTE_FIELDS:
            if f not in q or q[f] is None:
                print(f"[FATAL] 条目 #{idx} 缺失必须字段 '{f}': {q}")
                return 2
        qid = q["id"]
        if qid in seen_ids:
            print(f"[FATAL] 清单中存在重复 ID: '{qid}'")
            return 2
        seen_ids.add(qid)

    print("============================================================")
    print(" 厂商 PDF 数据手册引文严格核验工具 (Vendor PDF Verifier)")
    print(f" 清单路径: {manifest_path}")
    print(f" PDF 路径: {pdf_path}")
    print("============================================================")

    # 1. 校验 PDF 文件 SHA-256 哈希完整性
    print("[1/2] 校验 PDF 文件完整性与 SHA-256 基线...")
    if not os.path.isfile(pdf_path):
        print(f"  [FAIL] PDF 文件不存在: {pdf_path}")
        return 1

    actual_hash = compute_file_sha256(pdf_path)
    if actual_hash.lower() != expected_hash.lower():
        print(f"  [FAIL] PDF 文件 SHA-256 校验失败！\n  期望: {expected_hash}\n  实际: {actual_hash}")
        return 1
    print(f"  [OK] PDF 文件 SHA-256 比对完全一致！({actual_hash})")

    # 2. 逐条执行核验
    print(f"[2/2] 执行章节归属与逐页片段逐条核验 (共 {len(quotes)} 项)...")
    print("------------------------------------------------------------")

    pdftotext_bin = shutil.which("pdftotext") or "/opt/homebrew/bin/pdftotext"
    extractor = PdfExtractor(pdf_path, pdftotext_bin)

    failures = 0
    for q in quotes:
        ok, msg = verify_single_quote(q, extractor)
        if ok:
            print(msg)
        else:
            print(f"  [FAIL] {msg}")
            failures += 1

    print("------------------------------------------------------------")
    if failures == 0:
        print(f">>> 全部 {len(quotes)} 项 PDF 章节归属与引文核验通过！<<<")
        return 0
    else:
        print(f">>> 核验存在失败项: 共 {failures} 处错误！<<<")
        return 1

def main():
    parser = argparse.ArgumentParser(description="Vendor PDF Quote Verifier")
    parser.add_argument("--manifest", default="docs/pdf_quotes_manifest.json", help="Path to PDF quotes manifest")
    parser.add_argument("--pdf-path", default=None, help="Override PDF path")
    args = parser.parse_args()

    sys.exit(verify_pdf_manifest(args.manifest, args.pdf_path))

if __name__ == "__main__":
    main()
