#!/usr/bin/env python3
"""SOES baseline checking must distinguish strict and advisory modes."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/check_soes_hash.py'
FILES = ('esc.c', 'esc_foe.c', 'esc.h', 'esc_foe.h')

with tempfile.TemporaryDirectory(prefix='soes-hash-test-') as tmp:
    root = pathlib.Path(tmp)
    sums = {}
    for name in FILES:
        content = name.encode()
        (root / name).write_bytes(content)
        sums[name] = hashlib.sha256(content).hexdigest()
    manifest = root / 'manifest.json'
    manifest.write_text(json.dumps({'file_checksums': sums}))
    def run(*args):
        return subprocess.run([sys.executable, str(SCRIPT), '--soes-dir', str(root),
                               '--manifest', str(manifest), *args], capture_output=True, text=True)
    assert run().returncode == 0
    strict = run('--strict')
    assert strict.returncode == 0, strict.stderr
    (root / 'esc.c').write_text('modified')
    advisory = run()
    assert advisory.returncode == 0 and '[WARNING]' in advisory.stdout
    strict = run('--strict')
    assert strict.returncode != 0 and '[WARNING]' in strict.stdout
    (root / 'esc.c').unlink()
    assert run('--strict').returncode != 0
    (root / 'esc.c').write_bytes(b'esc.c')
    del sums['esc.c']
    manifest.write_text(json.dumps({'file_checksums': sums}))
    assert run('--strict').returncode != 0
    manifest.unlink()
    assert run('--strict').returncode != 0
print('PASS: SOES hash strict and advisory modes')
