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
                    '-I' + str(root / 'src'),
                                     '-c', str(root / 'src/ecat_transfer.c'),
                                     '-o', str(tmp_path / 'ecat_transfer_32bit.o')],
                                    capture_output=True, text=True)
        if proj_32bit.returncode != 0:
            print("FATAL: 32-bit project compilation failed in supported environment!", file=sys.stderr)
            print(proj_32bit.stderr, file=sys.stderr)
            sys.exit(1)

        print("PASS: 32-bit static compilation check (sizeof(size_t) == 4, static compile only; no host 32-bit runtime available)")

    # Verify >32-slot multi-byte target_incomplete_slots bitmap (MAX_BLOCKS=40) without shift UB
    bitmap_test = tmp_path / 'test_40_slots.c'
    bitmap_test.write_text(
        '#include "ecat_foe_core_internal.h"\n'
        '#include "ecat_foe_efw.h"\n'
        '#include <assert.h>\n'
        '#include <stdio.h>\n'
        '#include <string.h>\n'
        'static uint8_t s_mem[64];\n'
        'static int d_init(void *c) { (void)c; return 0; }\n'
        'static int d_inv(void *c) { (void)c; return 0; }\n'
        'static int d_erase(uint32_t need, uint32_t *out, void *c) { (void)c; *out = need; return 0; }\n'
        'static int d_write(uint32_t off, const uint8_t *d, uint32_t n, void *c) { (void)c; memcpy(s_mem + off, d, n); return 0; }\n'
        'static int d_read(uint32_t off, uint8_t *d, uint32_t n, void *c) { (void)c; memcpy(d, s_mem + off, n); return 0; }\n'
        'static int d_fin(void *c) { (void)c; return 0; }\n'
        'static void d_abort(void *c) { (void)c; }\n'
        'int port_get_version_floor(uint16_t id, uint32_t *v) { (void)id; if (v) *v = 0; return 0; }\n'
        'int main(void) {\n'
        '    typedef char check_bmp[sizeof(((ecat_foe_core_t *)0)->target_incomplete_slots) == 5 ? 1 : -1];\n'
        '    (void)sizeof(check_bmp);\n'
        '    ecat_target_registry_t reg;\n'
        '    assert(ecat_target_registry_init(&reg) == 0);\n'
        '    for (unsigned i = 0; i < 36U; i++) {\n'
        '        ecat_target_desc_t d;\n'
        '        memset(&d, 0, sizeof(d));\n'
        '        d.target_id = (uint16_t)(1000U + i * 100U);\n'
        '        snprintf(d.name, sizeof(d.name), "slot_%02u", i);\n'
        '        d.type = ECAT_TARGET_TYPE_FIRMWARE;\n'
        '        d.permissions = ECAT_PERM_WRITE;\n'
        '        d.max_size = 64U;\n'
        '        d.write_unit = 8U;\n'
        '        d.enabled = true;\n'
        '        d.driver.io.init = d_init;\n'
        '        d.driver.io.invalidate = d_inv;\n'
        '        d.driver.io.erase_to = d_erase;\n'
        '        d.driver.io.write = d_write;\n'
        '        d.driver.io.read = d_read;\n'
        '        d.driver.io.finalize = d_fin;\n'
        '        d.driver.io.abort = d_abort;\n'
        '        assert(ecat_target_registry_add(&reg, &d) == 0);\n'
        '    }\n'
        '    ecat_foe_core_cfg_t cfg;\n'
        '    memset(&cfg, 0, sizeof(cfg));\n'
        '    cfg.project_name = "BMP40";\n'
        '    cfg.project_id = 1U;\n'
        '    cfg.hardware_id = 1U;\n'
        '    cfg.local_hw_ver = 1U;\n'
        '    cfg.max_header_size = ECAT_FOE_EFW_HEADER_MIN_LEN;\n'
        '    cfg.registry = &reg;\n'
        '    ecat_foe_core_t core;\n'
        '    assert(ecat_foe_core_init(&core, &cfg) == 0);\n'
        '    uint16_t slot35_tid = (uint16_t)(1000U + 35U * 100U);\n'
        '    uint8_t pkt[ECAT_FOE_EFW_HEADER_MIN_LEN + 8U];\n'
        '    memset(pkt, 0, sizeof(pkt));\n'
        '    pkt[0]=0x45; pkt[1]=0x43; pkt[2]=0x46; pkt[3]=0x57;\n'
        '    pkt[4]=0x02; pkt[6]=0x94;\n'
        '    pkt[8]=1; pkt[12]=1; pkt[16]=1; pkt[18]=1;\n'
        '    pkt[20]=(uint8_t)(slot35_tid & 0xFFU); pkt[21]=(uint8_t)(slot35_tid >> 8);\n'
        '    pkt[24]=1; pkt[32]=8;\n'
        '    uint32_t pcrc = ecat_foe_crc32_calc(pkt + ECAT_FOE_EFW_HEADER_MIN_LEN, 8U);\n'
        '    pkt[36]=(uint8_t)pcrc; pkt[37]=(uint8_t)(pcrc>>8); pkt[38]=(uint8_t)(pcrc>>16); pkt[39]=(uint8_t)(pcrc>>24);\n'
        '    memcpy(pkt + 60, "SIG2", 4); pkt[64]=1;\n'
        '    uint32_t hcrc = ecat_foe_crc32_calc(pkt, 144U);\n'
        '    pkt[144]=(uint8_t)hcrc; pkt[145]=(uint8_t)(hcrc>>8); pkt[146]=(uint8_t)(hcrc>>16); pkt[147]=(uint8_t)(hcrc>>24);\n'
        '    ecat_upgrade_request_t req = { slot35_tid, 1U };\n'
        '    assert(ecat_foe_core_begin_request(&core, &req) == ECAT_FOE_CORE_OK);\n'
        '    assert(ecat_foe_core_on_data(&core, pkt, ECAT_FOE_EFW_HEADER_MIN_LEN) == 0);\n'
        '    ecat_foe_core_on_abort(&core);\n'
        '    assert(ecat_foe_core_upgrade_incomplete(&core));\n'
        '    assert(ecat_foe_core_target_upgrade_incomplete(&core, slot35_tid));\n'
        '    assert(!ecat_foe_core_target_upgrade_incomplete(&core, 1000U));\n'
        '    assert(ecat_foe_core_begin_request(&core, &req) == ECAT_FOE_CORE_OK);\n'
        '    assert(ecat_foe_core_on_data(&core, pkt, sizeof(pkt)) == 0);\n'
        '    assert(ecat_foe_core_on_end(&core) == 0);\n'
        '    assert(!ecat_foe_core_upgrade_incomplete(&core));\n'
        '    assert(!ecat_foe_core_target_upgrade_incomplete(&core, slot35_tid));\n'
        '    return 0;\n'
        '}\n',
        encoding='utf-8',
    )
    bmp_bin = tmp_path / 'test_40_slots'
    subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-DECAT_TARGET_REGISTRY_MAX_BLOCKS=40',
                    '-I' + str(root / 'include'),
                    '-I' + str(root / 'src'),
                    str(bitmap_test),
                    str(root / 'src/ecat_foe_efw.c'),
                    str(root / 'src/ecat_foe_core.c'),
                    str(root / 'src/ecat_target_registry.c'),
                    '-o', str(bmp_bin)], check=True)
    subprocess.run([str(bmp_bin)], check=True)

print("PASS: target registry ABI configuration mismatch caught at link time")
