#!/usr/bin/env python3
"""Build and link the public library with FoE enabled and disabled."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = shlex.split(os.environ.get('CC', 'cc'))
AR = shlex.split(os.environ.get('AR', 'ar'))

with tempfile.TemporaryDirectory(prefix='upgrade-build-') as tmp:
    work = Path(tmp)
    app = work / 'app.c'
    app.write_text('#include "ecat_transfer.h"\n'
                   'int port_get_version_floor(uint16_t t, uint32_t *v) { (void)t; (void)v; return 0; }\n'
                   'int main(void) { return ecat_transfer_init(0, 0, 0) == -1 ? 0 : 1; }\n')
    foe = work / 'foe.c'
    foe.write_text('#include "ecat_foe_ssc.h"\n'
                   'int port_get_version_floor(uint16_t t, uint32_t *v) { (void)t; (void)v; return 0; }\n'
                   'int main(void) { ecat_foe_ssc_init(0, 0, 0); return 0; }\n')
    # Firmware policy events are internal; applications enter via transfer APIs.
    for symbol in ('ecat_foe_core_on_wrq', 'ecat_foe_core_begin_request',
                   'ecat_foe_core_on_data', 'ecat_foe_core_on_end',
                   'ecat_foe_core_on_abort'):
        probe = work / (symbol + '.c')
        probe.write_text('#include "ecat_foe_core.h"\n'
                         'void *probe(void) { return (void *)&' + symbol + '; }\n')
        result = subprocess.run(CC + ['-std=c99', '-I' + str(ROOT / 'include'),
                                      '-fsyntax-only', str(probe)],
                                capture_output=True, text=True)
        assert result.returncode != 0 and symbol in result.stderr, result.stderr
    for symbol in ('ecat_foe_core_async_abort', 'ecat_foe_core_async_ops'):
        async_probe = work / (symbol + '.c')
        async_probe.write_text('#include "ecat_foe_core_async.h"\n'
                               'void *probe(void) { return (void *)&' + symbol + '; }\n')
        result = subprocess.run(CC + ['-std=c99', '-I' + str(ROOT / 'include'),
                                      '-fsyntax-only', str(async_probe)],
                                capture_output=True, text=True)
        assert result.returncode != 0 and symbol in result.stderr, result.stderr
    for mode in ('default', 'off', 'file', 'shared'):
        out = work / mode
        command = ['make', 'build-upgrade', 'UPGRADE_BUILD_DIR=' + str(out),
                   'CC=' + os.environ.get('CC', 'cc')]
        if mode in ('off', 'file'):
            command.append('ENABLE_FOE=0')
        if mode in ('file', 'shared'):
            command.append('ENABLE_FILE=1')
        built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        assert built.returncode == 0, built.stdout + built.stderr
        library = out / 'libecat_upgrade.a'
        members = subprocess.check_output(AR + ['t', str(library)], text=True)
        assert ('ecat_foe_ssc.o' in members) == (mode in ('default', 'shared')), members
        assert ('ecat_foe_transport.o' in members) == (mode in ('default', 'shared')), members
        assert 'ecat_foe_soes.o' not in members
        assert ('ecat_transfer_file.o' in members) == (mode in ('file', 'shared')), members
        executable = work / ('app-' + mode)
        subprocess.run(CC + ['-std=c99', '-I' + str(ROOT / 'include'), str(app),
                             str(library), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
        linked = subprocess.run(CC + ['-std=c99', '-I' + str(ROOT / 'include'), str(foe),
                                      str(library), '-o', str(work / ('foe-' + mode))],
                                capture_output=True, text=True)
        if mode in ('default', 'shared'):
            assert linked.returncode == 0, linked.stderr
        else:
            assert linked.returncode != 0 and 'ecat_foe_ssc_init' in linked.stderr, linked.stderr
    file_app = work / 'file_app.c'
    file_app.write_text('#include "ecat_transfer_file.h"\n'
                        'int port_get_version_floor(uint16_t t, uint32_t *v) { (void)t; (void)v; return 0; }\n'
                        'int main(void) { return ecat_transfer_file_init(0, 0, 0, 0) == -1 ? 0 : 1; }\n')
    for mode in ('default', 'off', 'file', 'shared'):
        result = subprocess.run(CC + ['-std=c99', '-I' + str(ROOT / 'include'), str(file_app),
                                      str(work / mode / 'libecat_upgrade.a'), '-o', str(work / ('file-app-' + mode))],
                                capture_output=True, text=True)
        assert (result.returncode == 0) == (mode in ('file', 'shared')), result.stderr
    # User output is never overwritten by a second invocation.
    original = (work / 'default/libecat_upgrade.a').read_bytes()
    again = subprocess.run(['make', 'build-upgrade', 'UPGRADE_BUILD_DIR=' + str(work / 'default')],
                           cwd=ROOT, capture_output=True, text=True)
    assert again.returncode != 0
    assert (work / 'default/libecat_upgrade.a').read_bytes() == original
print('PASS: FoE/file feature matrix, standalone links and output preservation')
