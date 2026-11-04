#!/usr/bin/env python3
"""
tests/test_verify_quotes.py

三方引用校验工具的全场景独立临时夹具测试套件。
在完全隔离的临时目录 (TemporaryDirectory) 中构建仿真环境，
测试覆盖以下全部场景：
1. 正例 (三方完全吻合、SHA-256 比对一致、无非法正文引用 -> 返回 0)
2. 篡改文档引文字符 (-> 返回 1)
3. 缺失文档配对结束标记 (-> 返回 1)
4. 文档标记嵌套错误 (-> 返回 1)
5. 篡改源文件引文内容 (-> 返回 1)
6. 空清单 [] 或 {"quotes": []} (-> 返回 2)
7. 清单存在重复 ID (-> 返回 2)
8. 源码文件缺失 (-> 返回 1)
9. 反向扫描发现文档中存在清单未登记的未知 ID (-> 返回 1)
10. 反向扫描发现标记出现在清单指定外的错误文档中 (-> 返回 1)
11. 反向扫描发现文档中存在重复标记 (-> 返回 1)
12. 源文件 SHA-256 哈希篡改/不匹配 (-> 返回 1)
13. 文档正文出现未登记且未豁免的行号引用 (-> 返回 1)
14. 文档正文行号引用配置了有效精确豁免项 (-> 返回 0)
15. 豁免项包含通配符 '*' 拦截 (-> 返回 1)
16. 跨文档借用引用块拦截 (-> 返回 1)
17. 豁免行文本被修改 (line_text_hash 不匹配) 拦截 (-> 返回 1)
18. 豁免项缺失必须字段拦截 (-> 返回 1)
19. 相同行号但引文内容错误拦截 (-> 返回 1)
20. 引用距离 quote-begin 标记边界测试 (恰好 3 行通过，4 行失败拦截)
21. 同名 basename 但不同目录路径混淆反例拦截 (-> 返回 1)

【安全保证】：
本测试仅在临时目录中读写夹具，绝不修改用户真实的 /tmp/soes_src 目录与源码。
"""

import os
import sys
import json
import hashlib
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

    # 计算源文件真实 SHA-256
    file_hash = hashlib.sha256(open(src_full, "rb").read()).hexdigest()

    # 2. 创建仿真 Markdown 文档 (包含带标记的 fenced code block)
    with open(doc_full, "w", encoding="utf-8") as f:
        f.write("# Test Document\n\n")
        f.write("<!-- quote-begin: TEST_Q1 -->\n")
        f.write("```c\n")
        for line in SAMPLE_SNIPPET:
            f.write(line + "\n")
        f.write("```\n")
        f.write("<!-- quote-end: TEST_Q1 -->\n")

    # 3. 创建外部清单 JSON (包含 SHA-256 基线、quotes 及 exemptions)
    manifest = {
        "source_metadata": {
            "source_directory": src_dir,
            "upstream_commit": "FIXTURE_COMMIT",
            "origin_statement": "Fixture test source baseline"
        },
        "file_checksums": {
            src_file: file_hash
        },
        "quotes": [
            {
                "id": "TEST_Q1",
                "doc_path": doc_rel,
                "source_file": src_file,
                "start_line": 10,
                "end_line": 13,
                "expected_snippet": SAMPLE_SNIPPET,
                "description": "Test fixture quote"
            }
        ],
        "prose_line_exemptions": []
    }
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
        assert_case("Case 1: 正例三方完全吻合通过", 0, "三方引文自动化核验通过", manifest, repo, soes)

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

    # Case 5: 篡改源文件内容 (更新 hash 以隔离三方比对错误)
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        src_path = os.path.join(soes, "esc_foe.c")
        with open(src_path, "r", encoding="utf-8") as f:
            lines = f.readlines()
        lines[11] = "      if (1 == 1)\n"
        with open(src_path, "w", encoding="utf-8") as f:
            f.writelines(lines)
        # 同步更新 manifest hash，确保错误报在三方比对而非 SHA 拦截
        new_hash = hashlib.sha256(open(src_path, "rb").read()).hexdigest()
        with open(manifest, "r", encoding="utf-8") as f:
            m = json.load(f)
        m["file_checksums"]["esc_foe.c"] = new_hash
        with open(manifest, "w", encoding="utf-8") as f:
            json.dump(m, f)
        assert_case("Case 5: 篡改源文件内容三方比对拦截", 1, "差异: 清单 vs 源码", manifest, repo, soes)

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
        m["quotes"].append(m["quotes"][0])
        with open(manifest, "w", encoding="utf-8") as f:
            json.dump(m, f)
        assert_case("Case 7: 清单重复 ID 拦截", 2, "存在重复 ID", manifest, repo, soes)

    # Case 8: 源码文件缺失拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        os.remove(os.path.join(soes, "esc_foe.c"))
        assert_case("Case 8: 源码文件缺失拦截", 1, "基线源文件不存在", manifest, repo, soes)

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

    # Case 12: 源文件 SHA-256 哈希篡改拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        with open(manifest, "r", encoding="utf-8") as f:
            m = json.load(f)
        m["file_checksums"]["esc_foe.c"] = "0000000000000000000000000000000000000000000000000000000000000000"
        with open(manifest, "w", encoding="utf-8") as f:
            json.dump(m, f)
        assert_case("Case 12: 源文件 SHA-256 哈希篡改拦截", 1, "源文件 SHA-256 校验失败", manifest, repo, soes)

    # Case 13: 文档正文出现未登记且未豁免的行号引用拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        with open(doc_path, "a", encoding="utf-8") as f:
            f.write("\n正文中引述未核验代码: `esc_foe.c:999`\n")
        assert_case("Case 13: 未登记且未豁免的正文行号引用拦截", 1, "发现未登记的正文行号引用: 'esc_foe.c:999'", manifest, repo, soes)

    # Case 14: 文档正文行号引用配置了有效精确豁免项通过
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        ex_line_text = "正文中引述豁免代码: `esc_foe.c:999`"
        with open(doc_path, "a", encoding="utf-8") as f:
            f.write(f"\n{ex_line_text}\n")

        # 查找实际行号并计算哈希
        doc_line_no = None
        with open(doc_path, "r", encoding="utf-8") as f:
            for lno, line in enumerate(f, 1):
                if ex_line_text in line:
                    doc_line_no = lno
                    break
        assert doc_line_no is not None
        line_hash = hashlib.sha256(ex_line_text.strip().encode("utf-8")).hexdigest()

        with open(manifest, "r", encoding="utf-8") as f:
            m = json.load(f)
        m["prose_line_exemptions"].append({
            "doc_path": "docs/test_doc.md",
            "doc_line": doc_line_no,
            "source_file": "esc_foe.c",
            "line_reference": "999",
            "line_text_hash": line_hash,
            "verification_status": "LOCAL_MANUAL_VERIFIED",
            "reason": "Test fixture exemption"
        })
        with open(manifest, "w", encoding="utf-8") as f:
            json.dump(m, f)
        assert_case("Case 14: 正文行号引用精确豁免通过", 0, "三方引文自动化核验通过", manifest, repo, soes)

    # Case 15: 豁免项包含通配符 '*' 拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        with open(manifest, "r", encoding="utf-8") as f:
            m = json.load(f)
        m["prose_line_exemptions"].append({
            "doc_path": "docs/test_doc.md",
            "doc_line": 10,
            "source_file": "esc_foe.c",
            "line_reference": "*",
            "line_text_hash": "dummy_hash",
            "verification_status": "LOCAL_MANUAL_VERIFIED",
            "reason": "Illegal wildcard exemption"
        })
        with open(manifest, "w", encoding="utf-8") as f:
            json.dump(m, f)
        assert_case("Case 15: 豁免项包含通配符 '*' 拦截", 1, "包含被严格禁止的通配符 '*'", manifest, repo, soes)

    # Case 16: 跨文档借用引用块拦截 (Doc B 试图借用 Doc A 的 quote)
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        other_doc = os.path.join(repo, "docs/other_doc.md")
        with open(other_doc, "w", encoding="utf-8") as f:
            f.write("# Other Doc\n\n正文跨文档借用: `esc_foe.c:10-13`\n")
        assert_case("Case 16: 跨文档借用引用块拦截", 1, "发现未登记的正文行号引用: 'esc_foe.c:10-13'", manifest, repo, soes)

    # Case 17: 豁免行文本被修改 (line_text_hash 不匹配) 拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        ex_line_text = "正文中引述豁免代码: `esc_foe.c:999`"
        with open(doc_path, "a", encoding="utf-8") as f:
            f.write(f"\n{ex_line_text}\n")
        # 查找实际行号并计算哈希
        doc_line_no = None
        with open(doc_path, "r", encoding="utf-8") as f:
            for lno, line in enumerate(f, 1):
                if ex_line_text in line:
                    doc_line_no = lno
                    break
        assert doc_line_no is not None
        # 使用篡改前哈希写入清单
        correct_hash = hashlib.sha256(ex_line_text.strip().encode("utf-8")).hexdigest()
        with open(manifest, "r", encoding="utf-8") as f:
            m = json.load(f)
        m["prose_line_exemptions"].append({
            "doc_path": "docs/test_doc.md",
            "doc_line": doc_line_no,
            "source_file": "esc_foe.c",
            "line_reference": "999",
            "line_text_hash": correct_hash,
            "verification_status": "LOCAL_MANUAL_VERIFIED",
            "reason": "Test fixture exemption"
        })
        with open(manifest, "w", encoding="utf-8") as f:
            json.dump(m, f)
        # 随后篡改文档内容 (在原行号位置修改文本)
        with open(doc_path, "r", encoding="utf-8") as f:
            content = f.read()
        content = content.replace("`esc_foe.c:999`", "[被篡改]`esc_foe.c:999`")
        with open(doc_path, "w", encoding="utf-8") as f:
            f.write(content)
        assert_case("Case 17: 豁免行文本篡改哈希不匹配拦截", 1, "行文本哈希不匹配", manifest, repo, soes)

    # Case 18: 豁免项缺失必须字段拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        with open(manifest, "r", encoding="utf-8") as f:
            m = json.load(f)
        # 缺少 doc_line 和 line_text_hash
        m["prose_line_exemptions"].append({
            "doc_path": "docs/test_doc.md",
            "source_file": "esc_foe.c",
            "line_reference": "999",
            "verification_status": "LOCAL_MANUAL_VERIFIED",
            "reason": "Missing required fields"
        })
        with open(manifest, "w", encoding="utf-8") as f:
            json.dump(m, f)
        assert_case("Case 18: 豁免项缺失必须字段拦截", 1, "缺失必须字段 'doc_line'", manifest, repo, soes)

    # Case 19: 相同行号但引文内容错误拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        with open(doc_path, "r", encoding="utf-8") as f:
            content = f.read()
        # 将文档内的引文内容修改，保持行号标记不变
        content = content.replace("n_files", "m_files")
        with open(doc_path, "w", encoding="utf-8") as f:
            f.write(content)
        assert_case("Case 19: 相同行号引文内容错误拦截", 1, "差异: 清单 vs 文档", manifest, repo, soes)

    # Case 20: 引用距离 quote-begin 标记边界测试 (恰好 3 行通过，4 行失败拦截)
    # 20.1: 恰好距离 3 行 -> 判定为相邻 Quote 覆盖通过 (Exit 0)
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        # quote-begin 在第 3 行，在第 6 行 (距离 3 行) 插入引用
        with open(doc_path, "r", encoding="utf-8") as f:
            content = f.read()
        # 构造: line 1: title, line 2: quote-begin, line 3: c, line 4: code, line 5: c, line 6: quote-end
        # 我们在 quote-begin 前一行 (距离 1) 或前三行
        new_doc = "行 1: `esc_foe.c:10-13`\n行 2: text\n行 3: text\n" + content
        # quote-begin 现在位于第 4+ 行，距离第 1 行恰好为 3 行
        lines = new_doc.splitlines(keepends=True)
        # 精确构造: line 1 引用, line 2 空行, line 3 空行, line 4 是 quote-begin (4 - 1 = 3)
        doc_text = "正文相邻引用: `esc_foe.c:10-13`\n\n\n<!-- quote-begin: TEST_Q1 -->\n```c\n"
        for line in SAMPLE_SNIPPET:
            doc_text += line + "\n"
        doc_text += "```\n<!-- quote-end: TEST_Q1 -->\n"
        with open(doc_path, "w", encoding="utf-8") as f:
            f.write(doc_text)
        assert_case("Case 20.1: 距离 quote-begin 恰好 3 行通过", 0, "三方引文自动化核验通过", manifest, repo, soes)

    # 20.2: 距离 4 行 -> 超出相邻覆盖范围拦截 (Exit 1)
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        # line 1 引用, line 2 空行, line 3 空行, line 4 空行, line 5 是 quote-begin (5 - 1 = 4 > 3)
        doc_text = "正文非相邻引用: `esc_foe.c:10-13`\n\n\n\n<!-- quote-begin: TEST_Q1 -->\n```c\n"
        for line in SAMPLE_SNIPPET:
            doc_text += line + "\n"
        doc_text += "```\n<!-- quote-end: TEST_Q1 -->\n"
        with open(doc_path, "w", encoding="utf-8") as f:
            f.write(doc_text)
        assert_case("Case 20.2: 距离 quote-begin 4 行拦截", 1, "发现未登记的正文行号引用: 'esc_foe.c:10-13'", manifest, repo, soes)

    # Case 21: 同名 basename 但不同目录路径混淆反例拦截
    with tempfile.TemporaryDirectory() as td:
        manifest, repo, soes = create_base_fixture(td)
        # 将源文件移至子目录 soes_src/soes/esc_foe.c
        sub_dir = os.path.join(soes, "soes")
        os.makedirs(sub_dir, exist_ok=True)
        old_src = os.path.join(soes, "esc_foe.c")
        new_src = os.path.join(sub_dir, "esc_foe.c")
        os.rename(old_src, new_src)
        # 更新 manifest 中 quote 的 source_file
        with open(manifest, "r", encoding="utf-8") as f:
            m = json.load(f)
        m["quotes"][0]["source_file"] = "soes/esc_foe.c"
        m["file_checksums"] = {"soes/esc_foe.c": hashlib.sha256(open(new_src, "rb").read()).hexdigest()}
        with open(manifest, "w", encoding="utf-8") as f:
            json.dump(m, f)
        doc_path = os.path.join(repo, "docs/test_doc.md")
        # 在距离 quote-begin 仅 1 行处引述 "other_dir/esc_foe.c:10-13" (同名 basename 但目录不同)
        doc_text = "不同目录混淆引用: `other_dir/esc_foe.c:10-13`\n<!-- quote-begin: TEST_Q1 -->\n```c\n"
        for line in SAMPLE_SNIPPET:
            doc_text += line + "\n"
        doc_text += "```\n<!-- quote-end: TEST_Q1 -->\n"
        with open(doc_path, "w", encoding="utf-8") as f:
            f.write(doc_text)
        assert_case("Case 21: 同名 basename 但不同目录混淆拦截", 1, "发现未登记的正文行号引用: 'esc_foe.c:10-13'", manifest, repo, soes)

    print("------------------------------------------------------------")
    print(f" 夹具测试统计: 共 {total_tests} 项 | 通过: {test_passed} 项 | 失败: {total_tests - test_passed} 项")
    if test_passed == total_tests:
        print(f">>> 校验器 {total_tests} 项全场景夹具测试全部通过！<<<")
        return 0
    else:
        print(">>> 夹具测试存在失败项！<<<")
        return 1

if __name__ == "__main__":
    sys.exit(test_fixture_suite())
