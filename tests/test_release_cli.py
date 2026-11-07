"""Exercise the built CLI using disposable files, including binary edge bytes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="ecat-release-test-") as directory:
    root = Path(directory)
    payload = bytes(range(256)) * 3 + b"\r\n\x1a\x00\xff"
    (root / "input.bin").write_bytes(payload)

    def run(*args, ok=True):
        result = subprocess.run([str(exe), *args], cwd=root, capture_output=True)
        if (result.returncode == 0) != ok:
            raise AssertionError(f"{args}: exit={result.returncode}, stderr={result.stderr!r}")

    run("--help")
    pack_args = ("pack", "--bin", "input.bin", "--project-id", "0x10010001",
                 "--hardware-id", "1", "--target", "MCU", "--fw-version", "1.0.0.0",
                 "--out", "output.efw")
    run(*pack_args)
    before = (root / "output.efw").read_bytes()
    run("pack", "-i", "input.bin", "-p", "0x10010001", "-d", "1",
        "-t", "MCU", "-v", "1.0.0.0", "-o", "short.efw")
    assert (root / "short.efw").read_bytes() == before
    run(*pack_args, ok=False)
    assert (root / "output.efw").read_bytes() == before
    run("inspect", "output.efw", "--extract-bin", "unpacked.bin")
    assert (root / "unpacked.bin").read_bytes() == payload
    corrupt = bytearray(before)
    corrupt[-1] ^= 1
    (root / "corrupt.efw").write_bytes(corrupt)
    run("inspect", "corrupt.efw", ok=False)
print("Pack, inspect, round trip, corruption and no-overwrite checks passed")
