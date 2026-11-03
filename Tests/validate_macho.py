#!/usr/bin/env python3
"""Strict x86_64 kernel-extension Mach-O structure validator for UVCProbeFix builds."""

from __future__ import annotations

import struct
import sys
from pathlib import Path

MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_X86_64 = 0x01000007
MH_KEXT_BUNDLE = 0x0B
HEADER_SIZE = 32
_HEADER = struct.Struct("<IiiIIIII")
_LOAD_COMMAND = struct.Struct("<II")


class HeaderError(ValueError):
    pass


def validate_header_bytes(header: bytes) -> tuple[int, int, int, int, int]:
    if len(header) != HEADER_SIZE:
        raise HeaderError(f"Mach-O header is truncated: {len(header)} bytes, expected {HEADER_SIZE}")

    magic, cputype, _cpusubtype, filetype, ncmds, sizeofcmds, _flags, _reserved = _HEADER.unpack(header)
    if magic != MH_MAGIC_64:
        raise HeaderError(f"unexpected Mach-O magic 0x{magic:08X}")
    if cputype != CPU_TYPE_X86_64:
        raise HeaderError(f"unexpected CPU type 0x{cputype & 0xFFFFFFFF:08X}; expected x86_64")
    if filetype != MH_KEXT_BUNDLE:
        raise HeaderError(f"unexpected Mach-O filetype 0x{filetype:X}; expected 0xB (MH_KEXT_BUNDLE)")
    if ncmds == 0:
        raise HeaderError("Mach-O contains zero load commands")
    if sizeofcmds < _LOAD_COMMAND.size:
        raise HeaderError("Mach-O load-command area is too small")
    if ncmds > sizeofcmds // _LOAD_COMMAND.size:
        raise HeaderError("Mach-O ncmds cannot fit inside sizeofcmds")
    return magic, cputype, filetype, ncmds, sizeofcmds


def validate_macho_bytes(data: bytes) -> tuple[int, int, int, int, int]:
    if len(data) < HEADER_SIZE:
        raise HeaderError(f"Mach-O file is truncated: {len(data)} bytes")
    fields = validate_header_bytes(data[:HEADER_SIZE])
    magic, cputype, filetype, ncmds, sizeofcmds = fields
    commands_end = HEADER_SIZE + sizeofcmds
    if commands_end > len(data):
        raise HeaderError(
            f"load-command area exceeds file: end={commands_end} file={len(data)}"
        )

    offset = HEADER_SIZE
    for index in range(ncmds):
        if offset + _LOAD_COMMAND.size > commands_end:
            raise HeaderError(f"load command {index} header exceeds sizeofcmds")
        _cmd, cmdsize = _LOAD_COMMAND.unpack_from(data, offset)
        if cmdsize < _LOAD_COMMAND.size:
            raise HeaderError(f"load command {index} has invalid cmdsize={cmdsize}")
        if cmdsize % 8 != 0:
            raise HeaderError(f"load command {index} cmdsize is not 8-byte aligned: {cmdsize}")
        if cmdsize > commands_end - offset:
            raise HeaderError(f"load command {index} exceeds sizeofcmds")
        offset += cmdsize

    if offset != commands_end:
        raise HeaderError(
            f"load commands consume {offset - HEADER_SIZE} bytes, expected sizeofcmds={sizeofcmds}"
        )
    if commands_end == len(data):
        raise HeaderError("Mach-O contains no payload beyond load commands")
    return magic, cputype, filetype, ncmds, sizeofcmds


def validate_path(path: Path) -> str:
    data = path.read_bytes()
    magic, cputype, filetype, ncmds, sizeofcmds = validate_macho_bytes(data)
    return (
        f"Mach-O audit: magic=0x{magic:08X} cputype=0x{cputype:08X} "
        f"filetype=0x{filetype:X} (MH_KEXT_BUNDLE) ncmds={ncmds} "
        f"sizeofcmds={sizeofcmds} fileSize={len(data)}"
    )


def _header(*, magic: int = MH_MAGIC_64, cputype: int = CPU_TYPE_X86_64,
            filetype: int = MH_KEXT_BUNDLE, ncmds: int = 1, sizeofcmds: int = 8) -> bytes:
    return _HEADER.pack(magic, cputype, 3, filetype, ncmds, sizeofcmds, 0, 0)


def _good_blob() -> bytes:
    # One structurally valid 8-byte load command plus a byte of payload.
    return _header() + _LOAD_COMMAND.pack(1, 8) + b"X"


def self_test() -> None:
    validate_header_bytes(_header())
    validate_macho_bytes(_good_blob())

    negative_headers = (
        _header(magic=0xCEFAEDFE),
        _header(cputype=0x0100000C),
        _header(filetype=0x08),
        _header(ncmds=0, sizeofcmds=0),
        _header(ncmds=2, sizeofcmds=8),
        _header()[:31],
    )
    for sample in negative_headers:
        try:
            validate_header_bytes(sample)
        except HeaderError:
            pass
        else:
            raise AssertionError("negative Mach-O header case was accepted")

    negative_files = (
        _header(ncmds=1, sizeofcmds=8),  # load-command bytes missing
        _header() + _LOAD_COMMAND.pack(1, 0) + b"X",
        _header() + _LOAD_COMMAND.pack(1, 9) + b"X" * 10,
        _header(ncmds=1, sizeofcmds=16) + _LOAD_COMMAND.pack(1, 8) + b"\x00" * 8 + b"X",
        _header() + _LOAD_COMMAND.pack(1, 8),  # no payload
    )
    for sample in negative_files:
        try:
            validate_macho_bytes(sample)
        except HeaderError:
            pass
        else:
            raise AssertionError("negative Mach-O structure case was accepted")
    print("Mach-O structure validator self-test: PASS")


def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[1] == "--self-test":
        self_test()
        return 0
    if len(argv) != 2:
        print(f"usage: {argv[0]} <Mach-O> | --self-test", file=sys.stderr)
        return 2
    try:
        print(validate_path(Path(argv[1])))
    except (OSError, HeaderError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
