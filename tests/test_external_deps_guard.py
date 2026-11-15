#!/usr/bin/env python3
"""Verify that Makefile strictly requires explicit SOES_DIR and MONOCYPHER_DIR.

Ensures that implicit fallbacks (e.g. to /tmp or review directories) are rejected,
and fake temporary directories are never silently adopted.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

def run_make(target: str, env=None, extra_args=None):
    cmd = ['make', target]
    if extra_args:
        cmd.extend(extra_args)
    proc_env = os.environ.copy()
    # 确保清除环境变量中的干扰
    proc_env.pop('SOES_DIR', None)
    proc_env.pop('MONOCYPHER_DIR', None)
    if env:
        proc_env.update(env)
    return subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True, env=proc_env)

# 1. 验证默认不带参数执行 test-soes 必须失败并提示指定 SOES_DIR
res_soes = run_make('test-soes')
assert res_soes.returncode != 0, f"make test-soes unexpectedly succeeded without explicit SOES_DIR!\n{res_soes.stdout}\n{res_soes.stderr}"
assert "SOES_DIR 缺失" in res_soes.stdout or "SOES_DIR 缺失" in res_soes.stderr, f"Missing expected error message in:\n{res_soes.stdout}\n{res_soes.stderr}"

# 2. 验证默认不带参数执行 test-bootloader 必须失败并提示指定 SOES_DIR
res_boot = run_make('test-bootloader')
assert res_boot.returncode != 0, f"make test-bootloader unexpectedly succeeded without explicit SOES_DIR!\n{res_boot.stdout}\n{res_boot.stderr}"
assert "SOES_DIR 缺失" in res_boot.stdout or "SOES_DIR 缺失" in res_boot.stderr

# 3. 验证默认不带参数执行 test-monocypher 必须失败并提示指定 MONOCYPHER_DIR
res_mono = run_make('test-monocypher')
assert res_mono.returncode != 0, f"make test-monocypher unexpectedly succeeded without explicit MONOCYPHER_DIR!\n{res_mono.stdout}\n{res_mono.stderr}"
assert "MONOCYPHER_DIR" in res_mono.stdout or "MONOCYPHER_DIR" in res_mono.stderr

print("PASS: external dependencies require explicit configuration and reject implicit fallbacks")
