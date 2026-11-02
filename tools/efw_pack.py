#!/usr/bin/env python3
"""
@file efw_pack.py
@brief EtherCAT FoE 标准固件容器 (.efw) 打包命令行工具

用于将编译器生成的原始固件 (.bin) 打包为带有规范元数据、双重识别与完整性校验的 .efw 文件。
"""

import argparse
import os
import struct
import sys
import zlib

ECAT_EFW_MAGIC = 0x31574645  # "EFW1"
ECAT_EFW_HEADER_VERSION = 1
ECAT_EFW_HEADER_SIZE = 84

TARGET_MAP = {
    "APP": 1,
    "FPGA": 2,
    "EEPROM": 3,
}


def parse_semver(version_str: str) -> int:
    """解析 x.y.z 版本号为 32 位整型 (Major<<24 | Minor<<16 | Patch<<8 | Build)"""
    parts = version_str.strip().split(".")
    major = int(parts[0]) if len(parts) > 0 and parts[0].isdigit() else 0
    minor = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
    patch = int(parts[2]) if len(parts) > 2 and parts[2].isdigit() else 0
    build = int(parts[3]) if len(parts) > 3 and parts[3].isdigit() else 0
    return ((major & 0xFF) << 24) | ((minor & 0xFF) << 16) | ((patch & 0xFF) << 8) | (build & 0xFF)


def pack_efw(bin_path: str, project_id: int, project_name: str,
             hardware_id: int, target_name: str, version_str: str,
             output_path: str = None) -> str:
    if not os.path.isfile(bin_path):
        raise FileNotFoundError(f"Input binary not found: {bin_path}")

    target_name_upper = target_name.upper()
    if target_name_upper not in TARGET_MAP:
        raise ValueError(f"Unsupported target: {target_name}. Choose from {list(TARGET_MAP.keys())}")
    target_id = TARGET_MAP[target_name_upper]

    with open(bin_path, "rb") as f:
        payload = f.read()

    payload_size = len(payload)
    if payload_size == 0:
        raise ValueError("Input binary payload is empty")

    payload_crc = zlib.crc32(payload) & 0xFFFFFFFF
    version_raw = parse_semver(version_str)

    proj_name_bytes = project_name.encode("utf-8")[:31].ljust(32, b"\x00")
    ver_str_bytes = version_str.encode("utf-8")[:15].ljust(16, b"\x00")

    # 预打包文件头 (header_crc 先置 0)
    fmt = "<IHHIIHHI32s16sIII"
    header_no_crc = struct.pack(
        fmt,
        ECAT_EFW_MAGIC,
        ECAT_EFW_HEADER_VERSION,
        ECAT_EFW_HEADER_SIZE,
        project_id,
        hardware_id,
        target_id,
        0,  # flags
        version_raw,
        proj_name_bytes,
        ver_str_bytes,
        payload_size,
        payload_crc,
        0   # header_crc placeholder
    )

    header_crc = zlib.crc32(header_no_crc) & 0xFFFFFFFF

    # 最终打包
    header_final = struct.pack(
        fmt,
        ECAT_EFW_MAGIC,
        ECAT_EFW_HEADER_VERSION,
        ECAT_EFW_HEADER_SIZE,
        project_id,
        hardware_id,
        target_id,
        0,  # flags
        version_raw,
        proj_name_bytes,
        ver_str_bytes,
        payload_size,
        payload_crc,
        header_crc
    )

    if not output_path:
        output_path = f"{project_name}_{target_name_upper}_{version_str}.efw"

    with open(output_path, "wb") as f:
        f.write(header_final)
        f.write(payload)

    return output_path


def main():
    parser = argparse.ArgumentParser(description="EtherCAT FoE .efw Firmware Pack Tool")
    parser.add_argument("--bin", required=True, help="Path to input raw binary file (.bin)")
    parser.add_argument("--project-id", required=True, type=lambda x: int(x, 0), help="Project ID (e.g. 0x10010001)")
    parser.add_argument("--project-name", required=True, help="Project name string (e.g. SERVO_DRV)")
    parser.add_argument("--hw-id", required=True, type=lambda x: int(x, 0), help="Hardware ID (e.g. 0x00000001)")
    parser.add_argument("--target", required=True, choices=["APP", "FPGA", "EEPROM"], help="Target type")
    parser.add_argument("--version", required=True, help="Version string (e.g. 1.0.0)")
    parser.add_argument("-o", "--output", help="Output .efw file path (default: <PROJECT>_<TARGET>_<VER>.efw)")

    args = parser.parse_args()

    try:
        out = pack_efw(
            bin_path=args.bin,
            project_id=args.project_id,
            project_name=args.project_name,
            hardware_id=args.hw_id,
            target_name=args.target,
            version_str=args.version,
            output_path=args.output
        )
        print(f"Successfully generated .efw package: {out}")
    except Exception as e:
        print(f"Error packing .efw: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
