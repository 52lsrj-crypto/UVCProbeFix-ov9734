#!/usr/bin/env python3
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "Tools" / "tahoe_kc_abi.py"
TARGET_ENTRY = "com.apple.iokit.IOUSBHostFamily"
TARGET_SYMBOL = "__ZN18IOUSBHostInterface13deviceRequestERN11StandardUSB13DeviceRequestEPvRjj"
CPU_X86_64 = 0x01000007
MH_MAGIC_64 = 0xFEEDFACF
MH_KEXT_BUNDLE = 0xB
MH_FILESET = 0xC
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_UUID = 0x1B
LC_FILESET_ENTRY = 0x80000035


def pad8(b: bytes) -> bytes:
    return b + b"\0" * ((8 - (len(b) % 8)) % 8)


def seg64(name: str, vmaddr: int, vmsize: int, fileoff: int, filesize: int) -> bytes:
    n = name.encode()[:16].ljust(16, b"\0")
    return struct.pack("<II16sQQQQiiII", LC_SEGMENT_64, 72, n,
                       vmaddr, vmsize, fileoff, filesize, 7, 5, 0, 0)


def uuid_cmd(raw: bytes) -> bytes:
    assert len(raw) == 16
    return struct.pack("<II16s", LC_UUID, 24, raw)


def symtab_cmd(symoff: int, nsyms: int, stroff: int, strsize: int) -> bytes:
    return struct.pack("<IIIIII", LC_SYMTAB, 24, symoff, nsyms, stroff, strsize)


def macho_header(filetype: int, cmds: list[bytes]) -> bytes:
    return struct.pack("<IIIIIIII", MH_MAGIC_64, CPU_X86_64, 3, filetype,
                       len(cmds), sum(map(len, cmds)), 0, 0) + b"".join(cmds)


def fileset_entry(entry: str, vmaddr: int, fileoff: int) -> bytes:
    name = entry.encode() + b"\0"
    cmdsize = 32 + len(name)
    cmdsize = (cmdsize + 7) & ~7
    return struct.pack("<IIQQII", LC_FILESET_ENTRY, cmdsize, vmaddr, fileoff, 32, 0) + name + b"\0" * (cmdsize - 32 - len(name))


def build_fileset(path: Path, duplicate_symbol: bool = False) -> tuple[bytes, bytes]:
    data = bytearray(0x4000)
    entry_off = 0x1000
    text_vm = 0xFFFFFF8001000000
    prologue_off = 0x1800
    symbol_addr = text_vm + (prologue_off - 0x1000)
    prologue = bytes(range(0x40, 0x60))
    uuid = bytes.fromhex("00112233445566778899aabbccddeeff")

    outer_cmd = fileset_entry(TARGET_ENTRY, text_vm, entry_off)
    outer = macho_header(MH_FILESET, [outer_cmd])
    data[:len(outer)] = outer

    symoff = 0x2100
    stroff = 0x2200
    strings = b"\0" + TARGET_SYMBOL.encode() + b"\0"
    nsyms = 2 if duplicate_symbol else 1
    cmds = [
        seg64("__TEXT", text_vm, 0x1000, 0x1000, 0x1000),
        seg64("__LINKEDIT", text_vm + 0x1000, 0x1000, 0x2000, 0x1000),
        uuid_cmd(uuid),
        symtab_cmd(symoff, nsyms, stroff, len(strings)),
    ]
    nested = macho_header(MH_KEXT_BUNDLE, cmds)
    data[entry_off:entry_off + len(nested)] = nested
    data[prologue_off:prologue_off + len(prologue)] = prologue
    nlist = struct.pack("<IbbHQ", 1, 0x0F, 1, 0, symbol_addr)
    data[symoff:symoff + 16] = nlist
    if duplicate_symbol:
        data[symoff + 16:symoff + 32] = nlist
    data[stroff:stroff + len(strings)] = strings
    path.write_bytes(data)
    return uuid, prologue


def build_standalone(path: Path) -> tuple[bytes, bytes]:
    data = bytearray(0x3000)
    text_vm = 0x100000000
    prologue_off = 0x800
    symbol_addr = text_vm + prologue_off
    prologue = bytes(range(0x60, 0x80))
    uuid = bytes.fromhex("ffeeddccbbaa99887766554433221100")
    symoff, stroff = 0x2100, 0x2200
    strings = b"\0" + TARGET_SYMBOL.encode() + b"\0"
    cmds = [
        seg64("__TEXT", text_vm, 0x1000, 0, 0x1000),
        seg64("__LINKEDIT", text_vm + 0x2000, 0x1000, 0x2000, 0x1000),
        uuid_cmd(uuid),
        symtab_cmd(symoff, 1, stroff, len(strings)),
    ]
    header = macho_header(MH_KEXT_BUNDLE, cmds)
    data[:len(header)] = header
    data[prologue_off:prologue_off + len(prologue)] = prologue
    data[symoff:symoff + 16] = struct.pack("<IbbHQ", 1, 0x0F, 1, 0, symbol_addr)
    data[stroff:stroff + len(strings)] = strings
    path.write_bytes(data)
    return uuid, prologue


def run_helper(*args: str, expect_ok: bool = True):
    proc = subprocess.run([sys.executable, str(HELPER), *args], text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if expect_ok and proc.returncode != 0:
        raise AssertionError(f"helper failed: {proc.stderr}")
    if not expect_ok and proc.returncode == 0:
        raise AssertionError("helper unexpectedly succeeded")
    return proc


def main() -> int:
    if not HELPER.exists():
        print(f"missing helper: {HELPER}", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        kc = td / "BootKernelExtensions.kc"
        uuid, prologue = build_fileset(kc)
        p = run_helper("--entry", TARGET_ENTRY, "--symbol", TARGET_SYMBOL, str(kc))
        obj = json.loads(p.stdout)
        assert obj["sourceKind"] == "kernel-collection-fileset"
        assert obj["filesetEntryID"] == TARGET_ENTRY
        assert obj["uuidHex"] == uuid.hex()
        assert obj["prologueHex32"] == prologue.hex()
        assert int(obj["symbolAddress"], 16) == 0xFFFFFF8001000800
        assert len(obj["imageSHA256"]) == 64
        assert len(obj["sourceSHA256"]) == 64

        standalone = td / "IOUSBHostFamily"
        uuid2, prologue2 = build_standalone(standalone)
        p2 = run_helper("--symbol", TARGET_SYMBOL, str(standalone))
        obj2 = json.loads(p2.stdout)
        assert obj2["sourceKind"] == "standalone-macho"
        assert obj2["uuidHex"] == uuid2.hex()
        assert obj2["prologueHex32"] == prologue2.hex()

        dup = td / "duplicate.kc"
        build_fileset(dup, duplicate_symbol=True)
        p3 = run_helper("--entry", TARGET_ENTRY, "--symbol", TARGET_SYMBOL, str(dup), expect_ok=False)
        assert "exact symbol count" in p3.stderr

        bad = td / "bad.kc"
        bad.write_bytes(b"not a macho")
        p4 = run_helper("--entry", TARGET_ENTRY, "--symbol", TARGET_SYMBOL, str(bad), expect_ok=False)
        assert "Mach-O" in p4.stderr or "magic" in p4.stderr

    print("test_tahoe_kc_abi: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
