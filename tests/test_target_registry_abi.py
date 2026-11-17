#!/usr/bin/env python3
"""Check that mismatched target registry buffer configurations fail at link time."""
import os
import pathlib
import subprocess
import sys
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

    # Stale v1 ABI object must fail to link against v2 registry layout
    import shutil
    old_abi_dir = tmp_path / 'old_abi_include'
    shutil.copytree(root / 'include', old_abi_dir)
    abi_src = (old_abi_dir / 'ecat_target_registry_abi.h').read_text(encoding='utf-8')
    assert 'ecat_target_registry_abi_v2_b' in abi_src
    (old_abi_dir / 'ecat_target_registry_abi.h').write_text(
        abi_src.replace('ecat_target_registry_abi_v2_b', 'ecat_target_registry_abi_b'),
        encoding='utf-8',
    )
    stale_caller_obj = tmp_path / 'caller_stale_v1.o'
    subprocess.run([cc, '-std=c99', '-I' + str(old_abi_dir),
                    '-c', str(caller),
                    '-o', str(stale_caller_obj)], check=True)
    stale_link = subprocess.run([cc, str(stale_caller_obj), str(reg_obj),
                                 '-o', str(tmp_path / 'stale_v1')],
                                capture_output=True, text=True)
    assert stale_link.returncode != 0, f"Stale v1 registry ABI object must fail to link, got returncode 0"
    assert 'ecat_target_registry_abi_' in stale_link.stderr, f"Linker error must cite ecat_target_registry_abi_, got: {stale_link.stderr}"

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
    # 1. First probe whether compiler and system headers support 32-bit cross-compilation with a minimal probe.
    probe_src = tmp_path / 'probe_32bit_env.c'
    probe_src.write_text(
        '#include <stddef.h>\n'
        '#include <stdint.h>\n'
        '#include <stdbool.h>\n'
        '#include <string.h>\n'
        'typedef char assert_32bit_size_t[(sizeof(size_t) == 4) ? 1 : -1];\n'
        'typedef char assert_32bit_ptr[(sizeof(void *) == 4) ? 1 : -1];\n'
        'int probe_env_entry(void) { return 0; }\n'
    )
    probe_obj = tmp_path / 'probe_32bit_env.o'
    probe_32bit = subprocess.run([cc, '-target', 'i386-apple-darwin', '-std=c99',
                                  '-c', str(probe_src),
                                  '-o', str(probe_obj)],
                                 capture_output=True, text=True)

    if probe_32bit.returncode != 0:
        # Toolchain or system headers do not support 32-bit targets.
        reason = probe_32bit.stderr.strip().replace('\n', ' ')
        print(f"SKIP: 32-bit cross-compilation environment not supported by host toolchain/headers (reason: {reason})")
    else:
        # Supported environment confirmed: verify that deliberate 32-bit compilation failures are NOT swallowed.
        bad_src = tmp_path / 'intentional_32bit_fail.c'
        bad_src.write_text(
            '#include <stddef.h>\n'
            'int bad_entry(void) {\n'
            '    char a[(sizeof(size_t) == 8) ? 1 : -1]; /* Deliberately invalid on 32-bit */\n'
            '    return (int)sizeof(a);\n'
            '}\n'
        )
        bad_res = subprocess.run([cc, '-target', 'i386-apple-darwin', '-std=c99',
                                  '-c', str(bad_src),
                                  '-o', str(tmp_path / 'bad_32bit.o')],
                                 capture_output=True, text=True)
        assert bad_res.returncode != 0, "FATAL: Compiler unexpectedly passed deliberate 32-bit failure!"

        # Compile project source file: failures in a supported environment MUST be surfaced and fail the test.
        proj_32bit = subprocess.run([cc, '-target', 'i386-apple-darwin', '-std=c99',
                                     '-Wall', '-Wextra', '-Werror',
                                     '-I' + str(root / 'include'),
                                     '-c', str(root / 'src/ecat_transfer.c'),
                                     '-o', str(tmp_path / 'ecat_transfer_32bit.o')],
                                    capture_output=True, text=True)
        if proj_32bit.returncode != 0:
            print("FATAL: 32-bit project compilation failed in supported environment!", file=sys.stderr)
            print(proj_32bit.stderr, file=sys.stderr)
            sys.exit(1)

        print("PASS: 32-bit static compilation check (sizeof(size_t) == 4, static compile only; no host 32-bit runtime available)")

print("PASS: target registry ABI configuration mismatch caught at link time")
