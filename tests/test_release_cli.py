"""Exercise the built CLI using disposable files, including binary edge bytes."""
import os
import binascii
import struct
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
        return result

    help_result = run("--help")
    assert b"info" in help_result.stdout
    version_result = run("--version")
    version_line = version_result.stdout.decode("utf-8").strip()
    assert version_line.startswith("ecat-efw ") and "(built " in version_line
    build_date = version_line.rsplit("(built ", 1)[1].removesuffix(")")
    assert len(build_date) == 14 and build_date.endswith(" UTC") and build_date[:10][4] == "-" and build_date[:10][7] == "-"
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
    renamed = root / "renamed-firmware.dat"
    (root / "output.efw").replace(renamed)
    info_result = run("info", str(renamed))
    assert b"V1.0.0.0" in info_result.stdout
    assert b"Header CRC32" in info_result.stdout and b"Payload CRC32" in info_result.stdout
    assert b"not provided" in info_result.stdout.lower() or "未提供".encode("utf-8") in info_result.stdout
    (root / "output.efw").write_bytes(before)
    corrupt = bytearray(before)
    corrupt[-1] ^= 1
    (root / "corrupt.efw").write_bytes(corrupt)
    run("inspect", "corrupt.efw", ok=False)
    run("info", "corrupt.efw", ok=False)
    # Reject self-contradictory metadata at pack and inspect, before touching files.
    base = ("pack", "-i", "input.bin", "-p", "1", "-d", "1", "-t", "MCU", "-v", "1.0.0.0")
    run(*base, "--hw-ver-min", "9", "--hw-ver-max", "1", "-o", "bad-range.efw", ok=False)
    run(*base, "--min-allow-version", "2.0.0.0", "-o", "bad-version.efw", ok=False)
    run("pack", "-i", "input.bin", "-p", "1", "-d", "1", "-t", "99",
        "-v", "1.0.0.0", "-o", "bad-target.efw", ok=False)
    run(*base, "-n", "../escape", ok=False)
    run(*base, "-o", "input.bin", "--overwrite", ok=False)
    assert (root / "input.bin").read_bytes() == payload
    run("inspect", "output.efw", "--extract-bin", "output.efw", "--overwrite", ok=False)
    assert (root / "output.efw").read_bytes() == before
    invalid = bytearray(before)
    struct.pack_into("<HH", invalid, 16, 9, 1)
    struct.pack_into("<I", invalid, 144, binascii.crc32(invalid[:144]) & 0xffffffff)
    (root / "invalid-range.efw").write_bytes(invalid)
    run("inspect", "invalid-range.efw", ok=False)
    if os.name != 'nt':
        os.link(root / "input.bin", root / "hardlink.bin")
        run(*base, "-o", "hardlink.bin", "--overwrite", ok=False)
        assert (root / "input.bin").read_bytes() == payload
print("Pack, inspect, round trip, corruption and no-overwrite checks passed")
