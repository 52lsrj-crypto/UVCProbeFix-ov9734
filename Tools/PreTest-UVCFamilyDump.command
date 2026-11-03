#!/bin/bash
# UVCProbeFix Pre-Test UVCFamily Runtime Dump v3
# Read-only collector for macOS Tahoe 26.6.2 / 25G83 / x86_64.
# Compiler-free: Python ctypes + Objective-C runtime + mach_vm_read_overwrite.
# Copyright © 2026 DNMTechLabs. All rights reserved.
# Contact: dnmtechlabs@gmail.com

set -euo pipefail
IFS=$'\n\t'
umask 077
PATH="/usr/bin:/bin:/usr/sbin:/sbin"
export PATH

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

if [ "$(/usr/bin/uname -s 2>/dev/null || true)" != "Darwin" ]; then
    fail "this collector must run on macOS. Nothing was changed."
fi
if [ "$(/usr/bin/uname -m 2>/dev/null || true)" != "x86_64" ]; then
    fail "this collector is only for the x86_64 25G83 target. Nothing was changed."
fi

OS_BUILD="$(/usr/bin/sw_vers -buildVersion 2>/dev/null || true)"
DARWIN_RELEASE="$(/usr/bin/uname -r 2>/dev/null || true)"
if [ "$OS_BUILD" != "25G83" ] || [ "$DARWIN_RELEASE" != "25.6.0" ]; then
    fail "expected macOS build 25G83 / Darwin 25.6.0; got ${OS_BUILD:-unknown} / ${DARWIN_RELEASE:-unknown}. Nothing was changed."
fi

PYTHON=""
for candidate in /usr/bin/python3 "$(/usr/bin/xcrun -f python3 2>/dev/null || true)" "$(command -v python3 2>/dev/null || true)"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then
        PYTHON="$candidate"
        break
    fi
done
[ -n "$PYTHON" ] || fail "python3 was not found. No ZIP was created."

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd -P)"
STAMP="$(/bin/date '+%Y%m%d-%H%M%S')"
RUN_ID="${STAMP}-$$"
OUT="$SCRIPT_DIR/UVCProbeFix-PreTest-UVCFamilyDump-$RUN_ID"
ZIP="$SCRIPT_DIR/UVCProbeFix-PreTest-UVCFamilyDump-$RUN_ID.zip"
/bin/mkdir "$OUT"

PY_SRC="$OUT/UVCFamilyRuntimeDump.py"
DUMP_TXT="$OUT/UVCFamily-runtime-full.txt"
RUN_LOG="$OUT/python-run.txt"

cat > "$PY_SRC" <<'PY_SRC_EOF'
#!/usr/bin/env python3
import ctypes
import os
import pathlib
import sys

OUT = pathlib.Path(sys.argv[1])

METHODS = (
    ("doProbeControl", "UVCUSBDeviceStreamingInterface", "doProbeControl:frameRate:probeData:error:", 8192),
    ("getStreamInPipe", "UVCUSBDeviceStreamingInterface", "getStreamInPipeOfType:type:probeData:alternateSetting:", 8192),
    ("doCommitControl", "UVCUSBDeviceStreamingInterface", "doCommitControl:error:", 4096),
)

UVC_PATHS = (
    "/System/Library/PrivateFrameworks/UVCFamily.framework/Versions/A/UVCFamily",
    "/System/Library/PrivateFrameworks/UVCFamily.framework/UVCFamily",
)


def emit(line=""):
    print(line, flush=True)


def load_library(paths):
    errors = []
    for p in paths:
        try:
            lib = ctypes.CDLL(p, mode=os.RTLD_NOW | os.RTLD_LOCAL)
            emit(f"UVCFamily.load=YES")
            emit(f"UVCFamily.path={p}")
            return lib
        except OSError as exc:
            errors.append((p, str(exc)))
            emit(f"UVCFamily.try={p}")
            emit(f"UVCFamily.tryError={exc}")
    emit("UVCFamily.load=NO")
    raise RuntimeError("; ".join(f"{p}: {e}" for p, e in errors))


def setup_runtime():
    objc = ctypes.CDLL("/usr/lib/libobjc.A.dylib", mode=os.RTLD_NOW | os.RTLD_LOCAL)
    system = ctypes.CDLL("/usr/lib/libSystem.B.dylib", mode=os.RTLD_NOW | os.RTLD_LOCAL)

    objc.objc_getClass.argtypes = [ctypes.c_char_p]
    objc.objc_getClass.restype = ctypes.c_void_p
    objc.sel_registerName.argtypes = [ctypes.c_char_p]
    objc.sel_registerName.restype = ctypes.c_void_p
    objc.class_getInstanceMethod.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    objc.class_getInstanceMethod.restype = ctypes.c_void_p
    objc.method_getImplementation.argtypes = [ctypes.c_void_p]
    objc.method_getImplementation.restype = ctypes.c_void_p
    objc.method_getTypeEncoding.argtypes = [ctypes.c_void_p]
    objc.method_getTypeEncoding.restype = ctypes.c_char_p

    system.mach_vm_read_overwrite.argtypes = [
        ctypes.c_uint32,
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_uint64),
    ]
    system.mach_vm_read_overwrite.restype = ctypes.c_int

    task_self = ctypes.c_uint32.in_dll(system, "mach_task_self_").value
    if task_self == 0:
        raise RuntimeError("mach_task_self_ resolved to zero")
    return objc, system, task_self


def safe_read(system, task_self, address, requested):
    if not address or requested <= 0:
        raise RuntimeError("invalid read request")
    buf = ctypes.create_string_buffer(requested)
    actual = ctypes.c_uint64(0)
    kr = system.mach_vm_read_overwrite(
        ctypes.c_uint32(task_self),
        ctypes.c_uint64(address),
        ctypes.c_uint64(requested),
        ctypes.c_uint64(ctypes.addressof(buf)),
        ctypes.byref(actual),
    )
    if kr != 0:
        raise RuntimeError(f"mach_vm_read_overwrite kr={kr} addr=0x{address:x} size={requested}")
    n = int(actual.value)
    if n <= 0 or n > requested:
        raise RuntimeError(f"invalid read length {n} for requested {requested}")
    return bytes(buf.raw[:n])


def method_info(objc, class_name, selector_name):
    cls = objc.objc_getClass(class_name.encode("utf-8"))
    if not cls:
        raise RuntimeError(f"class not found: {class_name}")
    sel = objc.sel_registerName(selector_name.encode("utf-8"))
    if not sel:
        raise RuntimeError(f"selector not found: {selector_name}")
    method = objc.class_getInstanceMethod(cls, sel)
    if not method:
        raise RuntimeError(f"method not found: {class_name} {selector_name}")
    imp = objc.method_getImplementation(method)
    if not imp:
        raise RuntimeError(f"IMP not found: {class_name} {selector_name}")
    types = objc.method_getTypeEncoding(method)
    types_s = types.decode("utf-8", errors="replace") if types else "(null)"
    return int(imp), types_s


def hex_lines(data, base, method_base):
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hexpart = " ".join(f"{b:02x}" for b in chunk)
        lines.append(f"{base+i:016x}  +0x{(base+i-method_base):06x}  {hexpart}")
    return lines


def main():
    emit("UVCProbeFix Pre-Test UVCFamily Runtime Dump v3")
    emit("collector.mode=READ_ONLY")
    emit("collector.compiler=NONE")
    emit(f"python={sys.executable}")
    emit(f"python.version={sys.version.split()[0]}")

    load_library(UVC_PATHS)
    objc, system, task_self = setup_runtime()
    emit(f"mach_task_self=0x{task_self:08x}")

    infos = {}
    for label, class_name, selector_name, requested in METHODS:
        emit()
        emit(f"=== METHOD {class_name} {selector_name} ===")
        imp, types = method_info(objc, class_name, selector_name)
        emit(f"label={label}")
        emit(f"IMP=0x{imp:016x}")
        emit(f"types={types}")
        emit(f"requested={requested}")
        data = safe_read(system, task_self, imp, requested)
        if len(data) < 1024:
            raise RuntimeError(f"short method read for {label}: {len(data)}")
        bin_path = OUT / f"{label}.bin"
        bin_path.write_bytes(data)
        emit("read=YES")
        emit(f"dumpLength={len(data)}")
        emit(f"binary={bin_path.name}")
        for line in hex_lines(data, imp, imp):
            emit(line)
        infos[label] = (imp, data)

    probe_imp, probe_data = infos["doProbeControl"]
    signature_offset = 0x18F
    signature_addr = probe_imp + signature_offset
    page_size = os.sysconf("SC_PAGE_SIZE")
    if not isinstance(page_size, int) or page_size <= 0 or (page_size & (page_size - 1)) != 0:
        raise RuntimeError(f"invalid page size: {page_size}")
    page_addr = signature_addr & ~(page_size - 1)
    page_data = safe_read(system, task_self, page_addr, page_size)
    if len(page_data) != page_size:
        raise RuntimeError(f"signature page short read: {len(page_data)} != {page_size}")
    (OUT / "signature-page.bin").write_bytes(page_data)

    emit()
    emit("=== SIGNATURE PAGE ===")
    emit(f"methodIMP=0x{probe_imp:016x}")
    emit(f"signatureAddress=0x{signature_addr:016x}")
    emit(f"signatureOffset=0x{signature_offset:x}")
    emit(f"pageAddress=0x{page_addr:016x}")
    emit(f"pageRequested={page_size}")
    emit("pageRead=YES")
    emit(f"pageLength={len(page_data)}")
    emit("binary=signature-page.bin")

    rel = signature_addr - probe_imp
    if rel + 64 > len(probe_data):
        raise RuntimeError("signature window is outside doProbeControl dump")
    sig64 = probe_data[rel:rel+64]
    (OUT / "signature-64.bin").write_bytes(sig64)
    emit(f"signature64={sig64.hex()}")

    emit()
    emit("COLLECTOR_RESULT=PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        emit()
        emit(f"COLLECTOR_ERROR={type(exc).__name__}: {exc}")
        emit("COLLECTOR_RESULT=FAIL")
        raise
PY_SRC_EOF

cat > "$OUT/README.txt" <<EOF_README
UVCProbeFix pre-test focused UVCFamily runtime extraction v3
OSBuild=$OS_BUILD
Darwin=$DARWIN_RELEASE
Architecture=x86_64
Mode=READ_ONLY
CompilerDependency=NONE
FoundationDependency=NONE
Python=$PYTHON
Purpose=Capture doProbeControl/post-GET_CUR, stream-pipe selection, doCommitControl, and the exact shared-cache signature page before physical camera testing.
EOF_README

printf 'Running compiler-free UVCFamily collector...\n'
set +e
"$PYTHON" "$PY_SRC" "$OUT" > "$DUMP_TXT" 2> "$RUN_LOG"
RC=$?
set -e
if [ "$RC" -ne 0 ]; then
    printf 'ERROR: runtime collector failed with rc=%d. No ZIP was created.\n' "$RC" >&2
    printf '%s\n' '---------------- collector output ----------------' >&2
    /usr/bin/tail -n 80 "$DUMP_TXT" >&2 || true
    printf '%s\n' '---------------- python stderr -------------------' >&2
    /usr/bin/tail -n 80 "$RUN_LOG" >&2 || true
    printf 'Failure folder retained at: %s\n' "$OUT" >&2
    exit "$RC"
fi

/usr/bin/grep -q '^COLLECTOR_RESULT=PASS$' "$DUMP_TXT" || fail "runtime validation did not report PASS. No ZIP was created."
for marker in \
    '=== METHOD UVCUSBDeviceStreamingInterface doProbeControl:frameRate:probeData:error: ===' \
    '=== METHOD UVCUSBDeviceStreamingInterface getStreamInPipeOfType:type:probeData:alternateSetting: ===' \
    '=== METHOD UVCUSBDeviceStreamingInterface doCommitControl:error: ===' \
    '=== SIGNATURE PAGE ==='; do
    /usr/bin/grep -Fq "$marker" "$DUMP_TXT" || fail "missing runtime marker: $marker. No ZIP was created."
done
[ "$(/usr/bin/grep -c '^read=YES$' "$DUMP_TXT" || true)" -eq 3 ] || fail "expected three successful method reads. No ZIP was created."
/usr/bin/grep -q '^pageRead=YES$' "$DUMP_TXT" || fail "signature page was not read. No ZIP was created."

for f in doProbeControl.bin getStreamInPipe.bin doCommitControl.bin signature-page.bin signature-64.bin; do
    [ -s "$OUT/$f" ] || fail "missing or empty runtime binary $f. No ZIP was created."
done
[ "$(/usr/bin/stat -f '%z' "$OUT/signature-64.bin")" -eq 64 ] || fail "signature-64.bin is not exactly 64 bytes. No ZIP was created."

/usr/bin/shasum -a 256 "$DUMP_TXT" > "$OUT/UVCFamily-runtime-full.sha256"
/usr/bin/sw_vers > "$OUT/sw_vers.txt" 2>&1 || true
/usr/bin/uname -a > "$OUT/uname.txt" 2>&1 || true

# Keep Python source and stderr in the successful archive for auditability.
(
    cd "$OUT"
    /usr/bin/find . -type f ! -name SHA256SUMS.txt -print0 | /usr/bin/sort -z | while IFS= read -r -d '' f; do
        /usr/bin/shasum -a 256 "$f"
    done
) > "$OUT/SHA256SUMS.txt"

/bin/rm -f -- "$ZIP"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$OUT" "$ZIP"
/usr/bin/unzip -tq "$ZIP" >/dev/null || fail "ZIP verification failed."

VERIFY_DIR="$OUT/.verify"
/bin/mkdir "$VERIFY_DIR"
/usr/bin/ditto -x -k "$ZIP" "$VERIFY_DIR"
VERIFY_ROOT="$VERIFY_DIR/$(/usr/bin/basename "$OUT")"
[ -f "$VERIFY_ROOT/UVCFamily-runtime-full.txt" ] || fail "verified ZIP is missing the runtime dump."
for f in doProbeControl.bin getStreamInPipe.bin doCommitControl.bin signature-page.bin signature-64.bin; do
    [ -s "$VERIFY_ROOT/$f" ] || fail "verified ZIP is missing runtime binary $f."
done
(
    cd "$VERIFY_ROOT"
    /usr/bin/shasum -a 256 -c SHA256SUMS.txt >/dev/null
) || fail "ZIP payload checksum verification failed."
/bin/rm -rf -- "$VERIFY_DIR"

ZIP_SHA="$(/usr/bin/shasum -a 256 "$ZIP" | /usr/bin/awk '{print $1}')"

echo "============================================================"
echo "PRE-TEST UVCFAMILY DUMP COMPLETE"
echo "ZIP: $ZIP"
echo "SHA-256: $ZIP_SHA"
echo "Runtime methods: 3/3 read successfully"
echo "Signature page: PASS"
echo "Compiler: NOT USED"
echo "No EFI, NVRAM, kext, camera setting, or macOS system file was modified."
echo "============================================================"
/usr/bin/open -R "$ZIP" >/dev/null 2>&1 || true
