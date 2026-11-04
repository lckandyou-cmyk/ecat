#!/usr/bin/env python3
"""
tests/test_verify_pdf_quotes.py

PDF 引文核验工具的独立全场景测试夹具套件。
针对全场景正例与反例进行严格自动化断言：
1. 正例全要素吻合通过 (Exit 0)
2. 绑定文档文件不存在拦截 (Exit 1)
3. 文档行号超出行数范围 / 行被删除拦截 (Exit 1)
4. 文档行文本被篡改 / 哈希不匹配拦截 (Exit 1)
5. 错误页码拦截 (Exit 1)
6. 错误章节编号拦截 (片段存在但章节所属错误，绝不放行) (Exit 1)
7. 错误章节标题拦截 (Exit 1)
8. 截断章节标题拦截 (严格全等比对，严禁截断标题放行) (Exit 1)
9. 错误片段字符篡改拦截 (Exit 1)
10. 同页多章节片段归属歧义拦截 (Exit 1)
11. PDF 文件 SHA-256 哈希篡改拦截 (Exit 1)
12. PDF 文件缺失拦截 (Exit 1)
13. 空清单 quotes: [] 拦截 (Exit 2)
14. 重复 ID 拦截 (Exit 2)
15. 缺失必须字段 line_text_hash 拦截 (Exit 2)
16. 空/纯空白章节标题拦截 (Exit 2)
17. 空/纯空白章节号拦截 (Exit 2)
18. 空/纯空白片段拦截 (Exit 2)
"""

import os
import sys
import json
import tempfile
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TOOL_PATH = os.path.join(REPO_ROOT, "tools", "verify_pdf_quotes.py")
REAL_PDF = "/Users/milo/ecat-sources/00001909A.pdf"
REAL_HASH = "f9b73097c21a1ad5e9b1a23ba10002c6bbeb7836422376c646e84313294e96c6"

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

def build_valid_manifest(tmp_dir):
    m_path = os.path.join(tmp_dir, "manifest.json")
    data = {
        "pdf_path": REAL_PDF,
        "pdf_sha256": REAL_HASH,
        "quotes": [
            {
                "id": "TEST_PDF_Q1",
                "doc_path": "docs/architecture/soes-integration-options.md",
                "doc_line": 264,
                "line_text_hash": "ba8415890b08d2a99579ace975b28fbfebbc8874ca42ff33751999c60ab42993",
                "pdf_page": 239,
                "section_number": "12.14.20",
                "section_title": "AL CONTROL REGISTER",
                "snippet": "12.14.20 AL CONTROL REGISTER"
            },
            {
                "id": "TEST_PDF_Q2_CROSS_PAGE",
                "doc_path": "docs/architecture/soes-integration-options.md",
                "doc_line": 267,
                "line_text_hash": "9117105c8297674cff84b6cb2b5686e84fddbc57bc0a083f627985e23c940890",
                "pdf_page": 253,
                "section_number": "12.14.34",
                "section_title": "AL EVENT REQUEST REGISTER",
                "snippet": "This bit is cleared by reading the AL Control Register from PDI."
            }
        ]
    }
    with open(m_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    return m_path

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
    print(" 运行 PDF 校验工具独立夹具测试套件 (全场景 18 项覆盖)")
    print("============================================================")

    if not os.path.isfile(REAL_PDF):
        print(f"[FATAL] 本地测试基准 PDF 文件不存在: {REAL_PDF}")
        return 1

    total = 0
    passed = 0

    # Case 1: 正例全要素吻合通过
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        if assert_case("Case 1: 正例全要素吻合通过", 0, "全部 2 项文档绑定、PDF 章节归属与引文核验通过", m):
            passed += 1

    # Case 2: 绑定文档文件不存在拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["doc_path"] = "docs/non_existent_doc_file.md"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 2: 绑定文档文件不存在拦截", 1, "绑定的文档文件不存在", m):
            passed += 1

    # Case 3: 文档行超出范围 / 行被删除拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["doc_line"] = 99999
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 3: 文档行超出范围 / 行被删除拦截", 1, "文档行号超出行数范围", m):
            passed += 1

    # Case 4: 文档行文本被篡改 / 哈希不匹配拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["line_text_hash"] = "0000000000000000000000000000000000000000000000000000000000000000"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 4: 文档行文本被篡改 / 哈希不匹配拦截", 1, "文档行文本哈希不匹配", m):
            passed += 1

    # Case 5: 错误页码拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["pdf_page"] = 238
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 5: 错误页码拦截", 1, "片段未在第 238 页中找到", m):
            passed += 1

    # Case 6: 错误章节编号拦截 (片段存在但章节所属错误，绝不放行) (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][1]["section_number"] = "12.14.28"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 6: 错误章节编号拦截 (防把错误章节通过片段匹配放行)", 1, "章节号归属错误拦截", m):
            passed += 1

    # Case 7: 错误章节标题拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["section_title"] = "WRONG TITLE REGISTER"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 7: 错误章节标题拦截", 1, "章节标题归属错误拦截", m):
            passed += 1

    # Case 8: 截断章节标题拦截 (严格全等比对，严禁截断标题放行) (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["section_title"] = "AL CONTROL"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 8: 截断章节标题拦截", 1, "章节标题归属错误拦截", m):
            passed += 1

    # Case 9: 错误片段字符篡改拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["snippet"] = "12.14.20 AL CONTROL TAMPERED REGISTER"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 9: 错误片段字符篡改拦截", 1, "片段未在第 239 页中找到", m):
            passed += 1

    # Case 10: 同页多章节片段归属歧义拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        # 该语句在第 240 页分别出现在 12.14.21 和 12.14.22 两处，必须歧义拒绝
        d["quotes"][0]["pdf_page"] = 240
        d["quotes"][0]["snippet"] = "For EtherCAT Core CSR registers longer than one byte"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 10: 同页多章节片段归属歧义拦截", 1, "同页多章节片段归属歧义拦截", m):
            passed += 1

    # Case 11: PDF 文件 SHA-256 哈希篡改拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["pdf_sha256"] = "0000000000000000000000000000000000000000000000000000000000000000"
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 11: PDF 文件 SHA-256 哈希篡改拦截", 1, "PDF 文件 SHA-256 校验失败", m):
            passed += 1

    # Case 12: PDF 文件缺失拦截 (Exit 1)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        if assert_case("Case 12: PDF 文件缺失拦截", 1, "PDF 文件不存在", m, "/tmp/non_existent_file.pdf"):
            passed += 1

    # Case 13: 空清单 quotes: [] 拦截 (Exit 2)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"] = []
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 13: 空清单 quotes: [] 拦截", 2, "清单 'quotes' 列表不能为空", m):
            passed += 1

    # Case 14: 重复 ID 拦截 (Exit 2)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"].append(dict(d["quotes"][0]))
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 14: 重复 ID 拦截", 2, "存在重复 ID", m):
            passed += 1

    # Case 15: 缺失必须字段 line_text_hash 拦截 (Exit 2)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        del d["quotes"][0]["line_text_hash"]
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 15: 缺失必须字段 line_text_hash 拦截", 2, "缺失必须字段 'line_text_hash'", m):
            passed += 1

    # Case 16: 空/纯空白章节标题拦截 (Exit 2)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["section_title"] = "   "
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 16: 空/纯空白章节标题拦截", 2, "不能为空或纯空白", m):
            passed += 1

    # Case 17: 空/纯空白章节号拦截 (Exit 2)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["section_number"] = "   "
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 17: 空/纯空白章节号拦截", 2, "不能为空或纯空白", m):
            passed += 1

    # Case 18: 空/纯空白片段拦截 (Exit 2)
    total += 1
    with tempfile.TemporaryDirectory() as td:
        m = build_valid_manifest(td)
        with open(m, "r") as f: d = json.load(f)
        d["quotes"][0]["snippet"] = "   "
        with open(m, "w") as f: json.dump(d, f)
        if assert_case("Case 18: 空/纯空白片段拦截", 2, "不能为空或纯空白", m):
            passed += 1

    print("------------------------------------------------------------")
    print(f" PDF 夹具测试统计: 共 {total} 项 | 通过: {passed} 项 | 失败: {total - passed} 项")
    if passed == total:
        print(f">>> PDF 校验器 {total} 项全场景夹具测试全部通过！<<<")
        return 0
    else:
        print(">>> PDF 夹具测试存在失败项！<<<")
        return 1

if __name__ == "__main__":
    sys.exit(test_pdf_fixture_suite())
