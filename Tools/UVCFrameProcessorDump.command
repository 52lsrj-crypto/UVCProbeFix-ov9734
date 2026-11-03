#!/bin/bash
set -euo pipefail

export PATH="/usr/bin:/bin:/usr/sbin:/sbin:/Library/Apple/usr/bin"

fail() {
  echo
  echo "ERROR: $*" >&2
  echo
  read -r -p "Press ENTER to close..." _ || true
  exit 1
}

[ "$(uname -s)" = "Darwin" ] || fail "This collector must run on macOS."
[ "$(uname -m)" = "x86_64" ] || fail "Expected x86_64."

BUILD="$(/usr/bin/sw_vers -buildVersion 2>/dev/null || true)"
DARWIN="$(/usr/bin/uname -r 2>/dev/null || true)"
case "$DARWIN" in
  25.6.*) ;;
  *) fail "Expected Darwin 25.6.x, found: $DARWIN" ;;
esac
[ "$BUILD" = "25G83" ] || fail "Expected macOS build 25G83, found: $BUILD"

PY=""
if [ -x /usr/bin/python3 ]; then
  PY=/usr/bin/python3
elif /usr/bin/xcrun -f python3 >/dev/null 2>&1; then
  PY="$(/usr/bin/xcrun -f python3)"
elif command -v python3 >/dev/null 2>&1; then
  PY="$(command -v python3)"
fi
[ -n "$PY" ] || fail "python3 was not found."

BASE="$(cd "$(dirname "$0")" && pwd)"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
OUT="$BASE/UVCProbeFix-UVCFrameProcessorDump-$STAMP"
ZIP="$OUT.zip"
mkdir -p "$OUT"

cat > "$OUT/dump.py" <<'PY'
import ctypes
import ctypes.util
import hashlib
import json
import os
import platform
import struct
import sys
from pathlib import Path

OUT = Path(sys.argv[1])

FRAMEWORK = b"/System/Library/PrivateFrameworks/UVCFrameProcessor.framework/Versions/A/UVCFrameProcessor"

libdl = ctypes.CDLL(None)
libobjc = ctypes.CDLL("/usr/lib/libobjc.A.dylib")

# dlopen / dladdr
libdl.dlopen.argtypes = [ctypes.c_char_p, ctypes.c_int]
libdl.dlopen.restype = ctypes.c_void_p

class Dl_info(ctypes.Structure):
    _fields_ = [
        ("dli_fname", ctypes.c_char_p),
        ("dli_fbase", ctypes.c_void_p),
        ("dli_sname", ctypes.c_char_p),
        ("dli_saddr", ctypes.c_void_p),
    ]

libdl.dladdr.argtypes = [ctypes.c_void_p, ctypes.POINTER(Dl_info)]
libdl.dladdr.restype = ctypes.c_int

RTLD_NOW = 2
RTLD_LOCAL = 4

h = libdl.dlopen(FRAMEWORK, RTLD_NOW | RTLD_LOCAL)
if not h:
    raise SystemExit("dlopen(UVCFrameProcessor) failed")

# ObjC runtime
libobjc.objc_getClass.argtypes = [ctypes.c_char_p]
libobjc.objc_getClass.restype = ctypes.c_void_p
libobjc.sel_registerName.argtypes = [ctypes.c_char_p]
libobjc.sel_registerName.restype = ctypes.c_void_p
libobjc.class_getInstanceMethod.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
libobjc.class_getInstanceMethod.restype = ctypes.c_void_p
libobjc.method_getImplementation.argtypes = [ctypes.c_void_p]
libobjc.method_getImplementation.restype = ctypes.c_void_p
libobjc.method_getTypeEncoding.argtypes = [ctypes.c_void_p]
libobjc.method_getTypeEncoding.restype = ctypes.c_char_p

libobjc.class_copyIvarList.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)]
libobjc.class_copyIvarList.restype = ctypes.POINTER(ctypes.c_void_p)
libobjc.ivar_getName.argtypes = [ctypes.c_void_p]
libobjc.ivar_getName.restype = ctypes.c_char_p
libobjc.ivar_getTypeEncoding.argtypes = [ctypes.c_void_p]
libobjc.ivar_getTypeEncoding.restype = ctypes.c_char_p
libobjc.ivar_getOffset.argtypes = [ctypes.c_void_p]
libobjc.ivar_getOffset.restype = ctypes.c_long

# free()
libc = ctypes.CDLL(None)
libc.free.argtypes = [ctypes.c_void_p]
libc.free.restype = None

# mach_vm_read_overwrite
mach = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
try:
    mach_task_self_ = ctypes.c_uint.in_dll(mach, "mach_task_self_").value
except Exception:
    mach.mach_task_self.restype = ctypes.c_uint
    mach_task_self_ = mach.mach_task_self()

mach.mach_vm_read_overwrite.argtypes = [
    ctypes.c_uint,
    ctypes.c_uint64,
    ctypes.c_uint64,
    ctypes.c_uint64,
    ctypes.POINTER(ctypes.c_uint64),
]
mach.mach_vm_read_overwrite.restype = ctypes.c_int

def read_mem(addr: int, size: int) -> bytes:
    buf = ctypes.create_string_buffer(size)
    outsz = ctypes.c_uint64(0)
    kr = mach.mach_vm_read_overwrite(
        mach_task_self_,
        ctypes.c_uint64(addr),
        ctypes.c_uint64(size),
        ctypes.c_uint64(ctypes.addressof(buf)),
        ctypes.byref(outsz),
    )
    if kr != 0:
        raise RuntimeError(f"mach_vm_read_overwrite failed kr=0x{kr:x} addr=0x{addr:x} size={size}")
    if outsz.value == 0:
        raise RuntimeError(f"read returned zero bytes at 0x{addr:x}")
    return bytes(buf.raw[:outsz.value])

def dl_info(addr: int):
    d = Dl_info()
    ok = libdl.dladdr(ctypes.c_void_p(addr), ctypes.byref(d))
    if not ok:
        return {}
    return {
        "image": d.dli_fname.decode(errors="replace") if d.dli_fname else None,
        "imageBase": int(ctypes.cast(d.dli_fbase, ctypes.c_void_p).value or 0),
        "symbol": d.dli_sname.decode(errors="replace") if d.dli_sname else None,
        "symbolAddress": int(ctypes.cast(d.dli_saddr, ctypes.c_void_p).value or 0),
    }

def hexdump(data: bytes, base: int) -> str:
    out = []
    for off in range(0, len(data), 16):
        chunk = data[off:off+16]
        hx = " ".join(f"{x:02x}" for x in chunk)
        out.append(f"{base+off:016x}  {hx}")
    return "\n".join(out) + "\n"

cls = libobjc.objc_getClass(b"UVCFrameProcessorNative")
if not cls:
    raise SystemExit("objc_getClass(UVCFrameProcessorNative) failed")

# Ivar layout is critical to map packetDropInvalidDataPayloadCount to the exact
# increment instruction in handleStreamData:.
count = ctypes.c_uint(0)
ivars_ptr = libobjc.class_copyIvarList(cls, ctypes.byref(count))
ivars = []
if ivars_ptr:
    for i in range(count.value):
        iv = ivars_ptr[i]
        name = libobjc.ivar_getName(iv)
        typ = libobjc.ivar_getTypeEncoding(iv)
        ivars.append({
            "name": name.decode(errors="replace") if name else None,
            "type": typ.decode(errors="replace") if typ else None,
            "offset": int(libobjc.ivar_getOffset(iv)),
        })
    libc.free(ivars_ptr)

methods = {
    "handleStreamData": ("handleStreamData:", 16384),
    "createNativeFrame": ("createNativeFrame:data:", 8192),
    "dispatchPreviousFrame": ("dispatchPreviousFrame", 4096),
    "reset": ("reset", 4096),
    "serialize": ("serialize", 4096),
}

meta = {
    "os_build": os.popen("/usr/bin/sw_vers -buildVersion").read().strip(),
    "darwin": platform.release(),
    "machine": platform.machine(),
    "class": "UVCFrameProcessorNative",
    "ivars": ivars,
    "methods": {},
}

for key, (selector_name, length) in methods.items():
    sel = libobjc.sel_registerName(selector_name.encode())
    meth = libobjc.class_getInstanceMethod(cls, sel)
    if not meth:
        raise SystemExit(f"class_getInstanceMethod failed: {selector_name}")
    imp = int(libobjc.method_getImplementation(meth) or 0)
    if not imp:
        raise SystemExit(f"method_getImplementation failed: {selector_name}")
    enc = libobjc.method_getTypeEncoding(meth)
    info = dl_info(imp)

    data = read_mem(imp, length)
    if len(data) < min(1024, length):
        raise SystemExit(f"short dump for {selector_name}: {len(data)}")

    bin_path = OUT / f"{key}.bin"
    hex_path = OUT / f"{key}.hex.txt"
    bin_path.write_bytes(data)
    hex_path.write_text(hexdump(data, imp))

    meta["methods"][key] = {
        "selector": selector_name,
        "typeEncoding": enc.decode(errors="replace") if enc else None,
        "imp": f"0x{imp:x}",
        "length": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        **info,
    }

# Capture the full page containing handleStreamData entry plus adjacent pages.
haddr = int(meta["methods"]["handleStreamData"]["imp"], 16)
page = haddr & ~0xFFF
page_blob = read_mem(page, 0x6000)
(OUT / "handleStreamData-pages.bin").write_bytes(page_blob)
(OUT / "handleStreamData-pages.hex.txt").write_text(hexdump(page_blob, page))
meta["handleStreamDataPages"] = {
    "base": f"0x{page:x}",
    "length": len(page_blob),
    "sha256": hashlib.sha256(page_blob).hexdigest(),
}

(OUT / "metadata.json").write_text(json.dumps(meta, indent=2, sort_keys=True))
(OUT / "ivars.txt").write_text(
    "\n".join(f"{x['offset']:4d}  0x{x['offset']:x}  {x['name']}  {x['type']}" for x in ivars) + "\n"
)

# Required invariants. Known 26.x layout should expose these names; do not create
# a success archive if the runtime cannot provide enough information.
names = {x["name"] for x in ivars}
required = {
    "_packetsReceived",
    "_packetsDropped",
    "_fidTracker",
    "_packetDropCount",
}
missing = sorted(required - names)
if missing:
    raise SystemExit("required ivars missing: " + ", ".join(missing))

for required_method in methods:
    if required_method not in meta["methods"]:
        raise SystemExit("required method missing: " + required_method)

print("UVCFrameProcessor runtime dump: PASS")
print("Class:", meta["class"])
for m, v in meta["methods"].items():
    print(f"{m}: IMP={v['imp']} bytes={v['length']} sha256={v['sha256']}")
print("Ivars:", len(ivars))
PY

"$PY" "$OUT/dump.py" "$OUT" > "$OUT/runtime-output.txt" 2>&1 || {
  cat "$OUT/runtime-output.txt" >&2 || true
  rm -f "$ZIP"
  fail "UVCFrameProcessor runtime dump failed. No ZIP was created."
}

for f in \
  "$OUT/metadata.json" \
  "$OUT/ivars.txt" \
  "$OUT/handleStreamData.bin" \
  "$OUT/createNativeFrame.bin" \
  "$OUT/dispatchPreviousFrame.bin" \
  "$OUT/reset.bin" \
  "$OUT/serialize.bin" \
  "$OUT/handleStreamData-pages.bin"
do
  [ -s "$f" ] || fail "Required dump missing/empty: $f"
done

(
  cd "$BASE"
  /usr/bin/zip -qry "$(basename "$ZIP")" "$(basename "$OUT")"
) || fail "zip creation failed"

[ -s "$ZIP" ] || fail "ZIP was not created."
/usr/bin/unzip -t "$ZIP" >/dev/null || fail "ZIP integrity test failed."

SHA="$(/usr/bin/shasum -a 256 "$ZIP" | /usr/bin/awk '{print $1}')"

echo
echo "============================================================"
echo " UVCFRAMEPROCESSOR DUMP COMPLETE"
echo "============================================================"
cat "$OUT/runtime-output.txt"
echo
echo "ZIP: $ZIP"
echo "SHA-256: $SHA"
echo
echo "This collector did NOT open the camera and did NOT modify EFI, NVRAM,"
echo "kexts, macOS files, or the UVC stream."
echo
read -r -p "Press ENTER to close..." _ || true
