#!/usr/bin/env python3
"""
tools/verify_pdf_quotes.py

厂商 PDF 数据手册引文严格核验工具 (Vendor PDF Quote Verifier)。

【核验原则与三方防御机制】：
1. 外部清单文件登记 PDF 路径、SHA-256 哈希基线及引用条目；
2. 逐条包含：id, doc_path, doc_line, line_text_hash, pdf_page, section_number, section_title, snippet；
3. 【文档行绑定与防漂移】：
   实际读取 doc_path 文档，检查指定 doc_line 是否存在并计算行文本 SHA-256，
   比对 line_text_hash，彻底拦截文档行被删除、篡改或发生行号漂移；
4. 【精确片段定位与多命中歧义防御】：
   使用多空白正则定位完整规范化 snippet 的全部出现位置；
   对每一次命中分别解析其所属章节；若单页内多次命中跨越不同章节，立即歧义拒绝；
5. 【严格标题与章节号全等比对】：
   规范化后严格比对期望章节号与标题，严禁截断标题、子串模糊匹配或空标题通过；
6. 【严格跨页章节归属核验】：
   对跨页表格自动向前回溯定位所属前序章节定义；
   【严禁把错误章节通过片段匹配放行】；
7. 【PDF 文件完整性校验】：
   计算 PDF 实际 SHA-256 哈希并比对预期值。
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
    "line_text_hash",
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

def compute_line_text_hash(line_text: str) -> str:
    """计算单行文本 (去首尾空白) 的 SHA-256"""
    return hashlib.sha256(line_text.strip().encode("utf-8")).hexdigest()

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

def verify_doc_binding(quote: Dict[str, Any], repo_root: str) -> Tuple[bool, str]:
    """严格核验文档行绑定与防篡改/防漂移"""
    qid = quote.get("id", "UNKNOWN")
    doc_path = quote.get("doc_path", "")
    doc_line = quote.get("doc_line")
    exp_hash = quote.get("line_text_hash", "")

    if not doc_path:
        return False, f"[{qid}] 'doc_path' 不能为空"
    if os.path.isabs(doc_path) or os.path.normpath(doc_path).startswith(".."):
        return False, f"[{qid}] 'doc_path' 必须为安全的相对路径: {doc_path}"

    full_doc_path = os.path.join(repo_root, doc_path)
    if not os.path.isfile(full_doc_path):
        return False, f"[{qid}] 绑定的文档文件不存在: {doc_path}"

    try:
        with open(full_doc_path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except Exception as e:
        return False, f"[{qid}] 无法读取文档文件 {doc_path}: {e}"

    if not isinstance(doc_line, int) or doc_line < 1:
        return False, f"[{qid}] 'doc_line' 必须为正整数 (实际: {doc_line})"

    if doc_line > len(lines):
        return False, (
            f"[{qid}] 文档行号超出行数范围: 第 {doc_line} 行不存在 "
            f"(文件共 {len(lines)} 行，可能已被删除)！"
        )

    actual_line = lines[doc_line - 1].rstrip("\r\n").strip()
    actual_hash = compute_line_text_hash(actual_line)

    if actual_hash.lower() != exp_hash.lower():
        return False, (
            f"[{qid}] 文档行文本哈希不匹配 (行可能被篡改或发生行号漂移)！\n"
            f"  文档路径: {doc_path}:{doc_line}\n"
            f"  期望哈希: {exp_hash}\n"
            f"  实际哈希: {actual_hash}\n"
            f"  实际文本: {repr(actual_line)}"
        )

    return True, f"  [DOC-OK] {qid:24s} -> {doc_path}:{doc_line} (哈希验证通过)"

def verify_single_quote(
    item: Dict[str, Any],
    extractor: PdfExtractor,
) -> Tuple[bool, str]:
    """严格核验单条 PDF 引用"""
    qid = item.get("id", "UNKNOWN")
    pdf_page = item.get("pdf_page")
    exp_sec_num = item.get("section_number", "")
    exp_sec_title = item.get("section_title", "")
    snippet = item.get("snippet", "")

    if not isinstance(pdf_page, int) or pdf_page < 1:
        return False, f"[{qid}] pdf_page 必须为正整数 (实际: {pdf_page})"

    norm_exp_title = normalize_whitespace(exp_sec_title)
    norm_exp_num = normalize_whitespace(exp_sec_num)
    if not norm_exp_title:
        return False, f"[{qid}] 期望章节标题不能为空或纯空白！"
    if not norm_exp_num:
        return False, f"[{qid}] 期望章节号不能为空或纯空白！"

    tokens = snippet.strip().split()
    if not tokens:
        return False, f"[{qid}] 片段 (snippet) 不能为空或纯空白！"

    ok, page_text, err = extractor.get_page_text(pdf_page)
    if not ok:
        return False, f"[{qid}] 无法读取第 {pdf_page} 页: {err}"

    # 1. 片段精确位置匹配 (支持跨换行与任意连续空白)
    escaped_tokens = [re.escape(t) for t in tokens]
    snippet_pattern = re.compile(r"\s+".join(escaped_tokens))
    matches = list(snippet_pattern.finditer(page_text))

    if not matches:
        return False, f"[{qid}] 片段未在第 {pdf_page} 页中找到！\n  期望片段: {repr(normalize_whitespace(snippet))}"

    # 2. 对每一次命中分别解析其章节归属
    resolved_sections: List[Tuple[str, str]] = []
    for m in matches:
        pos = m.start()
        sec_num, sec_title = resolve_effective_section(extractor, pdf_page, pos)
        if sec_num is None:
            return False, f"[{qid}] 无法解析第 {pdf_page} 页在位置 {pos} 的有效章节归属！"
        resolved_sections.append((sec_num, sec_title))

    # 3. 处理重复命中：若命中跨越了不同章节，存在归属歧义，必须拒绝！
    distinct_sections = list(set(
        (normalize_whitespace(s[0]).lower(), normalize_whitespace(s[1]).lower())
        for s in resolved_sections
    ))
    if len(distinct_sections) > 1:
        return False, (
            f"[{qid}] 【同页多章节片段归属歧义拦截】！\n"
            f"  片段在第 {pdf_page} 页命中 {len(matches)} 处，跨越了不同章节: {distinct_sections}\n"
            f"  (拒绝模糊放行，必须精准无歧义！)"
        )

    # 4. 章节号与标题严格全等核验 (严禁截断标题或空标题放行)
    actual_num, actual_title = resolved_sections[0]
    norm_act_num = normalize_whitespace(actual_num)
    norm_act_title = normalize_whitespace(actual_title)

    if norm_exp_num.lower() != norm_act_num.lower():
        return False, (
            f"[{qid}] 【章节号归属错误拦截】！\n"
            f"  期望章节号: '{exp_sec_num}'\n"
            f"  实际归属章节号: '{actual_num}'\n"
            f"  实际所属标题: '{actual_title}'\n"
            f"  (绝不把错误章节通过片段匹配放行！)"
        )

    if norm_exp_title.lower() != norm_act_title.lower():
        return False, (
            f"[{qid}] 【章节标题归属错误拦截】！\n"
            f"  期望标题: '{exp_sec_title}'\n"
            f"  实际标题: '{actual_title}'\n"
            f"  (严格全等比对，严禁截断标题或空标题放行！)"
        )

    return True, f"  [PDF-OK] {qid:24s} -> Page {pdf_page} | Sec {actual_num}: {actual_title}"

def verify_pdf_manifest(manifest_path: str, override_pdf: Optional[str] = None, repo_root: Optional[str] = None) -> int:
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

    # 检查 ID 重复与必须字段完整性
    seen_ids = set()
    for idx, q in enumerate(quotes):
        if not isinstance(q, dict):
            print(f"[FATAL] 条目 #{idx} 不是有效的 JSON Object")
            return 2
        for f in REQUIRED_QUOTE_FIELDS:
            if f not in q or q[f] is None:
                print(f"[FATAL] 条目 #{idx} 缺失必须字段 '{f}': {q}")
                return 2
            if isinstance(q[f], str) and not q[f].strip():
                print(f"[FATAL] 条目 #{idx} 字段 '{f}' 不能为空或纯空白！")
                return 2
        qid = q["id"]
        if qid in seen_ids:
            print(f"[FATAL] 清单中存在重复 ID: '{qid}'")
            return 2
        seen_ids.add(qid)

    effective_repo_root = repo_root or os.path.abspath(os.path.join(os.path.dirname(manifest_path), ".."))

    print("============================================================")
    print(" 厂商 PDF 数据手册引文严格核验工具 (Vendor PDF Verifier)")
    print(f" 清单路径: {manifest_path}")
    print(f" PDF 路径: {pdf_path}")
    print(f" 仓库根目录: {effective_repo_root}")
    print("============================================================")

    # 1. 校验 PDF 文件 SHA-256 哈希完整性
    print("[1/3] 校验 PDF 文件完整性与 SHA-256 基线...")
    if not os.path.isfile(pdf_path):
        print(f"  [FAIL] PDF 文件不存在: {pdf_path}")
        return 1

    actual_hash = compute_file_sha256(pdf_path)
    if actual_hash.lower() != expected_hash.lower():
        print(f"  [FAIL] PDF 文件 SHA-256 校验失败！\n  期望: {expected_hash}\n  实际: {actual_hash}")
        return 1
    print(f"  [OK] PDF 文件 SHA-256 比对完全一致！({actual_hash})")

    # 2. 校验文档行绑定与防篡改/防漂移
    print(f"[2/3] 校验文档行物理绑定与文本哈希 (防删除/防篡改/防漂移)...")
    doc_failures = 0
    for q in quotes:
        ok, msg = verify_doc_binding(q, effective_repo_root)
        if ok:
            print(msg)
        else:
            print(f"  [FAIL] {msg}")
            doc_failures += 1

    if doc_failures > 0:
        print("------------------------------------------------------------")
        print(f">>> 文档绑定核验存在失败项: 共 {doc_failures} 处错误！<<<")
        return 1

    # 3. 逐条执行 PDF 章节归属与逐页核验
    print(f"[3/3] 执行章节归属与逐页片段严格核验 (共 {len(quotes)} 项)...")
    print("------------------------------------------------------------")

    pdftotext_bin = shutil.which("pdftotext") or "/opt/homebrew/bin/pdftotext"
    extractor = PdfExtractor(pdf_path, pdftotext_bin)

    pdf_failures = 0
    for q in quotes:
        ok, msg = verify_single_quote(q, extractor)
        if ok:
            print(msg)
        else:
            print(f"  [FAIL] {msg}")
            pdf_failures += 1

    print("------------------------------------------------------------")
    if pdf_failures == 0:
        print(f">>> 全部 {len(quotes)} 项文档绑定、PDF 章节归属与引文核验通过！<<<")
        return 0
    else:
        print(f">>> PDF 章节归属核验存在失败项: 共 {pdf_failures} 处错误！<<<")
        return 1

def main():
    parser = argparse.ArgumentParser(description="Vendor PDF Quote Verifier")
    parser.add_argument("--manifest", default="docs/pdf_quotes_manifest.json", help="Path to PDF quotes manifest")
    parser.add_argument("--pdf-path", default=None, help="Override PDF path")
    parser.add_argument("--repo-root", default=None, help="Override repository root path")
    args = parser.parse_args()

    sys.exit(verify_pdf_manifest(args.manifest, args.pdf_path, args.repo_root))

if __name__ == "__main__":
    main()
