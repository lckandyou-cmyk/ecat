#!/usr/bin/env python3
"""
tools/efw_pack.py: EtherCAT .efw 固件容器打包与检查工具 (format v2)

严格遵循 docs/spec/efw-format-v0.2.md 规范：
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

try:
    from ecat_build_info import BUILD_DATE, VERSION
except ImportError:
    # Source checkouts remain runnable without generated release metadata.
    VERSION = "development"
    BUILD_DATE = "unavailable"


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


def _remove_created_file(path: str, identity):
    """Best-effort cleanup only if the path still names the file we created."""
    try:
        current = os.lstat(path)
        if stat.S_ISREG(current.st_mode) and os.path.samestat(identity, current):
            os.unlink(path)
    except OSError:
        pass


# 格式规范常量
EFW_MAGIC = 0x57464345  # 'ECFW' little-endian
EFW_FORMAT_VERSION = 2
EFW_HEADER_MIN_LEN = 148
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


def _read_key(path: str, expected_len: int, kind: str) -> bytes:
    """Read a raw key or its ASCII hex form without ever printing key material."""
    with open(path, 'rb') as f:
        raw = f.read()
    if len(raw) == expected_len:
        return bytes(raw)
    raw = raw.strip()
    try:
        decoded = bytes.fromhex(raw.decode('ascii'))
    except (UnicodeDecodeError, ValueError):
        decoded = b''
    if len(decoded) != expected_len:
        raise ValueError(f'{kind}必须是 {expected_len} 字节原始数据或 {expected_len * 2} 位十六进制文本')
    return decoded


def _sodium():
    try:
        from nacl import bindings
    except ImportError as exc:
        raise ValueError('签名功能需要可选依赖 PyNaCl（底层使用 libsodium）；无签名功能不需要安装') from exc
    required = ('crypto_sign_seed_keypair', 'crypto_sign_ed25519ph_state',
                'crypto_sign_ed25519ph_update', 'crypto_sign_ed25519ph_final_create',
                'crypto_sign_ed25519ph_final_verify')
    if not all(hasattr(bindings, name) for name in required):
        raise ValueError('当前 PyNaCl/libsodium 不提供 Ed25519ph streaming API')
    return bindings


def _key_id(public_key: bytes) -> bytes:
    return __import__('hashlib').sha256(b'ECAT-EFW-KEY-ID-v1\0' + public_key).digest()[:8]


def _sign_ed25519ph(seed: bytes, message_parts) -> tuple:
    sodium = _sodium()
    public_key, secret_key = sodium.crypto_sign_seed_keypair(seed)
    state = sodium.crypto_sign_ed25519ph_state()
    for part in message_parts:
        sodium.crypto_sign_ed25519ph_update(state, part)
    signature = sodium.crypto_sign_ed25519ph_final_create(state, secret_key)
    return public_key, signature


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
    protected_paths=(),
    signing_key: str = None,
    public_key_out: str = None
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
    if public_key_out and not signing_key:
        raise ValueError('--public-key-out 只能与 --signing-key 一起使用')
    header_length = EFW_HEADER_MIN_LEN
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

    public_key_bytes = None
    if signing_key:
        seed = bytearray(_read_key(signing_key, 32, 'Ed25519 私钥种子'))
        try:
            sodium = _sodium()
            public_key_bytes, secret_key = sodium.crypto_sign_seed_keypair(bytes(seed))
            ext = struct.pack('<4sHHHH8s', b'SIG2', 1, 1, 64, 8, _key_id(public_key_bytes))
            signed_prefix = header_prefix + ext
            domain = b'ECAT-EFW-SIG-V2\0'
            _, signature = _sign_ed25519ph(bytes(seed), (domain, signed_prefix, payload_bytes))
            signed_header = signed_prefix + signature
            header_bytes = signed_header + struct.pack('<I', crc32_ieee(signed_header))
        finally:
            for i in range(len(seed)):
                seed[i] = 0
    else:
        ext = struct.pack('<4sHHHH8s', b'SIG2', 1, 0, 0, 0, bytes(8))
        unsigned_header = header_prefix + ext + bytes(64)
        header_bytes = unsigned_header + struct.pack('<I', crc32_ieee(unsigned_header))

    efw_data = header_bytes + payload_bytes

    if not out_path:
        if not project_name or project_name in ('.', '..') or any(c in project_name for c in ('/', '\\', '\0')):
            raise ValueError('项目名不能包含路径分隔符或为空')
        target_name = TARGET_ID_TO_NAME[target_id]
        ver_str = format_version_int(fw_version)
        out_path = f"{project_name}_{target_name}_V{ver_str}.efw"
        if len(os.fsencode(out_path)) > 31:
            raise ValueError('生成的 FoE 文件名超过 31 字节；请缩短项目名')

    if public_key_out and os.path.normcase(os.path.realpath(os.path.abspath(public_key_out))) == \
            os.path.normcase(os.path.realpath(os.path.abspath(out_path))):
        raise ValueError('--public-key-out 与固件包输出路径不能指向同一文件')
    if public_key_out:
        if os.path.lexists(public_key_out):
            raise FileExistsError(f'安全拒绝：公钥输出路径已存在: {public_key_out}')

    package_protected = tuple(
        p for p in protected_paths
        if not public_key_out or os.path.normcase(os.path.abspath(p)) !=
        os.path.normcase(os.path.abspath(public_key_out))
    )
    key_identity = None
    if public_key_out:
        key_protected = tuple(p for p in (protected_paths) if p and
                              os.path.normcase(os.path.abspath(p)) !=
                              os.path.normcase(os.path.abspath(public_key_out)))
        # Create the optional public key first. If it cannot be written, the
        # firmware output (including an existing --overwrite target) is untouched.
        key_file = safe_open_for_write(public_key_out, protected_paths=key_protected)
        key_identity = os.fstat(key_file.fileno())
        try:
            with key_file:
                key_file.write(public_key_bytes)
        except BaseException:
            _remove_created_file(public_key_out, key_identity)
            raise

    try:
        with safe_open_for_write(out_path, overwrite=overwrite, protected_paths=package_protected) as f:
            f.write(efw_data)
    except BaseException:
        if key_identity is not None:
            _remove_created_file(public_key_out, key_identity)
        raise

    return out_path


def inspect_efw(efw_path: str, extract_bin: str = None, overwrite: bool = False,
                summary: bool = False, public_key_path: str = None) -> bool:
    """
    解析并验证 .efw 文件
    """
    if not os.path.exists(efw_path):
        print(f"错误: 文件不存在: {efw_path}", file=sys.stderr)
        return False

    with open(efw_path, 'rb') as f:
        data = f.read()

    file_size = len(data)
    if file_size < 8:
        print(f"错误: 文件尺寸过小: {file_size} 字节", file=sys.stderr)
        return False

    magic, format_version, header_length = struct.unpack_from('<IHH', data, 0)
    if magic != EFW_MAGIC:
        print(f"错误: 魔数不匹配: 期望 0x{EFW_MAGIC:08X}, 实际 0x{magic:08X}", file=sys.stderr)
        return False
    if format_version != EFW_FORMAT_VERSION:
        print(f"错误: 规范版本不支持: {format_version}", file=sys.stderr)
        return False
    expected_header_len = EFW_HEADER_MIN_LEN
    if header_length != expected_header_len:
        print(f'错误: EFW v{format_version} 头长度必须为 {expected_header_len} 字节', file=sys.stderr)
        return False
    if (header_length % 4) != 0:
        print(f"错误: 头部长度非法: {header_length}", file=sys.stderr)
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

    ext_magic, ext_version, algorithm, sig_len, key_id_len = struct.unpack_from('<4sHHHH', data, 60)
    if ext_magic != b'SIG2' or ext_version != 1 or algorithm not in (0, 1):
        print('错误: 签名扩展格式或算法不受支持', file=sys.stderr)
        return False
    if ((algorithm == 0 and (sig_len != 0 or key_id_len != 0 or any(data[72:144]))) or
            (algorithm == 1 and (sig_len != 64 or key_id_len != 8))):
        print('错误: 签名扩展长度或未签名保留字段非法', file=sys.stderr)
        return False
    is_signed = algorithm == 1
    public_key = None
    signature_valid = False
    if is_signed:
        if public_key_path:
            public_key = _read_key(public_key_path, 32, 'Ed25519 公钥')
            key_id = data[72:80]
            if _key_id(public_key) != key_id:
                print('错误: 公钥 Key ID 与固件不匹配', file=sys.stderr)
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

    if is_signed and public_key is not None:
        sodium = _sodium()
        state = sodium.crypto_sign_ed25519ph_state()
        for part in (b'ECAT-EFW-SIG-V2\0', data[:80], payload):
            sodium.crypto_sign_ed25519ph_update(state, part)
        try:
            sodium.crypto_sign_ed25519ph_final_verify(state, data[80:144], public_key)
        except Exception:
            print('错误: Ed25519ph 签名验证失败', file=sys.stderr)
            return False
        signature_valid = True
    elif public_key_path and not is_signed:
        print('错误: 指定了公钥，但固件未签名', file=sys.stderr)
        return False

    target_name = TARGET_ID_TO_NAME.get(target_id, "未知")

    if summary:
        print("EFW 包有效（结构和 CRC 校验通过；设备兼容性未验证）")
        print(f"  EFW 格式版本:     {format_version}")
        print(f"  签名状态:         {'Ed25519ph 已验证' if signature_valid else ('存在，未验证' if is_signed else '未签名')}")
        print(f"  签名 Key ID:      {data[72:80].hex() if is_signed else '无'}")
        print(f"  固件版本:         V{format_version_int(fw_version)}")
        print(f"  项目 ID:          0x{project_id:08X}")
        print(f"  硬件 ID:          0x{hardware_id:08X}")
        print(f"  硬件版本范围:     [{hw_ver_min}, {hw_ver_max}]")
        print(f"  升级目标:         {target_name} (0x{target_id:04X})")
        print(f"  最低允许版本:     V{format_version_int(min_allow_version)}")
        print(f"  固件大小:         {payload_length} 字节")
        print("  Header CRC32:     通过")
        print("  Payload CRC32:    通过")
        print("  包内打包日期:     未提供")
    else:
        print(f"=== .efw 结构、通用字段及 CRC 校验通过（设备兼容性未验证） ===")
        print(f"  文件路径:          {efw_path}")
        print(f"  文件总大小:        {file_size} 字节")
        print(f"  头部长度:          {header_length} 字节")
        print(f"  签名状态:          {'Ed25519ph 已验证' if signature_valid else ('存在，未验证' if is_signed else '未签名')}")
        print(f"  签名 Key ID:       {data[72:80].hex() if is_signed else '无'}")
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
        protected = tuple(path for path in (efw_path, public_key_path) if path)
        with safe_open_for_write(extract_bin, overwrite=overwrite, protected_paths=protected) as f:
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
    parser.add_argument(
        "--version", action="version",
        version=f"ecat-efw {VERSION} (built {BUILD_DATE})",
        help="显示工具版本和构建日期并退出",
    )
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
    pack_p.add_argument("--signing-key", default=None,
                        help="可选 Ed25519 32 字节私钥种子文件（原始或 64 位 hex；仅在可信打包环境使用）")
    pack_p.add_argument("--public-key-out", default=None,
                        help="签名时可选输出对应的 32 字节原始公钥（供设备端可信密钥配置使用）")

    # inspect / unpack 子命令
    inspect_p = subparsers.add_parser("inspect", help="检验并打印 .efw 容器元数据")
    inspect_p.add_argument("efw_file", help="待检验的 .efw 文件")
    inspect_p.add_argument("--extract-bin", default=None, help="提取 payload 至指定文件")
    inspect_p.add_argument("--overwrite", action="store_true", default=False, help="允许覆盖已有提取文件")

    unpack_p = subparsers.add_parser("unpack", help="解包 .efw 文件 (等价于 inspect)")
    unpack_p.add_argument("efw_file", help="待解包的 .efw 文件")
    unpack_p.add_argument("--extract-bin", default=None, help="提取 payload 至指定文件")
    unpack_p.add_argument("--overwrite", action="store_true", default=False, help="允许覆盖已有提取文件")

    info_p = subparsers.add_parser("info", help="简洁显示并校验 .efw 固件包信息")
    info_p.add_argument("efw_file", help="待检查的固件包（按内容识别，不依赖扩展名）")
    info_p.add_argument("--public-key", default=None, help="可选 Ed25519 32 字节公钥文件，用于验证签名")
    inspect_p.add_argument("--public-key", default=None, help="可选 Ed25519 32 字节公钥文件，用于验证签名")
    unpack_p.add_argument("--public-key", default=None, help="可选 Ed25519 32 字节公钥文件，用于验证签名")

    args = parser.parse_args()

    if args.command == "pack":
        if args.public_key_out and not args.signing_key:
            raise ValueError('--public-key-out 只能与 --signing-key 一起使用')
        if args.public_key_out and args.out and os.path.normcase(os.path.abspath(args.public_key_out)) == \
                os.path.normcase(os.path.abspath(args.out)):
            raise ValueError('--public-key-out 与固件包输出路径不能相同')
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
            protected_paths=tuple(p for p in (args.bin, args.signing_key) if p),
            signing_key=args.signing_key,
            public_key_out=args.public_key_out
        )
        print(f"成功打包 .efw 文件: {out_file}")

    elif args.command in ("inspect", "unpack"):
        ok = inspect_efw(args.efw_file, extract_bin=args.extract_bin, overwrite=args.overwrite,
                         public_key_path=args.public_key)
        sys.exit(0 if ok else 1)
    elif args.command == "info":
        sys.exit(0 if inspect_efw(args.efw_file, summary=True, public_key_path=args.public_key) else 1)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, struct.error) as exc:
        print(f"错误: {exc}", file=sys.stderr)
        sys.exit(1)
