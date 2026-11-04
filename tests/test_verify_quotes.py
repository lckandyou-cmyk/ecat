#!/usr/bin/env python3
"""
tests/test_verify_quotes.py

三方引用校验工具的全场景独立临时夹具测试套件。
在完全隔离的临时目录 (TemporaryDirectory) 中构建仿真环境，
测试覆盖以下全部场景：
1. 正例 (三方完全吻合 -> 返回 0)
2. 篡改文档引文字符 (-> 返回 1)
3. 缺失文档配对结束标记 (-> 返回 1)
4. 文档标记嵌套错误 (-> 返回 1)
5. 篡改源文件内容 (-> 返回 1)
6. 空清单 [] (-> 返回 2)
7. 清单存在重复 ID (-> 返回 2)
8. 源码文件缺失 (-> 返回 1)
9. 反向扫描发现文档中存在清单未登记的未知 ID (-> 返回 1)
10. 反向扫描发现标记出现在清单指定外的错误文档中 (-> 返回 1)
11. 反向扫描发现文档中存在重复标记 (-> 返回 1)

【安全保证】：
本测试仅在临时目录中读写夹具，绝不修改用户真实的 /tmp/soes_src 目录与源码。
"""

import os
import sys
import json
import tempfile
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
VERIFY_SCRIPT = os.path.join(REPO_ROOT, "tools", "verify_soes_quotes.py")

SAMPLE_SNIPPET = [
    "   for (i = 0; i < foe_cfg->n_files; i++)",
    "   {",
    "      if (0 == strncmp (foe_file_name, foe_cfg->files[i].name, num_chars))",
    "      {",
]

def create_base_fixture(temp_dir: str):
    """在临时目录创建标准的仿真三方环境"""
    manifest_path = os.path.join(temp_dir, "manifest.json")
    doc_rel = "docs/test_doc.md"
    doc_full = os.path.join(temp_dir, doc_rel)
    os.makedirs(os.path.dirname(doc_full), exist_ok=True)

    src_dir = os.path.join(temp_dir, "soes_src")
    src_file = "esc_foe.c"
    src_full = os.path.join(src_dir, src_file)
    os.makedirs(src_dir, exist_ok=True)

    # 1. 创建仿真源文件 (第 10-13 行)
    with open(src_full, "w", encoding="utf-8") as f:
        for idx in range(1, 10):
            f.write(f"// Header line {idx}\n")
        for line in SAMPLE_SNIPPET:
            f.write(line + "\n")
        for idx in range(14, 40):
            f.write(f"// Footer line {idx}\n")

    # 2. 创建仿真 Markdown 文档 (包含带标记的 fenced code block)
    with open(doc_full, "w", encoding="utf-8") as f:
        f.write("# Test Document\n\n")
        f.write("<!-- quote-begin: TEST_Q1 -->\n")
        f.write("```c\n")
        for line in SAMPLE_SNIPPET:
            f.write(line + "\n")
        f.write("```\n")
        f.write("<!-- quote-end: TEST_Q1 -->\n")

    # 3. 创建外部清单 JSON
    manifest = [
        {
            "id": "TEST_Q1",
            "doc_path": doc_rel,
            "source_file": src_file,
            "start_line": 10,
            "end_line": 13,
            "expected_snippet": SAMPLE_SNIPPET,
            "description": "Test fixture quote"
        }
    ]
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    return manifest_path, temp_dir, src_dir

def run_verifier(manifest: str, repo: str, soes: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, VERIFY_SCRIPT, "--manifest", manifest, "--repo-root", repo, "--soes-dir", soes],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

def test_fixture_suite():
    print("============================================================")
    print(" 运行三方校验工具独立夹具测试套件 (全场景覆盖)")
    print("============================================================")

    test_passed = 0
    total_tests = 0

    def assert_case(name: str, expected_exit: int, check_str: str, manifest: str, repo: str, soes: str):
        nonlocal test_passed, total_tests
        total_tests += 1
        res = run_verifier(manifest, repo, soes)
        cond_exit = (res.returncode == expected_exit) if expected_exit is not None else (res.returncode != 0)
        cond_str = (check_str in res.stdout or check_str in res.stderr) if check_str else True
        if cond_exit and cond_str:
            print(f"  [PASS] {name} (Exit code: {res.returncode})")
            test_passed += 1
        else:
            print(f"  [FAIL] {name}: 期望退出码 {expected_exit}, 实际 {res.returncode}; 期望包含 '{check_str}'")
            print(f"         stdout: {res.stdout.strip()}")
            print(f"         stderr: {res.stderr.strip()}")

    # Case 1: 正例 (三方完全吻合)
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        assert_case("Case 1: 正例三方完全吻合通过", 0, "全部三方比对 100% 吻合", manifest, repo, soes)

    # Case 2: 篡改文档中的引文字符
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        with open(doc_path, "r", encoding="utf-8") as f:
            content = f.read()
        content = content.replace("0 == strncmp", "strncmp == 0")
        with open(doc_path, "w", encoding="utf-8") as f:
            f.write(content)
        assert_case("Case 2: 篡改文档引文字符拦截", 1, "差异: 清单 vs 文档", manifest, repo, soes)

    # Case 3: 缺失配对结束标记
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        with open(doc_path, "w", encoding="utf-8") as f:
            f.write("<!-- quote-begin: TEST_Q1 -->\n```c\ncode\n```\n")
        assert_case("Case 3: 缺失配对结束标记拦截", 1, "未配对闭合的标记", manifest, repo, soes)

    # Case 4: 嵌套标记错误
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        with open(doc_path, "w", encoding="utf-8") as f:
            f.write("<!-- quote-begin: TEST_Q1 -->\n<!-- quote-begin: NESTED -->\n<!-- quote-end: TEST_Q1 -->\n")
        assert_case("Case 4: 嵌套标记错误拦截", 1, "发现嵌套标记", manifest, repo, soes)

    # Case 5: 篡改源文件内容
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        src_path = os.path.join(soes, "esc_foe.c")
        with open(src_path, "r", encoding="utf-8") as f:
            lines = f.readlines()
        lines[11] = "      if (1 == 1)\n"
        with open(src_path, "w", encoding="utf-8") as f:
            f.writelines(lines)
        assert_case("Case 5: 篡改源文件内容拦截", 1, "差异: 清单 vs 源码", manifest, repo, soes)

    # Case 6: 空清单 [] 拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        with open(manifest, "w", encoding="utf-8") as f:
            f.write("[]\n")
        assert_case("Case 6: 空清单 [] 拦截", 2, "清单内容为空列表", manifest, repo, soes)

    # Case 7: 清单存在重复 ID 拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        with open(manifest, "r", encoding="utf-8") as f:
            m = json.load(f)
        m.append(m[0])
        with open(manifest, "w", encoding="utf-8") as f:
            json.dump(m, f)
        assert_case("Case 7: 清单重复 ID 拦截", 2, "存在重复 ID", manifest, repo, soes)

    # Case 8: 源码文件缺失拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        os.remove(os.path.join(soes, "esc_foe.c"))
        assert_case("Case 8: 源码文件缺失拦截", 1, "源码引文提取失败", manifest, repo, soes)

    # Case 9: 反向扫描发现文档中存在清单未登记的未知 ID 拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        with open(doc_path, "a", encoding="utf-8") as f:
            f.write("\n<!-- quote-begin: UNKNOWN_Q -->\n```c\nint a;\n```\n<!-- quote-end: UNKNOWN_Q -->\n")
        assert_case("Case 9: 文档存在清单未登记未知 ID 拦截", 1, "存在清单未登记的未知 ID 'UNKNOWN_Q'", manifest, repo, soes)

    # Case 10: 反向扫描发现标记出现在清单指定外的错误文档中拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        wrong_doc = os.path.join(repo, "docs/other_doc.md")
        with open(wrong_doc, "w", encoding="utf-8") as f:
            f.write("<!-- quote-begin: TEST_Q1 -->\n```c\nline\n```\n<!-- quote-end: TEST_Q1 -->\n")
        doc_path = os.path.join(repo, "docs/test_doc.md")
        os.remove(doc_path) # 从指定文档中删除
        assert_case("Case 10: 标记出现在非指定文档拦截", 1, "标记文档错位", manifest, repo, soes)

    # Case 11: 反向扫描发现文档中存在重复标记拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        with open(doc_path, "a", encoding="utf-8") as f:
            f.write("\n<!-- quote-begin: TEST_Q1 -->\n```c\nrepeat\n```\n<!-- quote-end: TEST_Q1 -->\n")
        assert_case("Case 11: 文档存在重复标记拦截", 1, "重复标记 'TEST_Q1'", manifest, repo, soes)

    print("------------------------------------------------------------")
    print(f" 夹具测试统计: 共 {total_tests} 项 | 通过: {test_passed} 项 | 失败: {total_tests - test_passed} 项")
    if test_passed == total_tests:
        print(">>> 校验器 11 项夹具测试全部通过！<<<")
        return 0
    else:
        print(">>> 夹具测试存在失败项！<<<")
        return 1

if __name__ == "__main__":
    sys.exit(test_fixture_suite())
