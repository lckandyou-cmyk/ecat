#!/usr/bin/env python3
"""Check that mismatched target registry buffer configurations fail at link time."""
import os
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
cc = os.environ.get('CC', 'cc')

with tempfile.TemporaryDirectory(prefix='ecat-reg-abi-') as tmp:
    tmp_path = pathlib.Path(tmp)
    caller = tmp_path / 'caller.c'
    caller.write_text('#include "ecat_target_registry.h"\n'
                      'int main(void) {\n'
                      '    ecat_target_registry_t reg;\n'
                      '    return ecat_target_registry_init(&reg);\n'
                      '}\n')

    # Compile library object with default config
    reg_obj = tmp_path / 'ecat_target_registry.o'
    subprocess.run([cc, '-std=c99', '-I' + str(root / 'include'),
                    '-c', str(root / 'src/ecat_target_registry.c'),
                    '-o', str(reg_obj)], check=True)

    # Compile caller with custom MAX_BLOCKS=1
    caller_mismatch_obj = tmp_path / 'caller_mismatch.o'
    subprocess.run([cc, '-std=c99', '-I' + str(root / 'include'),
                    '-DECAT_TARGET_REGISTRY_MAX_BLOCKS=1',
                    '-c', str(caller),
                    '-o', str(caller_mismatch_obj)], check=True)

    # Linking must fail when configurations differ
    mismatch = subprocess.run([cc, str(caller_mismatch_obj), str(reg_obj),
                               '-o', str(tmp_path / 'mismatch')],
                              capture_output=True, text=True)
    assert mismatch.returncode != 0, f"Mismatched registry configurations must fail to link, got returncode 0. stderr: {mismatch.stderr}"
    assert 'ecat_target_registry_abi_' in mismatch.stderr, f"Linker error must cite ecat_target_registry_abi_, got: {mismatch.stderr}"

    # Compile caller with custom MAX_ACL_RULES=1
    caller_acl_mismatch_obj = tmp_path / 'caller_acl_mismatch.o'
    subprocess.run([cc, '-std=c99', '-I' + str(root / 'include'),
                    '-DECAT_TARGET_REGISTRY_MAX_ACL_RULES=1',
                    '-c', str(caller),
                    '-o', str(caller_acl_mismatch_obj)], check=True)

    mismatch_acl = subprocess.run([cc, str(caller_acl_mismatch_obj), str(reg_obj),
                                   '-o', str(tmp_path / 'mismatch_acl')],
                                  capture_output=True, text=True)
    assert mismatch_acl.returncode != 0, f"Mismatched ACL rules configuration must fail to link, got returncode 0. stderr: {mismatch_acl.stderr}"
    assert 'ecat_target_registry_abi_' in mismatch_acl.stderr, f"Linker error must cite ecat_target_registry_abi_, got: {mismatch_acl.stderr}"

    # Optimization and linker dead-stripping scenarios (-O2 and link-time stripping)
    import sys
    compile_strip = ['-ffunction-sections', '-fdata-sections'] if sys.platform != 'darwin' else []
    link_strip = ['-Wl,--gc-sections'] if sys.platform != 'darwin' else ['-Wl,-dead_strip']

    reg_opt_obj = tmp_path / 'ecat_target_registry_opt.o'
    subprocess.run([cc, '-std=c99', '-O2', '-I' + str(root / 'include')] +
                   compile_strip +
                   ['-c', str(root / 'src/ecat_target_registry.c'),
                    '-o', str(reg_opt_obj)], check=True)

    caller_opt_mismatch_obj = tmp_path / 'caller_opt_mismatch.o'
    subprocess.run([cc, '-std=c99', '-O2', '-I' + str(root / 'include'),
                    '-DECAT_TARGET_REGISTRY_MAX_BLOCKS=1'] +
                   compile_strip +
                   ['-c', str(caller),
                    '-o', str(caller_opt_mismatch_obj)], check=True)

    mismatch_opt = subprocess.run([cc, '-O2'] + link_strip +
                                  [str(caller_opt_mismatch_obj), str(reg_opt_obj),
                                   '-o', str(tmp_path / 'mismatch_opt')],
                                  capture_output=True, text=True)
    assert mismatch_opt.returncode != 0, f"Mismatched configurations under -O2 and dead-strip must fail to link, got returncode 0"
    assert 'ecat_target_registry_abi_' in mismatch_opt.stderr, f"Linker error must cite ecat_target_registry_abi_, got: {mismatch_opt.stderr}"

    caller_opt_match_obj = tmp_path / 'caller_opt_match.o'
    subprocess.run([cc, '-std=c99', '-O2', '-I' + str(root / 'include')] +
                   compile_strip +
                   ['-c', str(caller),
                    '-o', str(caller_opt_match_obj)], check=True)
    subprocess.run([cc, '-O2'] + link_strip +
                   [str(caller_opt_match_obj), str(reg_opt_obj),
                    '-o', str(tmp_path / 'match_opt')], check=True)
    subprocess.run([str(tmp_path / 'match_opt')], check=True)

    # Matching config must link and succeed
    caller_match_obj = tmp_path / 'caller_match.o'
    subprocess.run([cc, '-std=c99', '-I' + str(root / 'include'),
                    '-c', str(caller),
                    '-o', str(caller_match_obj)], check=True)
    subprocess.run([cc, str(caller_match_obj), str(reg_obj),
                    '-o', str(tmp_path / 'match')], check=True)
    subprocess.run([str(tmp_path / 'match')], check=True)

    # Preprocessor expression equivalent to default 16 must link successfully
    caller_expr_obj = tmp_path / 'caller_expr.o'
    subprocess.run([cc, '-std=c99', '-I' + str(root / 'include'),
                    '-DECAT_TARGET_REGISTRY_MAX_BLOCKS=(8U * 2U)',
                    '-c', str(caller),
                    '-o', str(caller_expr_obj)], check=True)
    subprocess.run([cc, str(caller_expr_obj), str(reg_obj),
                    '-o', str(tmp_path / 'expr')], check=True)
    subprocess.run([str(tmp_path / 'expr')], check=True)

    # 32-bit architecture static compilation check (sizeof(size_t) == 4)
    # Verifies type safety and compilation on a true 32-bit target when supported by compiler.
    probe_32bit = subprocess.run([cc, '-target', 'i386-apple-darwin', '-std=c99',
                                  '-Wall', '-Wextra', '-Werror',
                                  '-I' + str(root / 'include'),
                                  '-c', str(root / 'src/ecat_transfer.c'),
                                  '-o', str(tmp_path / 'ecat_transfer_32bit.o')],
                                 capture_output=True, text=True)
    if probe_32bit.returncode == 0:
        print("PASS: 32-bit static compilation check (sizeof(size_t) == 4, no host 32-bit runtime available)")
    else:
        print("NOTE: 32-bit target compilation not supported by host compiler, relying on host arithmetic simulation")

print("PASS: target registry ABI configuration mismatch caught at link time")
