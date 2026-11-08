#!/usr/bin/env python3
"""
tools/efw_pack.py: EtherCAT .efw 固件容器打包与检查工具 (v0.1)

严格遵循 docs/spec/efw-format-v0.1.md 规范：
- 打包模式 (pack): 根据参数与二进制文件生成规范的 .efw 固件容器
- 检查模式 (inspect/unpack): 解析 .efw 容器头，校验 Header CRC32、Payload CRC32 及各字段合法性
- 支持 --foe-packet-size N 尾部 0xFF 填充 (第 2.2 节)
- 遵循第 7 节文件名规范与双向定宽版本映射规则
"""

import sys
import os
import argparse
import struct
import binascii
import errno
import stat


def safe_open_for_write(path: str, overwrite: bool = False, protected_paths=()):
    """Open a regular output without truncating protected or linked files."""
    if overwrite and not hasattr(os, 'O_NOFOLLOW'):
        raise ValueError('当前平台不支持安全覆盖；请选择不存在的输出路径')
    flags = (os.O_WRONLY | os.O_CREAT | getattr(os, 'O_NOFOLLOW', 0)
             | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NONBLOCK', 0))
    if not overwrite:
        flags |= os.O_EXCL
    try:
        fd = os.open(path, flags, 0o644)
    except OSError as e:
        if e.errno == errno.EEXIST:
            raise FileExistsError(f"安全拒绝：目标文件已存在且未指定覆盖: {path}") from e
        if e.errno == errno.ELOOP or (hasattr(errno, 'EMLINK') and e.errno == errno.EMLINK):
            raise ValueError(f"安全拒绝：目标路径为符号链接: {path}") from e
        raise
    try:
        output = os.fstat(fd)
        if not stat.S_ISREG(output.st_mode):
            raise ValueError('安全拒绝：输出不是普通文件')
        if overwrite:
            if output.st_nlink > 1:
                raise ValueError('安全拒绝：输出有多个硬链接')
            for protected in protected_paths:
                source = os.stat(protected)
                if os.path.samestat(output, source):
                    raise ValueError('安全拒绝：输出与输入是同一文件')
            os.ftruncate(fd, 0)
        return os.fdopen(fd, 'wb')
    except BaseException:
        os.close(fd)
        raise


# 格式规范常量
EFW_MAGIC = 0x57464345  # 'ECFW' little-endian
EFW_FORMAT_VERSION = 1
EFW_HEADER_MIN_LEN = 64
EFW_RESERVED_LEN = 20

TARGET_MAP = {
    "MCU": 0x0001,
    "APP": 0x0001,
    "FPGA": 0x0002,
    "SII": 0x0003,
    "EEPROM": 0x0004,
}

TARGET_ID_TO_NAME = {
    0x0001: "MCU",
    0x0002: "FPGA",
    0x0003: "SII",
    0x0004: "EEPROM",
}


def parse_version_str(v_str: str) -> int:
    """
    将版本号解析为 32 位数值 (Major<<24 | Minor<<16 | Patch<<8 | Build)
    缺段补 0，如 'V1.2' -> 0x01020000
    """
    s = v_str.strip()
    if s.lower().startswith('0x'):
        return int(s, 16)
    if s.isdigit():
        return int(s, 10)
    if s.startswith(('v', 'V')):
        s = s[1:]
    parts = s.split('.')
    if len(parts) > 4:
        raise ValueError(f"版本号段数超过 4: {v_str}")
    vals = [int(p) for p in parts]
    while len(vals) < 4:
        vals.append(0)
    for v in vals:
        if v < 0 or v > 255:
            raise ValueError(f"版本分量超出 0-255 范围: {v}")
    return (vals[0] << 24) | (vals[1] << 16) | (vals[2] << 8) | vals[3]


def format_version_int(val: int) -> str:
    """将 32 位版本号格式化为字符串 Major.Minor.Patch.Build"""
    return f"{(val >> 24) & 0xFF}.{(val >> 16) & 0xFF}.{(val >> 8) & 0xFF}.{val & 0xFF}"


def parse_target_id(target_arg: str) -> int:
    """解析目标名称或 Target ID"""
    t_upper = target_arg.strip().upper()
    if t_upper in TARGET_MAP:
        return TARGET_MAP[t_upper]
    if t_upper.startswith('0X'):
        return int(t_upper, 16)
    if t_upper.isdigit():
        return int(t_upper, 10)
    raise ValueError(f"未知升级目标: {target_arg} (支持: MCU, FPGA, SII, EEPROM)")


def crc32_ieee(data: bytes) -> int:
    """计算 IEEE 802.3 CRC32"""
    return binascii.crc32(data) & 0xFFFFFFFF


def pack_efw(
    project_id: int,
    hardware_id: int,
    hw_ver_min: int,
    hw_ver_max: int,
    target_id: int,
    fw_version: int,
    min_allow_version: int,
    bin_data: bytes,
    project_name: str = "PROJECT",
    foe_packet_size: int = 0,
    out_path: str = None,
    overwrite: bool = False,
    protected_paths=()
) -> str:
    """
    打包 .efw 文件
    """
    if hw_ver_min > hw_ver_max:
        raise ValueError('最低硬件版本不能大于最高硬件版本')
    if min_allow_version > fw_version:
        raise ValueError('最低允许版本不能大于固件版本')
    if target_id not in TARGET_ID_TO_NAME:
        raise ValueError('不支持的升级目标 ID')
    if len(bin_data) == 0:
        raise ValueError('固件有效负载不能为空')
    if foe_packet_size < 0:
        raise ValueError('FoE 单包大小不能为负数')
    header_length = EFW_HEADER_MIN_LEN  # 64
    payload = bytearray(bin_data)

    # 尾部填充处理 (第 2.2 节)
    if foe_packet_size > 0:
        total_len = header_length + len(payload)
        if total_len % foe_packet_size == 0:
            # 恰好为整数倍，执行规避填充
            pad_len = 2 if target_id == 0x0003 else 1
            payload.extend(b'\xFF' * pad_len)

    payload_bytes = bytes(payload)
    payload_length = len(payload_bytes)
    payload_crc = crc32_ieee(payload_bytes)

    flags = 0
    reserved = bytes(EFW_RESERVED_LEN)

    # 构造前 60 字节
    header_prefix = struct.pack(
        '<IHHIIHHHHIIII',
        EFW_MAGIC,
        EFW_FORMAT_VERSION,
        header_length,
        project_id,
        hardware_id,
        hw_ver_min,
        hw_ver_max,
        target_id,
        flags,
        fw_version,
        min_allow_version,
        payload_length,
        payload_crc
    ) + reserved

    # 计算 Header CRC32 并追加到末尾
    header_crc = crc32_ieee(header_prefix)
    header_bytes = header_prefix + struct.pack('<I', header_crc)

    efw_data = header_bytes + payload_bytes

    if not out_path:
        if not project_name or project_name in ('.', '..') or any(c in project_name for c in ('/', '\\', '\0')):
            raise ValueError('项目名不能包含路径分隔符或为空')
        target_name = TARGET_ID_TO_NAME[target_id]
        ver_str = format_version_int(fw_version)
        out_path = f"{project_name}_{target_name}_V{ver_str}.efw"
        if len(os.fsencode(out_path)) > 31:
            raise ValueError('生成的 FoE 文件名超过 31 字节；请缩短项目名')

    with safe_open_for_write(out_path, overwrite=overwrite, protected_paths=protected_paths) as f:
        f.write(efw_data)

    return out_path


def inspect_efw(efw_path: str, extract_bin: str = None, overwrite: bool = False) -> bool:
    """
    解析并验证 .efw 文件
    """
    if not os.path.exists(efw_path):
        print(f"错误: 文件不存在: {efw_path}", file=sys.stderr)
        return False

    with open(efw_path, 'rb') as f:
        data = f.read()

    file_size = len(data)
    if file_size < EFW_HEADER_MIN_LEN:
        print(f"错误: 文件尺寸小于最小头部长度 64 字节 (实际 {file_size} 字节)", file=sys.stderr)
        return False

    magic, format_version, header_length = struct.unpack_from('<IHH', data, 0)
    if magic != EFW_MAGIC:
        print(f"错误: 魔数不匹配: 期望 0x{EFW_MAGIC:08X}, 实际 0x{magic:08X}", file=sys.stderr)
        return False
    if format_version != EFW_FORMAT_VERSION:
        print(f"错误: 规范版本不支持: 期望 {EFW_FORMAT_VERSION}, 实际 {format_version}", file=sys.stderr)
        return False
    if header_length < EFW_HEADER_MIN_LEN or (header_length % 4) != 0:
        print(f"错误: 头部长度非法: {header_length} (必须 >= 64 且为 4 的倍数)", file=sys.stderr)
        return False
    if file_size < header_length:
        print(f"错误: 文件尺寸小于头部声明长度 {header_length} 字节", file=sys.stderr)
        return False

    # Header CRC 校验 (覆盖 0 到 header_length - 4)
    expected_header_crc = struct.unpack_from('<I', data, header_length - 4)[0]
    calc_header_crc = crc32_ieee(data[:header_length - 4])
    if calc_header_crc != expected_header_crc:
        print(f"错误: Header CRC32 不匹配: 期望 0x{expected_header_crc:08X}, 计算 0x{calc_header_crc:08X}", file=sys.stderr)
        return False

    # 解析基础元数据
    (
        project_id,
        hardware_id,
        hw_ver_min,
        hw_ver_max,
        target_id,
        flags,
        fw_version,
        min_allow_version,
        payload_length,
        payload_crc
    ) = struct.unpack_from('<IIHHHHIIII', data, 8)

    reserved = data[40:60]
    if any(b != 0 for b in reserved):
        print("错误: 扩展保留区 reserved 非全零", file=sys.stderr)
        return False
    if flags != 0:
        print(f"错误: flags 标志位非零: 0x{flags:04X}", file=sys.stderr)
        return False

    if hw_ver_min > hw_ver_max or min_allow_version > fw_version:
        print('错误: 硬件或固件版本范围无效', file=sys.stderr)
        return False
    if target_id not in TARGET_ID_TO_NAME:
        print(f'错误: 不支持的升级目标 ID: {target_id}', file=sys.stderr)
        return False
    if payload_length == 0:
        print('错误: 固件有效负载不能为空', file=sys.stderr)
        return False

    # 校验流总长度
    expected_total = header_length + payload_length
    if file_size != expected_total:
        print(f"错误: 文件总大小不符: 期望 {expected_total} 字节, 实际 {file_size} 字节", file=sys.stderr)
        return False

    payload = data[header_length:]
    calc_payload_crc = crc32_ieee(payload)
    if calc_payload_crc != payload_crc:
        print(f"错误: Payload CRC32 不匹配: 期望 0x{payload_crc:08X}, 计算 0x{calc_payload_crc:08X}", file=sys.stderr)
        return False

    target_name = TARGET_ID_TO_NAME.get(target_id, "未知")

    print(f"=== .efw 结构、通用字段及 CRC 校验通过（设备兼容性未验证） ===")
    print(f"  文件路径:          {efw_path}")
    print(f"  文件总大小:        {file_size} 字节")
    print(f"  头部长度:          {header_length} 字节")
    print(f"  Header CRC32:      0x{expected_header_crc:08X} (校验通过)")
    print(f"  Project ID:        0x{project_id:08X}")
    print(f"  Hardware ID:       0x{hardware_id:08X}")
    print(f"  支持硬件版本:      [{hw_ver_min}, {hw_ver_max}]")
    print(f"  升级目标:          {target_id} ({target_name})")
    print(f"  固件版本:          0x{fw_version:08X} (V{format_version_int(fw_version)})")
    print(f"  允许运行下限:      0x{min_allow_version:08X} (V{format_version_int(min_allow_version)})")
    print(f"  Payload 长度:      {payload_length} 字节")
    print(f"  Payload CRC32:     0x{payload_crc:08X} (校验通过)")

    if extract_bin:
        with safe_open_for_write(extract_bin, overwrite=overwrite, protected_paths=(efw_path,)) as f:
            f.write(payload)
        print(f"  提取有效负载至:    {extract_bin}")

    return True


def main():
    # Frozen Windows executables can inherit a non-Chinese ANSI code page.
    # Keep console output usable even when redirection chooses another encoding.
    if os.name == "nt":
        for stream in (sys.stdout, sys.stderr):
            if hasattr(stream, "reconfigure"):
                stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="EtherCAT .efw 固件容器工具")
    subparsers = parser.add_subparsers(dest="command")

    # pack 子命令
    pack_p = subparsers.add_parser("pack", help="打包 .efw 固件文件")
    pack_p.add_argument("-i", "--bin", required=True, help="输入原始二进制固件文件 (.bin)")
    pack_p.add_argument("-p", "--project-id", required=True, help="工程唯一数字 ID (如 0x10010001)")
    pack_p.add_argument("-n", "--project-name", default="PROJECT", help="工程名称字符串 (默认 PROJECT)")
    pack_p.add_argument("-d", "--hardware-id", required=True, help="硬件板型 ID (如 0x00000001)")
    pack_p.add_argument("--hw-ver-min", type=int, default=1, help="支持的最低硬件版本 (默认 1)")
    pack_p.add_argument("--hw-ver-max", type=int, default=1, help="支持的最高硬件版本 (默认 1)")
    pack_p.add_argument("-t", "--target", required=True, help="升级目标 (MCU, FPGA, SII, EEPROM 或数字 ID)")
    pack_p.add_argument("-v", "--fw-version", required=True, help="固件版本 (如 1.2.3.4 或 0x01020304)")
    pack_p.add_argument("--min-allow-version", default="0", help="防回滚允许运行下限 (默认 0)")
    pack_p.add_argument("--foe-packet-size", type=int, default=0, help="FoE 单包大小 (如 512)，用于空包规避填充")
    pack_p.add_argument("--out", "-o", default=None, help="输出 .efw 文件路径 (默认按规范自动命名)")
    pack_p.add_argument("--overwrite", action="store_true", default=False, help="允许覆盖已有普通文件")

    # inspect / unpack 子命令
    inspect_p = subparsers.add_parser("inspect", help="检验并打印 .efw 容器元数据")
    inspect_p.add_argument("efw_file", help="待检验的 .efw 文件")
    inspect_p.add_argument("--extract-bin", default=None, help="提取 payload 至指定文件")
    inspect_p.add_argument("--overwrite", action="store_true", default=False, help="允许覆盖已有提取文件")

    unpack_p = subparsers.add_parser("unpack", help="解包 .efw 文件 (等价于 inspect)")
    unpack_p.add_argument("efw_file", help="待解包的 .efw 文件")
    unpack_p.add_argument("--extract-bin", default=None, help="提取 payload 至指定文件")
    unpack_p.add_argument("--overwrite", action="store_true", default=False, help="允许覆盖已有提取文件")

    args = parser.parse_args()

    if args.command == "pack":
        with open(args.bin, 'rb') as f:
            bin_data = f.read()

        p_id = int(args.project_id, 0)
        h_id = int(args.hardware_id, 0)
        t_id = parse_target_id(args.target)
        fw_ver = parse_version_str(args.fw_version)
        min_ver = parse_version_str(args.min_allow_version)

        out_file = pack_efw(
            project_id=p_id,
            hardware_id=h_id,
            hw_ver_min=args.hw_ver_min,
            hw_ver_max=args.hw_ver_max,
            target_id=t_id,
            fw_version=fw_ver,
            min_allow_version=min_ver,
            bin_data=bin_data,
            project_name=args.project_name,
            foe_packet_size=args.foe_packet_size,
            out_path=args.out,
            overwrite=args.overwrite,
            protected_paths=(args.bin,)
        )
        print(f"成功打包 .efw 文件: {out_file}")

    elif args.command in ("inspect", "unpack"):
        ok = inspect_efw(args.efw_file, extract_bin=args.extract_bin, overwrite=args.overwrite)
        sys.exit(0 if ok else 1)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, struct.error) as exc:
        print(f"错误: {exc}", file=sys.stderr)
        sys.exit(1)
