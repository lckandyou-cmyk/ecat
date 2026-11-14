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

print("PASS: target registry ABI configuration mismatch caught at link time")
