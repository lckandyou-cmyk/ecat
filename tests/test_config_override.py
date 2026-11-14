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
print('PASS: custom core configuration controls public buffer size')
