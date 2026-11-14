#!/usr/bin/env python3
"""Check that an application-provided config controls public core layout."""
import os
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ecat-config-') as tmp:
    tmp_path = pathlib.Path(tmp)
    config = (root / 'include/ecat_foe_conf_template.h').read_text()
    config = config.replace('#define ECAT_FOE_MAX_WRITE_UNIT             256U',
                            '#define ECAT_FOE_MAX_WRITE_UNIT             512U')
    (tmp_path / 'ecat_foe_conf.h').write_text(config)
    source = tmp_path / 'check.c'
    source.write_text('#include "ecat_foe_core.h"\n'
                      'typedef char check_size[sizeof(((ecat_foe_core_t *)0)->residual_buf) == 512 ? 1 : -1];\n')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-I' + str(root / 'include'),
                    '-I' + str(tmp_path), '-DECAT_FOE_CONFIG_HEADER="ecat_foe_conf.h"',
                    '-c', str(source), '-o', str(tmp_path / 'check.o')], check=True)
    # Every TU must select the custom config even when the template is seen first.
    first = tmp_path / 'template_first.c'
    first.write_text('#include "ecat_foe_conf_template.h"\n'
                     '#include "ecat_foe_core.h"\n'
                     'typedef char check_size[sizeof(((ecat_foe_core_t *)0)->residual_buf) == 512 ? 1 : -1];\n')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Werror',
                    '-I' + str(root / 'include'), '-I' + str(tmp_path),
                    '-DECAT_FOE_CONFIG_HEADER="ecat_foe_conf.h"', '-c', str(first),
                    '-o', str(tmp_path / 'template_first.o')], check=True)
    caller = tmp_path / 'caller.c'
    caller.write_text('#include "ecat_foe_core.h"\n'
                      'int port_get_version_floor(uint16_t id, uint32_t *v) { (void)id; (void)v; return 0; }\n'
                      'int main(void) { ecat_foe_core_t core; return ecat_foe_core_init(&core, 0); }\n')
    cc = os.environ.get('CC', 'cc')
    flags = [cc, '-std=c99', '-I' + str(root / 'include'), '-I' + str(tmp_path)]
    for name, source_path, extra in [
            ('caller', caller, []),
            ('core', root / 'src/ecat_foe_core.c', ['-DECAT_FOE_CONFIG_HEADER="ecat_foe_conf.h"']),
            ('efw', root / 'src/ecat_foe_efw.c', [])]:
        subprocess.run(flags + extra + ['-c', str(source_path), '-o', str(tmp_path / (name + '.o'))], check=True)
    mismatch = subprocess.run([cc, *(str(tmp_path / (name + '.o')) for name in ('caller', 'core', 'efw')),
                               '-o', str(tmp_path / 'mismatch')], capture_output=True, text=True)
    assert mismatch.returncode != 0, 'mismatched core and caller configurations must fail to link'
    assert 'ecat_foe_config_abi_' in mismatch.stderr, mismatch.stderr
    subprocess.run(flags + ['-DECAT_FOE_CONFIG_HEADER="ecat_foe_conf.h"', '-c', str(caller),
                            '-o', str(tmp_path / 'custom_caller.o')], check=True)
    subprocess.run([cc, *(str(tmp_path / (name + '.o')) for name in ('custom_caller', 'core', 'efw')),
                    '-o', str(tmp_path / 'matching')], check=True)
    # A TU need not call init: accessing the public layout must still carry the ABI reference.
    observer = tmp_path / 'observer.c'
    observer.write_text('#include "ecat_foe_core.h"\n'
                        'int observe(ecat_foe_core_t *core) { return (int)core->residual_len; }\n')
    subprocess.run(flags + ['-c', str(observer), '-o', str(tmp_path / 'observer.o')], check=True)
    mixed = subprocess.run([cc, *(str(tmp_path / (name + '.o')) for name in ('custom_caller', 'core', 'efw', 'observer')),
                            '-o', str(tmp_path / 'mixed')], capture_output=True, text=True)
    assert mixed.returncode != 0 and 'ecat_foe_config_abi_' in mixed.stderr, mixed.stderr
print('PASS: custom core configuration controls public buffer size')
