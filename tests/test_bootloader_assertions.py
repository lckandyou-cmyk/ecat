#!/usr/bin/env python3
"""A test binary must reject builds that disable its assertions."""
import os
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
tmp = tempfile.TemporaryDirectory(prefix='ecat-assert-')
(pathlib.Path(tmp.name) / 'esc.h').write_text('')
command = [os.environ.get('CC', 'cc'), '-E', '-DNDEBUG', '-I' + tmp.name,
           '-I' + str(root / 'include'), '-I' + str(root / 'tests/stubs'),
           '-I' + str(root / 'examples/bootloader_soes'),
           str(root / 'examples/bootloader_soes/test_bootloader_integration.c')]
result = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
assert result.returncode != 0 and 'bootloader integration tests require assertions' in result.stderr, result.stderr
print('PASS: bootloader integration tests reject NDEBUG')
