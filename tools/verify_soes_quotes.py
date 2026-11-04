#!/usr/bin/env python3
"""
tools/verify_soes_quotes.py

轻量 SOES 源码引用校验工具。
用于自动化核验工程文档中标记为“源码原文”并标注具体行号的引用条目，
确保文档引用与本地真实 SOES 源码逐字完全一致，杜绝记忆概括或笔误改写。

【边界声明】：
本工具仅针对 REGISTERED_QUOTES 列表中明确登记的引用条目进行比对，
不声称亦不覆盖任何未登记的文本或引文。
"""

import os
import sys
import argparse

# 登记的 SOES 源码引用清单
REGISTERED_QUOTES = [
    {
        "id": "Q1_ESC_FOE_STRNCMP",
        "doc": "docs/architecture/soes-integration-options.md",
        "file": "esc_foe.c",
        "start_line": 70,
        "end_line": 73,
        "expected_lines": [
            "   for (i = 0; i < foe_cfg->n_files; i++)",
            "   {",
            "      if (0 == strncmp (foe_file_name, foe_cfg->files[i].name, num_chars))",
            "      {",
        ],
        "description": "WRQ 文件名匹配判定 (0 == strncmp 前缀匹配)",
    },
    {
        "id": "Q2_ESC_FOE_ZERO_LEN_CLOSE",
        "doc": "docs/architecture/soes-integration-options.md",
        "file": "esc_foe.c",
        "start_line": 459,
        "end_line": 465,
        "expected_lines": [
            "   else if (data_len == 0)",
            "   {",
            "      DPRINT(\"FOE_data completed\\n\");",
            "      FOE_fclose ();",
            "      res = FOE_send_ack ();",
            "      FOE_init ();",
            "   }",
        ],
        "description": "0 长度结束包分支 (FOE_fclose 无参且返回值未检查)",
    },
    {
        "id": "Q3_ESC_FOE_SHORT_PACKET_CLOSE",
        "doc": "docs/architecture/soes-integration-options.md",
        "file": "esc_foe.c",
        "start_line": 493,
        "end_line": 506,
        "expected_lines": [
            "      else",
            "      {",
            "         if ((ncopied != data_len) || FOE_fclose ())",
            "         {",
            "            DPRINT(\"FOE_fclose failed to write extra buffer\\n\");",
            "            FOE_abort (FOE_ERR_PROGERROR);",
            "         }",
            "         else",
            "         {",
            "            DPRINT(\"FOE_data completed\\n\");",
            "            res = FOE_send_ack ();",
            "            FOE_init ();",
            "         }",
            "      }",
        ],
        "description": "短包结束分支 (检查 FOE_fclose 返回值)",
    },
    {
        "id": "Q4_ESC_H_STATE_HOOKS",
        "doc": "docs/architecture/soes-integration-options.md",
        "file": "esc.h",
        "start_line": 317,
        "end_line": 318,
        "expected_lines": [
            "   void (*pre_state_change_hook) (uint8_t * as, uint8_t * an);",
            "   void (*post_state_change_hook) (uint8_t * as, uint8_t * an);",
        ],
        "description": "pre/post 状态迁移钩子函数指针定义 (双指针参数)",
    },
    {
        "id": "Q5_ESC_H_ALERROR_FUNC",
        "doc": "docs/architecture/soes-integration-options.md",
        "file": "esc.h",
        "start_line": 712,
        "end_line": 712,
        "expected_lines": [
            "void ESC_ALerror (uint16_t errornumber);",
        ],
        "description": "ESC_ALerror 函数声明",
    },
    {
        "id": "Q6_ESC_C_PRE_HOOK_INVOCATION",
        "doc": "docs/architecture/soes-integration-options.md",
        "file": "esc.c",
        "start_line": 1128,
        "end_line": 1135,
        "expected_lines": [
            "   /* Mask high bits ALcommand, low bits ALstatus */",
            "   as = (uint8_t)((ac << 4) | (as & 0x0f));",
            "",
            "   /* Call post state change hook case it have been configured  */",
            "   if (ESCvar.pre_state_change_hook != NULL)",
            "   {",
            "      ESCvar.pre_state_change_hook (&as, &an);",
            "   }",
        ],
        "description": "pre_state_change_hook 调用点及 as 组合迁移码合成时机",
    },
    {
        "id": "Q7_ESC_C_BOOT_TO_INIT",
        "doc": "docs/architecture/soes-integration-options.md",
        "file": "esc.c",
        "start_line": 1191,
        "end_line": 1196,
        "expected_lines": [
            "      case BOOT_TO_INIT:",
            "      {",
            "         ESC_stopmbx ();",
            "         an = ESCinit;",
            "         break;",
            "      }",
        ],
        "description": "BOOT_TO_INIT 迁移处理 (停邮箱并设置 an=ESCinit)",
    },
    {
        "id": "Q8_ESC_C_POST_HOOK_INVOCATION",
        "doc": "docs/architecture/soes-integration-options.md",
        "file": "esc.c",
        "start_line": 1304,
        "end_line": 1308,
        "expected_lines": [
            "   /* Call post state change hook case it have been configured  */",
            "   if (ESCvar.post_state_change_hook != NULL)",
            "   {",
            "      ESCvar.post_state_change_hook (&as, &an);",
            "   }",
        ],
        "description": "post_state_change_hook 调用点 (在 stopmbx 之后)",
    },
]

def verify_quotes(soes_dir: str, tamper: bool = False) -> int:
    print(f"============================================================")
    print(f" SOES 源码原文引用校验工具 (Quote Verification Tool)")
    print(f" SOES 目标源码目录: {soes_dir}")
    print(f" 登记核验条目数: {len(REGISTERED_QUOTES)}")
    print(f" 模式: {'【故意篡改注入模式 - 测试报警机制】' if tamper else '【正常核验模式】'}")
    print(f"============================================================")

    if not os.path.isdir(soes_dir):
        print(f"[FATAL] SOES 目录不存在: {soes_dir}")
        return 2

    failures = 0
    checked_count = 0

    for item in REGISTERED_QUOTES:
        checked_count += 1
        rel_path = item["file"]
        full_path = os.path.join(soes_dir, rel_path)
        qid = item["id"]
        start_line = item["start_line"]
        end_line = item["end_line"]
        expected = list(item["expected_lines"])

        if tamper and qid == "Q1_ESC_FOE_STRNCMP":
            # 故意篡改第一条引用，验证校验器能否报错截断
            expected[2] = "      if (strncmp (foe_file_name, foe_cfg->files[i].name, num_chars) == 0)"

        if not os.path.isfile(full_path):
            print(f"[{qid}] [MISSING] 源文件缺失: {full_path}")
            failures += 1
            continue

        try:
            with open(full_path, "r", encoding="utf-8", errors="replace") as f:
                all_lines = [l.rstrip("\r\n") for l in f]
        except Exception as e:
            print(f"[{qid}] [ERROR] 读取文件失败: {e}")
            failures += 1
            continue

        if start_line < 1 or end_line > len(all_lines) or start_line > end_line:
            print(f"[{qid}] [OUT_OF_BOUNDS] 行号范围非法: {start_line}-{end_line} (文件总行数: {len(all_lines)})")
            failures += 1
            continue

        actual = all_lines[start_line - 1 : end_line]

        if actual == expected:
            print(f"  [PASS] {qid:26s} -> {rel_path}:{start_line}-{end_line} ({item['description']})")
        else:
            failures += 1
            print(f"  [FAIL] {qid:26s} -> {rel_path}:{start_line}-{end_line} 不匹配！")
            print(f"         来源文档: {item['doc']}")
            print(f"         差异对比 (期望行 vs 实际行):")
            max_len = max(len(expected), len(actual))
            for i in range(max_len):
                exp_l = expected[i] if i < len(expected) else "<EOF>"
                act_l = actual[i] if i < len(actual) else "<EOF>"
                if exp_l != act_l:
                    line_no = start_line + i
                    print(f"           Line {line_no:4d}:")
                    print(f"             - 期望: {repr(exp_l)}")
                    print(f"             + 实际: {repr(act_l)}")

    print(f"------------------------------------------------------------")
    print(f" 核验统计: 检查 {checked_count} 条 | 通过: {checked_count - failures} 条 | 失败: {failures} 条")
    if failures == 0:
        print(f">>> 校验全部通过！登记的 SOES 源码原文引用 100% 吻合！<<<")
        return 0
    else:
        print(f">>> 存在引用不匹配条目，核验失败！<<<")
        return 1

def main():
    parser = argparse.ArgumentParser(description="SOES 源码引用校验工具")
    parser.add_argument(
        "--soes-dir",
        default="/tmp/soes_src/soes",
        help="SOES 源码本地路径 (默认: /tmp/soes_src/soes)",
    )
    parser.add_argument(
        "--tamper-test",
        action="store_true",
        help="故意篡改用例，测试工具报警与非零退出码机制",
    )
    args = parser.parse_args()
    ret = verify_quotes(args.soes_dir, args.tamper_test)
    sys.exit(ret)

if __name__ == "__main__":
    main()
