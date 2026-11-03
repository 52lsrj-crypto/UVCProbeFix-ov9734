#!/usr/bin/env python3
"""Strict release ZIP structure validator for UVCProbeFix."""

from __future__ import annotations

import plistlib
import stat
import sys
import tempfile
import warnings
import zipfile
from pathlib import Path, PurePosixPath

EXPECTED_ROOT = "UVCProbeFix.kext"
MAX_UNCOMPRESSED = 64 * 1024 * 1024


class ZipAuditError(ValueError):
    pass


def _audit_name(name: str) -> None:
    if (not name or "\\" in name or name.startswith("/") or
            name.startswith("./") or "//" in name):
        raise ZipAuditError(f"unsafe archive path: {name!r}")
    path = PurePosixPath(name)
    if any(part in ("", ".", "..") for part in path.parts):
        raise ZipAuditError(f"unsafe archive path component: {name!r}")
    if path.parts[0] != EXPECTED_ROOT:
        raise ZipAuditError(f"unexpected archive root: {name!r}")


def validate_zip(path: Path, expected_version: str, expected_build=None) -> str:
    with zipfile.ZipFile(path, "r") as zf:
        infos = zf.infolist()
        if not infos:
            raise ZipAuditError("release ZIP is empty")

        names = [info.filename.rstrip("/") for info in infos if info.filename.rstrip("/")]
        if len(names) != len(set(names)):
            raise ZipAuditError("release ZIP contains duplicate paths")

        total = 0
        for info in infos:
            raw_name = info.filename.rstrip("/")
            if not raw_name:
                continue
            _audit_name(raw_name)
            mode = (info.external_attr >> 16) & 0xFFFF
            if stat.S_ISLNK(mode):
                raise ZipAuditError(f"symlink entry is not allowed: {raw_name}")
            if not info.is_dir():
                total += info.file_size
                if total > MAX_UNCOMPRESSED:
                    raise ZipAuditError("release ZIP exceeds uncompressed size limit")

        plist_name = f"{EXPECTED_ROOT}/Contents/Info.plist"
        exe_name = f"{EXPECTED_ROOT}/Contents/MacOS/UVCProbeFix"
        if plist_name not in names or exe_name not in names:
            raise ZipAuditError("release ZIP is missing required kext files")
        if zf.getinfo(exe_name).file_size <= 0:
            raise ZipAuditError("release executable is empty")

        try:
            plist = plistlib.loads(zf.read(plist_name))
        except Exception as exc:  # pragma: no cover - exact plist parser exception is not important
            raise ZipAuditError(f"invalid embedded Info.plist: {exc}") from exc
        if plist.get("CFBundleIdentifier") != "com.dnmtechlabs.UVCProbeFix":
            raise ZipAuditError("embedded bundle identifier mismatch")
        if plist.get("CFBundleExecutable") != "UVCProbeFix":
            raise ZipAuditError("embedded executable name mismatch")
        if plist.get("CFBundleShortVersionString") != expected_version:
            raise ZipAuditError("embedded short version mismatch")
        expected_build = expected_version if expected_build is None else expected_build
        if plist.get("CFBundleVersion") != expected_build:
            raise ZipAuditError("embedded bundle build mismatch")

    return f"Release ZIP audit: entries={len(infos)} uncompressed={total} bytes root={EXPECTED_ROOT}"


def _plist(version: str, build=None) -> bytes:
    build = version if build is None else build
    return plistlib.dumps({
        "CFBundleIdentifier": "com.dnmtechlabs.UVCProbeFix",
        "CFBundleExecutable": "UVCProbeFix",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": build,
    })


def self_test() -> None:
    version = "9.9.9"
    build = "999"
    with tempfile.TemporaryDirectory(prefix="uvcp-ziptest-") as td:
        root = Path(td)
        good = root / "good.zip"
        with zipfile.ZipFile(good, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(f"{EXPECTED_ROOT}/Contents/Info.plist", _plist(version, build))
            zf.writestr(f"{EXPECTED_ROOT}/Contents/MacOS/UVCProbeFix", b"MACHO")
        validate_zip(good, version, build)

        bad_cases = []
        traversal = root / "traversal.zip"
        with zipfile.ZipFile(traversal, "w") as zf:
            zf.writestr("../escape", b"x")
        bad_cases.append(traversal)

        wrong_root = root / "wrong-root.zip"
        with zipfile.ZipFile(wrong_root, "w") as zf:
            zf.writestr("Other.kext/Contents/Info.plist", _plist(version, build))
        bad_cases.append(wrong_root)

        duplicate = root / "duplicate.zip"
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(duplicate, "w") as zf:
                zf.writestr(f"{EXPECTED_ROOT}/Contents/Info.plist", _plist(version, build))
                zf.writestr(f"{EXPECTED_ROOT}/Contents/Info.plist", _plist(version, build))
                zf.writestr(f"{EXPECTED_ROOT}/Contents/MacOS/UVCProbeFix", b"MACHO")
        bad_cases.append(duplicate)

        symlink_zip = root / "symlink.zip"
        with zipfile.ZipFile(symlink_zip, "w") as zf:
            zi = zipfile.ZipInfo(f"{EXPECTED_ROOT}/Contents/MacOS/UVCProbeFix")
            zi.create_system = 3
            zi.external_attr = (stat.S_IFLNK | 0o777) << 16
            zf.writestr(zi, b"/tmp/escape")
            zf.writestr(f"{EXPECTED_ROOT}/Contents/Info.plist", _plist(version, build))
        bad_cases.append(symlink_zip)

        double_slash = root / "double-slash.zip"
        with zipfile.ZipFile(double_slash, "w") as zf:
            zf.writestr(f"{EXPECTED_ROOT}//Contents/Info.plist", _plist(version, build))
        bad_cases.append(double_slash)

        for candidate in bad_cases:
            try:
                validate_zip(candidate, version)
            except (ZipAuditError, zipfile.BadZipFile):
                pass
            else:
                raise AssertionError(f"negative release ZIP case was accepted: {candidate.name}")
    print("Release ZIP validator self-test: PASS")


def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[1] == "--self-test":
        self_test()
        return 0
    if len(argv) not in (3, 4):
        print(f"usage: {argv[0]} <release.zip> <expected-version> [expected-build] | --self-test", file=sys.stderr)
        return 2
    try:
        build = argv[3] if len(argv) == 4 else None
        print(validate_zip(Path(argv[1]), argv[2], build))
    except (OSError, ZipAuditError, zipfile.BadZipFile) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
