#!/bin/bash
set -euo pipefail
umask 077

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_COMMIT="05094e5e88cec7caedbfb35e8449ed0db94bf95b"
TARGET_ENTRY="com.apple.iokit.IOUSBHostFamily"
TARGET_SYMBOL="__ZN18IOUSBHostInterface13deviceRequestERN11StandardUSB13DeviceRequestEPvRjj"
EXPECTED_SIGNATURE='IOUSBHostInterface::deviceRequest(StandardUSB::DeviceRequest&, void*, unsigned int&, unsigned int)'
STANDALONE="/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily"
BOOT_KC="/System/Library/KernelCollections/BootKernelExtensions.kc"
SYSTEM_KC="/System/Library/KernelCollections/SystemKernelExtensions.kc"
PYTHON="/usr/bin/python3"
HELPER="$ROOT/Tools/tahoe_kc_abi.py"
EXPLICIT_SOURCE="${1:-}"

fail() { printf 'ABI AUDIT FAIL: %s\n' "$*" >&2; exit 1; }
[ "$(uname -s)" = "Darwin" ] || fail "Darwin target required"
[ "$(uname -m)" = "x86_64" ] || fail "x86_64 target required"
BUILD="$(/usr/bin/sw_vers -buildVersion)"
case "$BUILD" in
  25G229|25G241) ;;
  *) fail "unsupported/unverified macOS build $BUILD (expected 25G229 or 25G241)" ;;
esac
[ -x "$PYTHON" ] || fail "/usr/bin/python3 is required"
[ -f "$HELPER" ] || fail "Mach-O/KC ABI helper missing at $HELPER"

SDK_ROOT="$ROOT/MacKernelSDK"
[ -d "$SDK_ROOT" ] || fail "pinned MacKernelSDK must be prepared at $SDK_ROOT first"
ACTUAL_SDK_COMMIT="$(/usr/bin/git -C "$SDK_ROOT" rev-parse HEAD 2>/dev/null || true)"
[ "$ACTUAL_SDK_COMMIT" = "$SDK_COMMIT" ] || fail "MacKernelSDK commit mismatch: ${ACTUAL_SDK_COMMIT:-missing}"

TMP="$(/usr/bin/mktemp -d /tmp/uvcp-abi.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT INT TERM
SOURCE_JSON="$TMP/source.json"

probe_source() {
  local source="$1"
  [ -f "$source" ] || return 1
  # First try the modern MH_FILESET layout, then a standalone Mach-O.
  if "$PYTHON" "$HELPER" --entry "$TARGET_ENTRY" --symbol "$TARGET_SYMBOL" "$source" > "$SOURCE_JSON" 2>"$TMP/probe.err"; then
    return 0
  fi
  if "$PYTHON" "$HELPER" --symbol "$TARGET_SYMBOL" "$source" > "$SOURCE_JSON" 2>"$TMP/probe.err"; then
    return 0
  fi
  return 1
}

if [ -n "$EXPLICIT_SOURCE" ]; then
  probe_source "$EXPLICIT_SOURCE" || {
    /bin/cat "$TMP/probe.err" >&2 || true
    fail "explicit IOUSBHost ABI source is not usable: $EXPLICIT_SOURCE"
  }
elif [ -f "$STANDALONE" ]; then
  probe_source "$STANDALONE" || {
    /bin/cat "$TMP/probe.err" >&2 || true
    fail "standalone IOUSBHostFamily Mach-O exists but failed ABI validation"
  }
else
  KC_CANDIDATES=()
  [ -f "$BOOT_KC" ] && KC_CANDIDATES+=("$BOOT_KC")
  [ -f "$SYSTEM_KC" ] && KC_CANDIDATES+=("$SYSTEM_KC")
  [ "${#KC_CANDIDATES[@]}" -gt 0 ] || fail "no IOUSBHostFamily binary and no system Kernel Collections were found"
  if ! "$PYTHON" "$HELPER" --entry "$TARGET_ENTRY" --symbol "$TARGET_SYMBOL" "${KC_CANDIDATES[@]}" > "$SOURCE_JSON" 2>"$TMP/probe.err"; then
    /bin/cat "$TMP/probe.err" >&2 || true
    fail "IOUSBHostFamily fileset entry/symbol was not uniquely resolved from the system Kernel Collections"
  fi
fi

read_json() {
  "$PYTHON" - "$SOURCE_JSON" "$1" <<'PY'
import json,sys
obj=json.load(open(sys.argv[1],encoding='utf-8'))
value=obj.get(sys.argv[2],"")
if value is None: value=""
print(value)
PY
}

SOURCE_KIND="$(read_json sourceKind)"
SOURCE_PATH="$(read_json sourcePath)"
SOURCE_SHA256="$(read_json sourceSHA256)"
IMAGE_SHA256="$(read_json imageSHA256)"
UUID_HEX="$(read_json uuidHex)"
SYMBOL="$(read_json mangledSymbol)"
ADDRESS_HEX="$(read_json symbolAddress)"
PROLOGUE_HEX="$(read_json prologueHex32)"
PROLOGUE_SHA256="$(read_json prologueSHA256)"
FILESET_ENTRY_ID="$(read_json filesetEntryID)"
FILESET_ENTRY_OFFSET="$(read_json filesetEntryOffset)"
FILESET_ENTRY_VMADDR="$(read_json filesetEntryVMAddress)"

[ "$SYMBOL" = "$TARGET_SYMBOL" ] || fail "unexpected exact symbol: $SYMBOL"
case "$ADDRESS_HEX" in ''|*[!0-9A-Fa-f]*) fail "unexpected symbol address: $ADDRESS_HEX" ;; esac
[ "${#PROLOGUE_HEX}" -eq 64 ] || fail "expected exactly 32 prologue bytes"
case "$UUID_HEX" in
  ????????????????????????????????) ;;
  *) fail "expected a 16-byte Mach-O UUID" ;;
esac
case "$IMAGE_SHA256" in
  ????????????????????????????????????????????????????????????????) ;;
  *) fail "invalid IOUSBHostFamily image hash" ;;
esac

CXXFILT="$(/usr/bin/xcrun -f llvm-cxxfilt 2>/dev/null || true)"
[ -x "$CXXFILT" ] || fail "llvm-cxxfilt is required from Xcode/CommandLineTools"
DEMANGLE_INPUT="$SYMBOL"
case "$DEMANGLE_INPUT" in __Z*) DEMANGLE_INPUT="${DEMANGLE_INPUT#_}" ;; esac
DEMANGLED="$(printf '%s\n' "$DEMANGLE_INPUT" | "$CXXFILT")"
[ "$DEMANGLED" = "$EXPECTED_SIGNATURE" ] || fail "unexpected synchronous signature: $DEMANGLED"

UUID_FORMATTED="$("$PYTHON" - "$UUID_HEX" <<'PY'
import sys
h=sys.argv[1].upper()
print(f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:32]}")
PY
)"
DARWIN="$(uname -r)"

"$PYTHON" - "$DARWIN" "$BUILD" "$SDK_COMMIT" "$SOURCE_KIND" "$SOURCE_PATH" "$SOURCE_SHA256" "$IMAGE_SHA256" "$UUID_FORMATTED" "$SYMBOL" "$DEMANGLED" "$ADDRESS_HEX" "$PROLOGUE_HEX" "$PROLOGUE_SHA256" "$FILESET_ENTRY_ID" "$FILESET_ENTRY_OFFSET" "$FILESET_ENTRY_VMADDR" <<'PY'
import json,sys
(darwin,build,sdk,source_kind,source_path,source_sha,image_sha,uuid,symbol,demangled,
 address,prologue,prologue_sha,entry_id,entry_off,entry_vmaddr)=sys.argv[1:]
out={
  "darwin":darwin,
  "build":build,
  "macKernelSDKCommit":sdk,
  "sourceKind":source_kind,
  "binaryPath":source_path,
  "sourceSHA256":source_sha,
  "ioUSBHostFamilyUUID":uuid,
  "ioUSBHostFamilySHA256":image_sha,
  "mangledSymbol":symbol,
  "demangledSignature":demangled,
  "symbolAddress":address,
  "prologueHex32":prologue,
  "prologueSHA256":prologue_sha,
}
if entry_id:
  out["filesetEntryID"]=entry_id
  out["filesetEntryOffset"]=entry_off
  out["filesetEntryVMAddress"]=entry_vmaddr
  out["kernelCollectionSHA256"]=source_sha
print(json.dumps(out,sort_keys=True,indent=2))
PY
