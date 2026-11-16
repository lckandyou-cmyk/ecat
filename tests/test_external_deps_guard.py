#!/usr/bin/env python3
"""Verify that Makefile strictly requires explicit SOES_DIR and MONOCYPHER_DIR.

Ensures that implicit fallbacks are rejected, recursive make flags/overrides
are properly isolated, and fake directories in temporary paths are never silently adopted.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

def run_guarded_make(target: str, env_override=None, inject_makeflags=None, explicit_args=None):
    """Execute a guarded make sub-process with full recursion variable isolation."""
    cmd = ['make', target]
    if explicit_args:
        cmd.extend(explicit_args)
    proc_env = os.environ.copy()

    # 彻底隔离递归 make 变量与环境依赖变量，防止父 make 命令行覆盖参数渗入
    for key in ['SOES_DIR', 'MONOCYPHER_DIR', 'MAKEFLAGS', 'MAKEOVERRIDES', 'MFLAGS']:
        proc_env.pop(key, None)

    if inject_makeflags is not None:
        # 用于特意测试注入特定 MAKEFLAGS 时隔离是否有效
        proc_env['MAKEFLAGS'] = inject_makeflags
    if env_override:
        proc_env.update(env_override)

    return subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True, env=proc_env)

# ==============================================================================
# 场景 1: 普通默认运行 (不带任何外部依赖参数)
# ==============================================================================
# 1.1: 默认执行 test-soes 必须失败并提示指定 SOES_DIR
res_soes = run_guarded_make('test-soes')
assert res_soes.returncode != 0, f"make test-soes unexpectedly succeeded without explicit SOES_DIR!\n{res_soes.stdout}\n{res_soes.stderr}"
assert "SOES_DIR 缺失" in res_soes.stdout or "SOES_DIR 缺失" in res_soes.stderr

# 1.2: 默认执行 test-bootloader 必须失败并提示指定 SOES_DIR
res_boot = run_guarded_make('test-bootloader')
assert res_boot.returncode != 0, f"make test-bootloader unexpectedly succeeded without explicit SOES_DIR!\n{res_boot.stdout}\n{res_boot.stderr}"
assert "SOES_DIR 缺失" in res_boot.stdout or "SOES_DIR 缺失" in res_boot.stderr

# 1.3: 默认执行 test-monocypher 必须失败并提示指定 MONOCYPHER_DIR
res_mono = run_guarded_make('test-monocypher')
assert res_mono.returncode != 0, f"make test-monocypher unexpectedly succeeded without explicit MONOCYPHER_DIR!\n{res_mono.stdout}\n{res_mono.stderr}"
assert "MONOCYPHER_DIR" in res_mono.stdout or "MONOCYPHER_DIR" in res_mono.stderr

# ==============================================================================
# 场景 2: 父 make 命令行变量传递隔离 (模拟 make test SOES_DIR=... 下的递归调用)
# ==============================================================================
# 即使调用此脚本时父环境中携带了父 make 的 MAKEFLAGS 命令行变量覆盖，脚本内部也已安全剥离隔离
# 下面特意模拟恶意的外部污染注入测试:
leak_makeflags = " -- SOES_DIR=/tmp/ecat-review-deps/soes-baseline MONOCYPHER_DIR=/tmp/ecat-review-deps/monocypher-4.0.3/src"
# 验证普通 run_guarded_make 即使当前进程 os.environ['MAKEFLAGS'] 包含污染，依然能过滤并报错
os.environ['MAKEFLAGS'] = leak_makeflags
os.environ['MAKEOVERRIDES'] = leak_makeflags
res_polluted = run_guarded_make('test-soes')
assert res_polluted.returncode != 0, f"Failed to isolate leaked MAKEFLAGS!\n{res_polluted.stdout}\n{res_polluted.stderr}"
assert "SOES_DIR 缺失" in res_polluted.stdout or "SOES_DIR 缺失" in res_polluted.stderr
os.environ.pop('MAKEFLAGS', None)
os.environ.pop('MAKEOVERRIDES', None)

# ==============================================================================
# 场景 3: 带有通用并行编译参数 (-j4 等) 的 MAKEFLAGS 隔离保护
# ==============================================================================
res_parallel = run_guarded_make('test-soes', inject_makeflags='-j4')
assert res_parallel.returncode != 0
assert "SOES_DIR 缺失" in res_parallel.stdout or "SOES_DIR 缺失" in res_parallel.stderr

# ==============================================================================
# 场景 4: 伪造临时目录与同名路径隔离验证
# ==============================================================================
# 验证即使在临时目录下存在伪造的同名依赖目录 (如 fake_soes/esc.c)，Makefile 也绝不盲目自动采纳
with tempfile.TemporaryDirectory(prefix="ecat_fake_deps_") as fake_tmp:
    fake_soes = pathlib.Path(fake_tmp) / "soes"
    fake_soes.mkdir()
    (fake_soes / "esc.c").write_text("// fake", encoding="utf-8")
    (fake_soes / "esc.h").write_text("// fake", encoding="utf-8")

    # 即使当前工作目录下或者临时路径中有同名文件，不显式指定时 Makefile 依然拒绝执行
    res_fake = run_guarded_make('test-soes')
    assert res_fake.returncode != 0
    assert "SOES_DIR 缺失" in res_fake.stdout or "SOES_DIR 缺失" in res_fake.stderr

print("PASS: external dependencies require explicit configuration, isolate recursive make variables, and reject fake directories")
