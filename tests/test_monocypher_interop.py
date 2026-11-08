#!/usr/bin/env python3
"""Generate CLI packages and run the externally linked Monocypher core test."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: test_monocypher_interop.py <compiled-harness>')
    harness = Path(sys.argv[1]).resolve()
    tool = Path(__file__).resolve().parents[1] / 'tools' / 'efw_pack.py'
    with tempfile.TemporaryDirectory(prefix='ecat-monocypher-interop-') as tmp:
        root = Path(tmp)
        (root / 'seed').write_bytes(bytes(range(32)))  # Test-only deterministic key.
        (root / 'payload').write_bytes(bytes((i * 13 + 7) & 0xff for i in range(257)))
        common = [sys.executable, str(tool), 'pack', '--bin', str(root / 'payload'),
                  '--project-id', '0x10010001', '--hardware-id', '1', '--target', 'MCU',
                  '--fw-version', '1.2.3.4']
        subprocess.run(common + ['--signing-key', str(root / 'seed'),
                                 '--public-key-out', str(root / 'public'),
                                 '--out', str(root / 'signed.efw')], check=True)
        subprocess.run(common + ['--out', str(root / 'unsigned.efw')], check=True)
        subprocess.run([str(harness), str(root / 'signed.efw'),
                        str(root / 'unsigned.efw'), str(root / 'public')], check=True)


if __name__ == '__main__':
    main()
