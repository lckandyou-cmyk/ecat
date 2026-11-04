#!/usr/bin/env python3
"""
tests/test_verify_quotes.py

三方引用校验工具的独立临时夹具测试套件。
在完全隔离的临时目录 (TemporaryDirectory) 中构建仿真环境，
分别测试：
1. 正例 (三方完全吻合 -> 返回 0)
2. 篡改文档引文字符 (-> 返回 1)
3. 缺失文档标记 (-> 返回 1)
4. 篡改仿真源文件 (-> 返回 1)
5. 清单存在重复 ID 或非法字段 (-> 返回 1)
6. 仿真源文件缺失 (-> 返回 1)

【安全保证】：
本测试仅在临时目录中读写夹具，绝不修改用户真实的 /tmp/soes_src 目录与源码。
"""

import os
import sys
import json
import shutil
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

def create_fixture(temp_dir: str):
    """在临时目录创建标准的仿真环境"""
    manifest_path = os.path.join(temp_dir, "manifest.json")
    doc_rel = "docs/test_doc.md"
    doc_full = os.path.join(temp_dir, doc_rel)
    os.makedirs(os.path.dirname(doc_full), exist_ok=True)

    src_dir = os.path.join(temp_dir, "soes_src")
    src_file = "esc_foe.c"
    src_full = os.path.join(src_dir, src_file)
    os.makedirs(src_dir, exist_ok=True)

    # 1. 创建仿真源文件 (100 行，将代码放在第 10-13 行)
    with open(src_full, "w", encoding="utf-8") as f:
        for idx in range(1, 10):
            f.write(f"// Header line {idx}\n")
        for line in SAMPLE_SNIPPET:
            f.write(line + "\n")
        for idx in range(14, 50):
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
    print(" 运行三方校验工具独立夹具测试套件 (Fixture Test Suite)")
    print("============================================================")

    test_passed = 0
    total_tests = 0

    # Test 1: 正例 (完全一致)
    total_tests += 1
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_fixture(td)
        res = run_verifier(manifest, repo, soes)
        if res.returncode == 0 and "100% 吻合" in res.stdout:
            print("  [PASS] Case 1: 正例三方完全吻合通过 (Exit code: 0)")
            test_passed += 1
        else:
            print(f"  [FAIL] Case 1 应当成功，但返回码为: {res.returncode}")

    # Test 2: 篡改文档中的引文字符
    total_tests += 1
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        with open(doc_path, "r", encoding="utf-8") as f:
            content = f.read()
        content = content.replace("0 == strncmp", "strncmp == 0") # 篡改一个字符
        with open(doc_path, "w", encoding="utf-8") as f:
            f.write(content)
        res = run_verifier(manifest, repo, soes)
        if res.returncode != 0 and "差异: 清单 vs 文档" in res.stdout:
            print(f"  [PASS] Case 2: 篡改文档内容被精准拦截 (Exit code: {res.returncode})")
            test_passed += 1
        else:
            print(f"  [FAIL] Case 2 应当失败，但返回码为: {res.returncode}")

    # Test 3: 删除文档中的标记
    total_tests += 1
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        with open(doc_path, "w", encoding="utf-8") as f:
            f.write("# No tags document\n")
        res = run_verifier(manifest, repo, soes)
        if res.returncode != 0 and "未找到起始标记" in res.stdout:
            print(f"  [PASS] Case 3: 缺失文档标记被精准拦截 (Exit code: {res.returncode})")
            test_passed += 1
        else:
            print(f"  [FAIL] Case 3 应当失败，但返回码为: {res.returncode}")

    # Test 4: 篡改源文件内容
    total_tests += 1
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_fixture(td)
        src_path = os.path.join(soes, "esc_foe.c")
        with open(src_path, "r", encoding="utf-8") as f:
            lines = f.readlines()
        lines[11] = "      if (1 == 1)\n" # 篡改第 12 行
        with open(src_path, "w", encoding="utf-8") as f:
            f.writelines(lines)
        res = run_verifier(manifest, repo, soes)
        if res.returncode != 0 and "差异: 清单 vs 源码" in res.stdout:
            print(f"  [PASS] Case 4: 篡改源文件内容被精准拦截 (Exit code: {res.returncode})")
            test_passed += 1
        else:
            print(f"  [FAIL] Case 4 应当失败，但返回码为: {res.returncode}")

    # Test 5: 清单存在重复 ID
    total_tests += 1
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_fixture(td)
        with open(manifest, "r", encoding="utf-8") as f:
            m = json.load(f)
        m.append(m[0]) # 重复插入相同条目
        with open(manifest, "w", encoding="utf-8") as f:
            json.dump(m, f)
        res = run_verifier(manifest, repo, soes)
        if res.returncode != 0 and "重复的引用 ID" in res.stdout:
            print(f"  [PASS] Case 5: 重复 ID 清单被精准拦截 (Exit code: {res.returncode})")
            test_passed += 1
        else:
            print(f"  [FAIL] Case 5 应当失败，但返回码为: {res.returncode}")

    # Test 6: 源文件缺失
    total_tests += 1
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_fixture(td)
        os.remove(os.path.join(soes, "esc_foe.c"))
        res = run_verifier(manifest, repo, soes)
        if res.returncode != 0 and "源码文件不存在" in res.stdout:
            print(f"  [PASS] Case 6: 源码文件缺失被精准拦截 (Exit code: {res.returncode})")
            test_passed += 1
        else:
            print(f"  [FAIL] Case 6 应当失败，但返回码为: {res.returncode}")

    print("------------------------------------------------------------")
    print(f" 夹具测试统计: 共 {total_tests} 项 | 通过: {test_passed} 项 | 失败: {total_tests - test_passed} 项")
    if test_passed == total_tests:
        print(">>> 校验器夹具测试全部通过！<<<")
        return 0
    else:
        print(">>> 夹具测试存在失败项！<<<")
        return 1

if __name__ == "__main__":
    sys.exit(test_fixture_suite())
