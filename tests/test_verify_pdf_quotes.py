#!/usr/bin/env python3
"""
tests/test_verify_pdf_quotes.py

PDF 引文核验工具的独立全场景测试夹具套件。
【设计原则】：
1. 完全采用合成文本 (Synthetic Text)，严禁复制任何厂商专有原文；
2. 独立生成标准轻量多页合成 PDF，常规测试不依赖任何外部 PDF 文件或目录；
3. 针对全场景正例与反例进行严格自动化断言 (共 31 项)：
   - Case 1: 正例全要素吻合通过 (Exit 0)
   - Case 2: 绑定文档文件不存在拦截 (Exit 1)
   - Case 3: 文档行超出范围 / 行被删除拦截 (Exit 1)
   - Case 4: 文档行文本被篡改 / 哈希不匹配拦截 (Exit 1)
   - Case 5: 文档行哈希正确但章节号不一致拦截 (A1 反例) (Exit 1)
   - Case 6: 文档行哈希正确但 PDF 页码不一致拦截 (A1 反例) (Exit 1)
   - Case 7: 错误 PDF 页码拦截 (Exit 1)
   - Case 8: 错误章节编号拦截 (片段存在但章节所属错误，拦截不放行) (Exit 1)
   - Case 9: 错误章节标题拦截 (Exit 1)
   - Case 10: 截断章节标题拦截 (严格全等比对，严禁截断标题放行) (Exit 1)
   - Case 11: 错误片段字符篡改拦截 (Exit 1)
   - Case 12: 同页多章节片段归属歧义拦截 (Exit 1)
   - Case 13: 印刷页码 printed_page 与页脚不匹配拦截 (A2 反例) (Exit 1)
   - Case 14: 目录 (TOC) 点线页码被排除，防止误判章节标题 (A3 合成反例) (Exit 1)
   - Case 15: PDF 文件 SHA-256 哈希篡改拦截 (Exit 1)
   - Case 16: PDF 文件缺失拦截 (Exit 1)
   - Case 17: 空清单 quotes: [] 拦截 (Exit 2)
   - Case 18: 重复 ID 拦截 (Exit 2)
   - Case 19: 缺失必须字段 line_text_hash 拦截 (Exit 2)
   - Case 20: 空/纯空白章节标题拦截 (Exit 2)
   - Case 21: 空/纯空白章节号拦截 (Exit 2)
   - Case 22: 空/纯空白片段拦截 (Exit 2)
   - Case 23: A1-a 旁路反例：文档行含 12.14.340，清单为 12.14.34 (数字边界拦截) (Exit 1)
   - Case 24: A1-b 旁路反例：文档行声明 Page 99，另在别处出现 253 (主页码不符拦截) (Exit 1)
   - Case 25: A1-c 旁路反例：文档行含正确数字，同时出现矛盾数字 (矛盾页码拦截) (Exit 1)
   - Case 26: A1-d 旁路反例：printed_page 只在正文而非页脚 (正文 page 254，页脚 101) (Exit 1)
   - Case 27: A4 强化反例：页脚缺失导致 printed_page 校验拦截 (Exit 1)
   - Case 28: A4 强化反例：页脚页码不符拦截 (Exit 1)
   - Case 29: A3 权威文档手写模式扫描拦截 (Exit 1)
   - Case 30: P1c 夹具：soes-integration-options.md 标记外 Page 261 拦截 (Exit 1)
   - Case 31: P1c 夹具：soes-integration-options.md 标记外 12.14.49 拦截 (Exit 1)
"""

import os
import sys
import json
import hashlib
import tempfile
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TOOL_PATH = os.path.join(REPO_ROOT, "tools", "verify_pdf_quotes.py")

def compute_file_sha256(filepath: str) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()

def compute_line_text_hash(text: str) -> str:
    return hashlib.sha256(text.strip().encode("utf-8")).hexdigest()

def make_synthetic_pdf(pages):
    """纯 Python 生成标准无外部依赖的合成 PDF 文件"""
    objs = [
        ("1 0 obj", "<< /Type /Catalog /Pages 2 0 R >>"),
        ("2 0 obj", "REPLACE_PAGES"),
        ("3 0 obj", "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
    ]
    cur_id = 4
    kids_list = []
    for p_text in pages:
        stream_data = "BT\n/F1 12 Tf\n14 TL\n50 750 Td\n"
        for line in p_text.splitlines():
            clean = line.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
            stream_data += f"({clean}) Tj\nT*\n"
        stream_data += "ET"
        c_id = cur_id
        cur_id += 1
        p_id = cur_id
        cur_id += 1
        kids_list.append(f"{p_id} 0 R")
        objs.append((f"{c_id} 0 obj", f"<< /Length {len(stream_data)} >>\nstream\n{stream_data}\nendstream"))
        objs.append((f"{p_id} 0 obj", f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents {c_id} 0 R /Resources << /Font << /F1 3 0 R >> >> >>"))

    objs[1] = ("2 0 obj", f"<< /Type /Pages /Kids [{' '.join(kids_list)}] /Count {len(pages)} >>")

    body = "%PDF-1.4\n"
    offs = []
    for header, content in objs:
        offs.append(len(body))
        body += f"{header}\n{content}\nendobj\n"

    xref_pos = len(body)
    body += f"xref\n0 {len(objs)+1}\n0000000000 65535 f \n"
    for o in offs:
        body += f"{o:010d} 00000 n \n"
    body += f"trailer\n<< /Size {len(objs)+1} /Root 1 0 R >>\nstartxref\n{xref_pos}\n%%EOF\n"
    return body.encode("latin1")

def run_tool(manifest_path, override_pdf=None, repo_root=None):
    cmd = [sys.executable, TOOL_PATH, "--manifest", manifest_path]
    if override_pdf:
        cmd.extend(["--pdf-path", override_pdf])
    if repo_root:
        cmd.extend(["--repo-root", repo_root])
    else:
        cmd.extend(["--repo-root", REPO_ROOT])
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return res.returncode, res.stdout, res.stderr

def setup_synthetic_fixture(tmp_dir):
    """在隔离临时目录中构建全套合成文档与合成 PDF 环境"""
    # 1. 生成 5 页合成 PDF
    pages = [
        # Page 1: 基础章节 1.1 与印刷页脚 101
        "SYNTHETIC ESC DATASHEET\n1.1 SYNTHETIC CONTROL REGISTER\nUnique synthetic sentence for control register.\nDS9999-page 101",
        # Page 2: 包含 1.2 与 1.3 两个章节，且含有一处同页重复句子 (用于测试歧义拦截)
        "SYNTHETIC ESC DATASHEET\n1.2 SYNTHETIC STATUS REGISTER\nUnique sentence for status register.\nShared duplicate sentence across chapters.\n1.3 SYNTHETIC STATUS CODE REGISTER\nShared duplicate sentence across chapters.\nDS9999-page 102",
        # Page 3: 跨页表格章节定义 1.4
        "SYNTHETIC ESC DATASHEET\n1.4 SYNTHETIC CROSS PAGE REGISTER\nInitial table data on page 3.\nDS9999-page 103",
        # Page 4: 延续 1.4 的跨页表格内容 (本页无章节标题定义，回溯至第 3 页)
        "Continuation payload of cross page table.\nDS9999-page 104",
        # Page 5: 目录 (TOC) 点线页码定义 (排除项)
        "SYNTHETIC ESC DATASHEET\n1.5 SYNTHETIC TOC REGISTER ..................................... 999\nBody sentence under TOC entry.\nDS9999-page 105"
    ]
    pdf_bytes = make_synthetic_pdf(pages)
    pdf_path = os.path.join(tmp_dir, "synthetic.pdf")
    with open(pdf_path, "wb") as f:
        f.write(pdf_bytes)
    pdf_hash = compute_file_sha256(pdf_path)

    # 2. 生成合成关联 Markdown 文档
    doc_rel = "docs/synthetic_spec.md"
    doc_full = os.path.join(tmp_dir, doc_rel)
    os.makedirs(os.path.dirname(doc_full), exist_ok=True)
    doc_lines = [
        "# Synthetic Documentation for ESC Specifications",
        "| Section 1.1 (Synthetic Control) | Page 1 | Page 101 | Verbatim text | Line 10 | Line 10 |",
        "| Section 1.4 (Synthetic Cross Page) | Page 4 (Title on 3) | Page 104 | Verbatim text | Line 20 | Line 20 |",
        "| Section 1.5 (TOC Entry) | Page 5 | Page 105 | Verbatim text | Line 30 | Line 30 |"
    ]
    with open(doc_full, "w", encoding="utf-8") as f:
        f.write("\n".join(doc_lines) + "\n")

    hash_line_2 = compute_line_text_hash(doc_lines[1])
    hash_line_3 = compute_line_text_hash(doc_lines[2])
    hash_line_4 = compute_line_text_hash(doc_lines[3])

    # 3. 生成有效清单
    manifest_path = os.path.join(tmp_dir, "manifest.json")
    data = {
        "pdf_path": pdf_path,
        "pdf_sha256": pdf_hash,
        "quotes": [
            {
                "id": "SYNTH_Q1_BASIC",
                "doc_path": doc_rel,
                "doc_line": 2,
                "line_text_hash": hash_line_2,
                "pdf_page": 1,
                "printed_page": 101,
                "section_number": "1.1",
                "section_title": "SYNTHETIC CONTROL REGISTER",
                "snippet": "Unique synthetic sentence for control register."
            },
            {
                "id": "SYNTH_Q2_CROSS_PAGE",
                "doc_path": doc_rel,
                "doc_line": 3,
                "line_text_hash": hash_line_3,
                "pdf_page": 4,
                "printed_page": 104,
                "section_number": "1.4",
                "section_title": "SYNTHETIC CROSS PAGE REGISTER",
                "snippet": "Continuation payload of cross page table."
            }
        ]
    }
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)

    return manifest_path, pdf_path, tmp_dir, hash_line_2, hash_line_3, hash_line_4

def assert_case(name, exp_code, exp_msg_fragment, manifest_path, override_pdf=None, repo_root=None):
    code, stdout, stderr = run_tool(manifest_path, override_pdf, repo_root)
    combined = stdout + "\n" + stderr
    if code != exp_code or (exp_msg_fragment and exp_msg_fragment not in combined):
        print(f"  [FAIL] {name}: 期望退出码 {exp_code}, 实际 {code}; 期望包含 {repr(exp_msg_fragment)}")
        print(f"         stdout: {stdout.strip()[:300]}")
        print(f"         stderr: {stderr.strip()[:300]}")
        return False
    else:
        print(f"  [PASS] {name} (Exit code: {code})")
        return True

def test_pdf_fixture_suite():
    print("============================================================")
    print(" 运行 PDF 校验工具独立夹具测试套件 (合成无外部依赖 31 项覆盖)")
    print("============================================================")

    total = 0
    passed = 0

    # Case 1: 正例全要素吻合通过
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        if assert_case("Case 1: 正例全要素吻合通过", 0, "全部 2 项文档绑定、PDF 章节归属与引文核验通过", m, repo_root=root):
            passed += 1

    # Case 2: 绑定文档文件不存在拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["doc_path"] = "docs/non_existent.md"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 2: 绑定文档文件不存在拦截", 1, "绑定的文档文件不存在", m, repo_root=root):
            passed += 1

    # Case 3: 文档行超出范围 / 行被删除拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["doc_line"] = 99999
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 3: 文档行超出范围 / 行被删除拦截", 1, "文档行号超出行数范围", m, repo_root=root):
            passed += 1

    # Case 4: 文档行文本被篡改 / 哈希不匹配拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["line_text_hash"] = "0000000000000000000000000000000000000000000000000000000000000000"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 4: 文档行文本被篡改 / 哈希不匹配拦截", 1, "文档行文本哈希不匹配", m, repo_root=root):
            passed += 1

    # Case 5: 文档行哈希正确但章节号不一致拦截 (A1 反例) (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        # 文档行实际为 Section 1.1，清单故意声明为 9.9
        d["quotes"][0]["section_number"] = "9.9"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 5: 文档行哈希正确但章节号不一致拦截 (A1)", 1, "文档行章节号不一致拦截", m, repo_root=root):
            passed += 1

    # Case 6: 文档行哈希正确但 PDF 页码不一致拦截 (A1 反例) (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        # 文档行实际声明 Page 1，清单故意声明为 99
        d["quotes"][0]["pdf_page"] = 99
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 6: 文档行哈希正确但 PDF 页码不一致拦截 (A1)", 1, "文档行 PDF 页码不一致拦截", m, repo_root=root):
            passed += 1

    # Case 7: 错误 PDF 页码拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        # 修改文档行使其包含 Page 2，保持行内页码一致但实际片段在 Page 1
        doc_file = os.path.join(root, "docs/synthetic_spec.md")
        with open(doc_file, "r") as f: lines = f.readlines()
        lines[1] = "| Section 1.1 (Synthetic Control) | Page 2 | Page 102 | Verbatim text |\n"
        with open(doc_file, "w") as f: f.writelines(lines)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["line_text_hash"] = compute_line_text_hash(lines[1])
        d["quotes"][0]["pdf_page"] = 2
        d["quotes"][0]["printed_page"] = 102
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 7: 错误 PDF 页码拦截", 1, "片段未在第 2 页中找到", m, repo_root=root):
            passed += 1

    # Case 8: 错误章节编号拦截 (片段存在但章节所属错误，拦截不放行) (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        # 文档行修改使其包含 Section 1.9，片段在 1.1
        doc_file = os.path.join(root, "docs/synthetic_spec.md")
        with open(doc_file, "r") as f: lines = f.readlines()
        lines[1] = "| Section 1.9 (Synthetic Control) | Page 1 | Page 101 | Verbatim text |\n"
        with open(doc_file, "w") as f: f.writelines(lines)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["line_text_hash"] = compute_line_text_hash(lines[1])
        d["quotes"][0]["section_number"] = "1.9"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 8: 错误章节编号拦截 (防把错误章节通过片段匹配放行)", 1, "章节号归属错误拦截", m, repo_root=root):
            passed += 1

    # Case 9: 错误章节标题拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["section_title"] = "WRONG SYNTHETIC TITLE"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 9: 错误章节标题拦截", 1, "章节标题归属错误拦截", m, repo_root=root):
            passed += 1

    # Case 10: 截断章节标题拦截 (严格全等比对，严禁截断标题放行) (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["section_title"] = "SYNTHETIC CONTROL"  # 故意截断 "REGISTER"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 10: 截断章节标题拦截", 1, "章节标题归属错误拦截", m, repo_root=root):
            passed += 1

    # Case 11: 错误片段字符篡改拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["snippet"] = "Tampered snippet text that does not exist"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 11: 错误片段字符篡改拦截", 1, "片段未在第 1 页中找到", m, repo_root=root):
            passed += 1

    # Case 12: 同页多章节片段归属歧义拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        doc_file = os.path.join(root, "docs/synthetic_spec.md")
        with open(doc_file, "r") as f: lines = f.readlines()
        lines[1] = "| Section 1.2 (Synthetic Shared) | Page 2 | Page 102 | Verbatim text |\n"
        with open(doc_file, "w") as f: f.writelines(lines)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["line_text_hash"] = compute_line_text_hash(lines[1])
        d["quotes"][0]["pdf_page"] = 2
        d["quotes"][0]["printed_page"] = 102
        d["quotes"][0]["section_number"] = "1.2"
        d["quotes"][0]["section_title"] = "SYNTHETIC STATUS REGISTER"
        # 该语句在第 2 页同时出现在 1.2 与 1.3 章节下，必须歧义拒绝
        d["quotes"][0]["snippet"] = "Shared duplicate sentence across chapters."
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 12: 同页多章节片段归属歧义拦截", 1, "同页多章节片段归属歧义拦截", m, repo_root=root):
            passed += 1

    # Case 13: 印刷页码 printed_page 与页脚不匹配拦截 (A2 反例) (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["printed_page"] = 999  # 第 1 页页脚实际为 101
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 13: 印刷页码 printed_page 与页脚不匹配拦截 (A2)", 1, "印刷页码校验拦截", m, repo_root=root):
            passed += 1

    # Case 14: 目录 (TOC) 点线页码被排除，防止误判章节标题 (A3 合成反例) (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, hash4 = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["doc_line"] = 4
        d["quotes"][0]["line_text_hash"] = hash4
        d["quotes"][0]["pdf_page"] = 5
        d["quotes"][0]["printed_page"] = 105
        d["quotes"][0]["section_number"] = "1.5"
        d["quotes"][0]["section_title"] = "SYNTHETIC TOC REGISTER"
        d["quotes"][0]["snippet"] = "Body sentence under TOC entry."
        with open(m, "w") as f: json.dump(d, f)
        # 因 1.5 是目录点线行，被自动排除，无法匹配为有效章节，故应拦截
        if assert_case("Case 14: 目录点线页码被排除防止误判 (A3)", 1, "章节号归属错误拦截", m, repo_root=root):
            passed += 1

    # Case 15: PDF 文件 SHA-256 哈希篡改拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["pdf_sha256"] = "0000000000000000000000000000000000000000000000000000000000000000"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 15: PDF 文件 SHA-256 哈希篡改拦截", 1, "PDF 文件 SHA-256 校验失败", m, repo_root=root):
            passed += 1

    # Case 16: PDF 文件缺失拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        if assert_case("Case 16: PDF 文件缺失拦截", 1, "PDF 文件不存在", m, "/tmp/non_existent_file.pdf", repo_root=root):
            passed += 1

    # Case 17: 空清单 quotes: [] 拦截 (Exit 2)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"] = []
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 17: 空清单 quotes: [] 拦截", 2, "清单 'quotes' 列表不能为空", m, repo_root=root):
            passed += 1

    # Case 18: 重复 ID 拦截 (Exit 2)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"].append(dict(d["quotes"][0]))
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 18: 重复 ID 拦截", 2, "存在重复 ID", m, repo_root=root):
            passed += 1

    # Case 19: 缺失必须字段 line_text_hash 拦截 (Exit 2)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        del d["quotes"][0]["line_text_hash"]
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 19: 缺失必须字段 line_text_hash 拦截", 2, "缺失必须字段 'line_text_hash'", m, repo_root=root):
            passed += 1

    # Case 20: 空/纯空白章节标题拦截 (Exit 2)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["section_title"] = "   "
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 20: 空/纯空白章节标题拦截", 2, "不能为空或纯空白", m, repo_root=root):
            passed += 1

    # Case 21: 空/纯空白章节号拦截 (Exit 2)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["section_number"] = "   "
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 21: 空/纯空白章节号拦截", 2, "不能为空或纯空白", m, repo_root=root):
            passed += 1

    # Case 22: 空/纯空白片段拦截 (Exit 2)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["snippet"] = "   "
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 22: 空/纯空白片段拦截", 2, "不能为空或纯空白", m, repo_root=root):
            passed += 1

    # Case 23: A1-a 旁路反例：文档行含 12.14.340，清单为 12.14.34 (数字边界拦截)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        doc_file = os.path.join(root, "docs/synthetic_spec.md")
        with open(doc_file, "r") as f: lines = f.readlines()
        lines[1] = "| Section 12.14.340 | Page 1 | Page 101 | Verbatim text |\n"
        with open(doc_file, "w") as f: f.writelines(lines)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["line_text_hash"] = compute_line_text_hash(lines[1])
        d["quotes"][0]["section_number"] = "12.14.34"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 23: A1-a 旁路反例 (12.14.340 vs 12.14.34 边界拦截)", 1, "文档行章节号不一致拦截", m, repo_root=root):
            passed += 1

    # Case 24: A1-b 旁路反例：文档行声明 Page 99，另在别处出现 253 (主页码不符拦截)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        doc_file = os.path.join(root, "docs/synthetic_spec.md")
        with open(doc_file, "r") as f: lines = f.readlines()
        lines[1] = "| Section 1.1 | Page 99 (note 253) | Page 101 | Verbatim text |\n"
        with open(doc_file, "w") as f: f.writelines(lines)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["line_text_hash"] = compute_line_text_hash(lines[1])
        d["quotes"][0]["pdf_page"] = 253
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 24: A1-b 旁路反例 (Page 99 与 note 253 主页码拦截)", 1, "文档行 PDF 页码不一致拦截", m, repo_root=root):
            passed += 1

    # Case 25: A1-c 旁路反例：文档行含正确数字，同时出现矛盾数字 (矛盾页码拦截)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        doc_file = os.path.join(root, "docs/synthetic_spec.md")
        with open(doc_file, "r") as f: lines = f.readlines()
        lines[1] = "| Section 1.1 | Page 1 与 第 99 页 | Page 101 | Verbatim text |\n"
        with open(doc_file, "w") as f: f.writelines(lines)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["line_text_hash"] = compute_line_text_hash(lines[1])
        d["quotes"][0]["pdf_page"] = 1
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 25: A1-c 旁路反例 (文档行含正确数字与矛盾数字拦截)", 1, "文档行存在矛盾页码拦截", m, repo_root=root):
            passed += 1

    # Case 26: A1-d 旁路反例：printed_page 只在正文而非页脚 (正文 page 254，页脚 101)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        # 生成正文含 page 254 但页脚为 101 的合成 PDF
        p_body = make_synthetic_pdf([
            "SYNTHETIC ESC DATASHEET\n1.1 SYNTHETIC CONTROL REGISTER\nUnique synthetic sentence for control register.\nSee page 254 for details.\nDS9999-page 101"
        ])
        with open(p, "wb") as f: f.write(p_body)
        with open(m, "r") as f: d = json.load(f)
        d["pdf_sha256"] = compute_file_sha256(p)
        doc_file = os.path.join(root, "docs/synthetic_spec.md")
        with open(doc_file, "r") as f: lines = f.readlines()
        lines[1] = "| Section 1.1 | Page 1 | Page 254 | Verbatim text |\n"
        with open(doc_file, "w") as f: f.writelines(lines)
        d["quotes"][0]["line_text_hash"] = compute_line_text_hash(lines[1])
        d["quotes"][0]["pdf_page"] = 1
        d["quotes"][0]["printed_page"] = 254
        # 删除第 2 条 quote 避免单页 PDF 缺少第 4 页
        d["quotes"] = [d["quotes"][0]]
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 26: A1-d 旁路反例 (printed_page 仅在正文不在页脚拦截)", 1, "页脚印刷页码与清单不匹配", m, repo_root=root):
            passed += 1

    # Case 27: A4 强化反例：页脚缺失导致 printed_page 校验拦截
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        # 生成无 DS9999-page 页脚的 PDF
        p_nofooter = make_synthetic_pdf([
            "SYNTHETIC ESC DATASHEET\n1.1 SYNTHETIC CONTROL REGISTER\nUnique synthetic sentence for control register.\nNo standard footer here"
        ])
        with open(p, "wb") as f: f.write(p_nofooter)
        with open(m, "r") as f: d = json.load(f)
        d["pdf_sha256"] = compute_file_sha256(p)
        d["quotes"] = [d["quotes"][0]]
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 27: A4 强化反例 (页脚缺失拦截)", 1, "印刷页码页脚缺失拦截", m, repo_root=root):
            passed += 1

    # Case 28: A4 强化反例：页脚页码不符拦截
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["printed_page"] = 888  # 实际为 101
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 28: A4 强化反例 (页脚页码不符拦截)", 1, "页脚印刷页码与清单不匹配", m, repo_root=root):
            passed += 1

    # Case 29: A3 权威文档手写引用模式拦截
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        # 在临时权威文档 README.md 中写入禁止的手写模式
        readme_path = os.path.join(root, "README.md")
        with open(readme_path, "w", encoding="utf-8") as f:
            f.write("# README\nHandwritten violation: see Section 12.14.20 on page 239\n")
        if assert_case("Case 29: A3 权威文档手写模式扫描拦截", 1, "发现禁止的手写引用模式", m, repo_root=root):
            passed += 1

    # Case 30: P1c 夹具：soes-integration-options.md 标记之外出现 "Page 261" 拦截
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        soes_doc = os.path.join(root, "docs/architecture/soes-integration-options.md")
        os.makedirs(os.path.dirname(soes_doc), exist_ok=True)
        with open(soes_doc, "w", encoding="utf-8") as f:
            f.write("# SOES Options\nViolation text outside table: refer to Page 261 for details\n<!-- pdf-table-begin -->\n<!-- pdf-table-end -->\n")
        if assert_case("Case 30: P1c soes-integration-options.md 标记外 Page 261 拦截", 1, "发现禁止的手写引用模式", m, repo_root=root):
            passed += 1

    # Case 31: P1c 夹具：soes-integration-options.md 标记之外出现 "12.14.49" 拦截
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m, p, root, _, _, _ = setup_synthetic_fixture(td)
        soes_doc = os.path.join(root, "docs/architecture/soes-integration-options.md")
        os.makedirs(os.path.dirname(soes_doc), exist_ok=True)
        with open(soes_doc, "w", encoding="utf-8") as f:
            f.write("# SOES Options\nViolation text outside table: Section 12.14.49 register\n<!-- pdf-table-begin -->\n<!-- pdf-table-end -->\n")
        if assert_case("Case 31: P1c soes-integration-options.md 标记外 12.14.49 拦截", 1, "发现禁止的手写引用模式", m, repo_root=root):
            passed += 1

    print("------------------------------------------------------------")
    print(f" PDF 夹具测试统计: 共 {total} 项 | 通过: {passed} 项 | 失败: {total - passed} 项")
    if passed == total:
        print(f">>> PDF 校验器 {total} 项合成夹具测试全部通过！<<<")
        return 0
    else:
        print(">>> PDF 夹具测试存在失败项！<<<")
        return 1

if __name__ == "__main__":
    sys.exit(test_pdf_fixture_suite())
