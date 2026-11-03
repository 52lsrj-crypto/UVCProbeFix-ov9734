#!/bin/bash
set -euo pipefail
IFS=$'\n\t'
umask 077
PATH="/usr/bin:/bin:/usr/sbin:/sbin"
export PATH

# UVCProbeFix 0.2.15 Tahoe Exact Binary Extractor
# Read-only diagnostics/extraction for macOS Tahoe 26.x.
# Copyright © 2026 DNMTechLabs <dnmtechlabs@gmail.com>

if [[ "$(uname -s 2>/dev/null || true)" != "Darwin" ]]; then
  echo "ERROR: this extractor must run on macOS. Nothing was changed."
  exit 1
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd -P)"
STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_ID="${STAMP}-$$"
OUT="$SCRIPT_DIR/UVCProbeFix-Tahoe-ExactBinary-$RUN_ID"
ZIP="$SCRIPT_DIR/UVCProbeFix-Tahoe-ExactBinary-$RUN_ID.zip"
if ! mkdir "$OUT"; then
  echo "ERROR: output directory already exists or cannot be created: $OUT"
  exit 1
fi
mkdir "$OUT/binaries" "$OUT/analysis" "$OUT/runtime"
LOG="$OUT/EXTRACTION.log"

# From here on, log every operation. This script is read-only with respect to macOS.
# Preserve console descriptors and track the tee process so EXTRACTION.log is
# completely closed before it is hashed and archived.
exec 3>&1 4>&2
exec > >(/usr/bin/tee -a "$LOG") 2>&1
TEE_PID="${!:-}"

printf '%s\n' '============================================================'
printf '%s\n' ' UVCProbeFix Tahoe Exact Binary Extractor'
printf '%s\n' ' Target: UVCAssistant / UVCExtension / UVCFamily / UVCFrameProcessor / VDC / VDCAssistant / IOUSBHost / UVCService'
printf '%s\n' ' READ-ONLY: no EFI/NVRAM/kext/system modification is performed'
printf '%s\n' '============================================================'
printf 'Started: %s\n\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

have() { command -v "$1" >/dev/null 2>&1; }

# Hard limits prevent an unexpected system layout from producing an unbounded archive.
MAX_COPY_BYTES=134217728       # 128 MiB per copied userspace binary
MAX_DISASSEMBLY_BLOCKS=131072 # 64 MiB at the POSIX 512-byte ulimit block size

capture() {
  local name="$1"; shift
  local dest="$OUT/analysis/$name"
  printf '\n--- %s ---\n' "$name"
  if "$@" >"$dest" 2>&1; then
    printf 'PASS: %s\n' "$name"
  else
    local rc=$?
    printf 'NOTE: %s returned %d (kept output)\n' "$name" "$rc"
  fi
}

copy_file() {
  local src="$1"
  local label="$2"
  if [[ -f "$src" && -r "$src" ]]; then
    local size=""
    size="$(/usr/bin/stat -f '%z' "$src" 2>/dev/null || true)"
    if [[ "$size" =~ ^[0-9]+$ ]] && (( size > MAX_COPY_BYTES )); then
      printf 'SKIPPED (too large: %s bytes): %s\n' "$size" "$src"
      return 2
    fi
    local dst="$OUT/binaries/${label}"
    /bin/cp -p "$src" "$dst"
    printf 'COPIED: %s -> %s\n' "$src" "$dst"
    return 0
  fi
  printf 'NOT FOUND/READABLE: %s\n' "$src"
  return 1
}

find_tool() {
  local tool="$1"
  if have "$tool"; then
    command -v "$tool"
    return 0
  fi
  if have xcrun; then
    xcrun -f "$tool" 2>/dev/null || true
  fi
}

OTOOL="$(find_tool otool)"
NM="$(find_tool nm)"
DWARFDUMP="$(find_tool dwarfdump)"
DYLD_INFO="$(find_tool dyld_info)"
CLANG="$(find_tool clang)"
STRINGS="$(find_tool strings)"
[[ -n "$STRINGS" ]] || STRINGS="/usr/bin/strings"

{
  echo "ExtractorVersion=2.4"
  echo "DateUTC=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "Machine=$(uname -m)"
  echo "Kernel=$(uname -r)"
  echo "OTOOL=${OTOOL:-missing}"
  echo "NM=${NM:-missing}"
  echo "DWARFDUMP=${DWARFDUMP:-missing}"
  echo "DYLD_INFO=${DYLD_INFO:-missing}"
  echo "CLANG=${CLANG:-missing}"
} > "$OUT/analysis/TOOLS.txt"

capture "sw_vers.txt" /usr/bin/sw_vers
capture "uname.txt" /usr/bin/uname -a
capture "boot-args.txt" /usr/sbin/nvram boot-args
if [[ -x /usr/bin/csrutil ]]; then
  capture "csrutil-status.txt" /usr/bin/csrutil status
  capture "csrutil-authenticated-root.txt" /usr/bin/csrutil authenticated-root status
fi
capture "systemextensionsctl-list.txt" /usr/bin/systemextensionsctl list
if [[ -x /usr/bin/kmutil ]]; then
  capture "kmutil-showloaded.txt" /usr/bin/kmutil showloaded
  capture "kmutil-inspect-IOUSBHostFamily.txt" /usr/bin/kmutil inspect -b com.apple.iokit.IOUSBHostFamily
  capture "kmutil-inspect-UVCService.txt" /usr/bin/kmutil inspect -b com.apple.UVCService
fi
capture "ioreg-usb.txt" /usr/sbin/ioreg -l -w0 -p IOUSB
capture "ioreg-camera-filter.txt" /bin/sh -c "/usr/sbin/ioreg -l -w0 | /usr/bin/grep -iE -C 12 '174F|1176|Integrated Camera|UVCAssistant|VDCAssistant|VDC|CameraAssistantBundleID|AppleUSBHostFrameworkInterfaceClient|UVCService' || true"
capture "processes-camera.txt" /bin/sh -c "/bin/ps -axo pid,ppid,user,%cpu,%mem,etime,command | /usr/bin/grep -iE 'UVCAssistant|VDCAssistant|VDC\.plugin|cmio|camera' | /usr/bin/grep -v grep || true"

UVCA_BUNDLE="/System/Library/Frameworks/CoreMediaIO.framework/Versions/A/Resources/UVCAssistant.systemextension"
UVCA_BIN="$UVCA_BUNDLE/Contents/MacOS/UVCAssistant"
UVCA_INFO="$UVCA_BUNDLE/Contents/Info.plist"
UVCE_FRAMEWORK="/System/Library/PrivateFrameworks/UVCExtension.framework"
UVCE_BIN="$UVCE_FRAMEWORK/Versions/A/UVCExtension"
[[ -e "$UVCE_BIN" ]] || UVCE_BIN="$UVCE_FRAMEWORK/UVCExtension"
UVCE_INFO="$UVCE_FRAMEWORK/Versions/A/Resources/Info.plist"
[[ -f "$UVCE_INFO" ]] || UVCE_INFO="$UVCE_FRAMEWORK/Resources/Info.plist"
UVCF_FRAMEWORK="/System/Library/PrivateFrameworks/UVCFamily.framework"
UVCF_BIN="$UVCF_FRAMEWORK/Versions/A/UVCFamily"
[[ -e "$UVCF_BIN" ]] || UVCF_BIN="$UVCF_FRAMEWORK/UVCFamily"
UVCF_INFO="$UVCF_FRAMEWORK/Versions/A/Resources/Info.plist"
[[ -f "$UVCF_INFO" ]] || UVCF_INFO="$UVCF_FRAMEWORK/Resources/Info.plist"
UVCP_FRAMEWORK="/System/Library/PrivateFrameworks/UVCFrameProcessor.framework"
UVCP_BIN="$UVCP_FRAMEWORK/Versions/A/UVCFrameProcessor"
[[ -e "$UVCP_BIN" ]] || UVCP_BIN="$UVCP_FRAMEWORK/UVCFrameProcessor"
UVCP_INFO="$UVCP_FRAMEWORK/Versions/A/Resources/Info.plist"
[[ -f "$UVCP_INFO" ]] || UVCP_INFO="$UVCP_FRAMEWORK/Resources/Info.plist"
IOUSB_BIN="/System/Library/Frameworks/IOUSBHost.framework/Versions/A/IOUSBHost"
[[ -e "$IOUSB_BIN" ]] || IOUSB_BIN="/System/Library/Frameworks/IOUSBHost.framework/IOUSBHost"

# Tahoe 26.x still ships the legacy VDC DAL plugin/assistant.  Collect it as a
# second, non-global candidate path so we can decide from the user's exact build
# whether it can own this camera without any IOUSBHostFamily kernel routing.
VDC_PLUGIN=""
for p in \
  "/System/Library/Frameworks/CoreMediaIO.framework/Resources/VDC.plugin" \
  "/System/Library/Frameworks/CoreMediaIO.framework/Versions/A/Resources/VDC.plugin"; do
  if [[ -d "$p" ]]; then VDC_PLUGIN="$p"; break; fi
done
VDC_BIN=""
VDC_INFO=""
VDC_ASSISTANT=""
if [[ -n "$VDC_PLUGIN" ]]; then
  VDC_INFO="$VDC_PLUGIN/Contents/Info.plist"
  VDC_BIN="$VDC_PLUGIN/Contents/MacOS/VDC"
  VDC_ASSISTANT="$VDC_PLUGIN/Contents/Resources/VDCAssistant"
fi
VDC_LAUNCHD=""
for p in \
  "/System/Library/LaunchDaemons/com.apple.cmio.VDCAssistant.plist" \
  "/System/Library/LaunchAgents/com.apple.cmio.VDCAssistant.plist"; do
  if [[ -f "$p" ]]; then VDC_LAUNCHD="$p"; break; fi
done

copy_file "$UVCA_BIN" "UVCAssistant" || true
copy_file "$UVCA_INFO" "UVCAssistant-Info.plist" || true
copy_file "$UVCE_BIN" "UVCExtension.framework.bin" || true
copy_file "$UVCE_INFO" "UVCExtension-Info.plist" || true
copy_file "$UVCF_BIN" "UVCFamily.framework.bin" || true
copy_file "$UVCF_INFO" "UVCFamily-Info.plist" || true
copy_file "$UVCP_BIN" "UVCFrameProcessor.framework.bin" || true
copy_file "$UVCP_INFO" "UVCFrameProcessor-Info.plist" || true
copy_file "$IOUSB_BIN" "IOUSBHost.framework.bin" || true
[[ -n "$VDC_BIN" ]] && copy_file "$VDC_BIN" "VDC.plugin.bin" || true
[[ -n "$VDC_INFO" ]] && copy_file "$VDC_INFO" "VDC.plugin-Info.plist" || true
[[ -n "$VDC_ASSISTANT" ]] && copy_file "$VDC_ASSISTANT" "VDCAssistant" || true
[[ -n "$VDC_LAUNCHD" ]] && copy_file "$VDC_LAUNCHD" "VDCAssistant-launchd.plist" || true

# Compact metadata for both assistants/plugins; failures are non-fatal because
# system layouts can differ between Tahoe point releases.
{
  echo "UVCAssistantBundle=$UVCA_BUNDLE"
  echo "UVCAssistantExecutable=$UVCA_BIN"
  echo "UVCExtensionFramework=$UVCE_FRAMEWORK"
  echo "UVCExtensionExecutable=$UVCE_BIN"
  echo "UVCFamilyFramework=$UVCF_FRAMEWORK"
  echo "UVCFamilyExecutable=$UVCF_BIN"
  echo "UVCFrameProcessorFramework=$UVCP_FRAMEWORK"
  echo "UVCFrameProcessorExecutable=$UVCP_BIN"
  if [[ -f "$UVCE_INFO" ]]; then
    echo "UVCExtension.CFBundleIdentifier=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$UVCE_INFO" 2>/dev/null || true)"
    echo "UVCExtension.CFBundleVersion=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$UVCE_INFO" 2>/dev/null || true)"
  fi
  if [[ -f "$UVCF_INFO" ]]; then
    echo "UVCFamily.CFBundleIdentifier=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$UVCF_INFO" 2>/dev/null || true)"
    echo "UVCFamily.CFBundleVersion=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$UVCF_INFO" 2>/dev/null || true)"
  fi
  if [[ -f "$UVCP_INFO" ]]; then
    echo "UVCFrameProcessor.CFBundleIdentifier=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$UVCP_INFO" 2>/dev/null || true)"
    echo "UVCFrameProcessor.CFBundleVersion=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$UVCP_INFO" 2>/dev/null || true)"
  fi
  if [[ -f "$UVCA_INFO" ]]; then
    echo "UVCAssistant.CFBundleIdentifier=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$UVCA_INFO" 2>/dev/null || true)"
    echo "UVCAssistant.CFBundleVersion=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$UVCA_INFO" 2>/dev/null || true)"
  fi
  echo "VDCPlugin=${VDC_PLUGIN:-not-found}"
  echo "VDCExecutable=${VDC_BIN:-not-found}"
  echo "VDCAssistantExecutable=${VDC_ASSISTANT:-not-found}"
  if [[ -n "$VDC_INFO" && -f "$VDC_INFO" ]]; then
    echo "VDC.CFBundleIdentifier=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$VDC_INFO" 2>/dev/null || true)"
    echo "VDC.CFBundleVersion=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$VDC_INFO" 2>/dev/null || true)"
    echo "VDC.CFBundleShortVersionString=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$VDC_INFO" 2>/dev/null || true)"
  fi
  echo "VDCAssistantLaunchd=${VDC_LAUNCHD:-not-found}"
} > "$OUT/analysis/CMIO-ASSISTANTS-METADATA.txt"

# Locate on-disk kext bundles if present on this sealed system volume.
UVC_KEXT=""
USBHOST_KEXT=""
for p in \
  "/System/Library/Extensions/UVCService.kext" \
  "/System/Library/Extensions/DriverKit-UVCUserService.kext"; do
  if [[ -d "$p" ]]; then UVC_KEXT="$p"; break; fi
done
for p in \
  "/System/Library/Extensions/IOUSBHostFamily.kext" \
  "/System/Library/Extensions/AppleUSBHostFamily.kext"; do
  if [[ -d "$p" ]]; then USBHOST_KEXT="$p"; break; fi
done

# Kernel-side binaries are intentionally NOT copied/disassembled in 0.2.15.
# The unsafe kernel-hook architecture was retired; kmutil metadata plus Info.plist
# are sufficient for compatibility triage and avoid large, irrelevant archives.
if [[ -n "$UVC_KEXT" ]]; then
  copy_file "$UVC_KEXT/Contents/Info.plist" "UVCService-Info.plist" || true
fi
if [[ -n "$USBHOST_KEXT" ]]; then
  copy_file "$USBHOST_KEXT/Contents/Info.plist" "IOUSBHostFamily-Info.plist" || true
fi

analyze_macho() {
  local src="$1"
  local tag="$2"
  [[ -f "$src" && -r "$src" ]] || return 0
  local base="$OUT/analysis/$tag"
  mkdir -p "$base"
  printf '\n=== Mach-O analysis: %s ===\n' "$src"

  /usr/bin/file "$src" > "$base/file.txt" 2>&1 || true
  /usr/bin/shasum -a 256 "$src" > "$base/sha256.txt" 2>&1 || true
  /usr/bin/codesign -dvvv --verbose=4 "$src" > "$base/codesign.txt" 2>&1 || true
  /usr/bin/codesign -d --entitlements :- "$src" > "$base/entitlements.plist" 2>&1 || true

  if [[ -n "$OTOOL" ]]; then
    "$OTOOL" -hv "$src" > "$base/otool-hv.txt" 2>&1 || true
    "$OTOOL" -l "$src" > "$base/otool-loadcommands.txt" 2>&1 || true
    "$OTOOL" -L "$src" > "$base/otool-libraries.txt" 2>&1 || true
    "$OTOOL" -Iv "$src" > "$base/otool-indirect-symbols.txt" 2>&1 || true
    "$OTOOL" -ov "$src" > "$base/otool-objc.txt" 2>&1 || true
    "$OTOOL" -v -s __TEXT __cstring "$src" > "$base/otool-cstring.txt" 2>&1 || true
    "$OTOOL" -v -s __TEXT __objc_methname "$src" > "$base/otool-objc-methname.txt" 2>&1 || true
    # x86_64 disassembly is useful for a build-specific shared-cache patch, but cap
    # its output so a changed system binary cannot create an unbounded archive.
    (
      ulimit -f "$MAX_DISASSEMBLY_BLOCKS" 2>/dev/null || true
      "$OTOOL" -arch x86_64 -tvV "$src" > "$base/disassembly-x86_64.txt" 2>&1 || true
    )
  fi

  if [[ -n "$NM" ]]; then
    "$NM" -arch x86_64 -nm "$src" > "$base/nm-nm-x86_64.txt" 2>&1 || "$NM" -nm "$src" > "$base/nm-nm.txt" 2>&1 || true
    "$NM" -arch x86_64 -m "$src" > "$base/nm-m-x86_64.txt" 2>&1 || "$NM" -m "$src" > "$base/nm-m.txt" 2>&1 || true
    "$NM" -arch x86_64 -u "$src" > "$base/nm-undefined-x86_64.txt" 2>&1 || "$NM" -u "$src" > "$base/nm-undefined.txt" 2>&1 || true
  fi

  if [[ -n "$DWARFDUMP" ]]; then
    "$DWARFDUMP" --uuid "$src" > "$base/uuid.txt" 2>&1 || true
  fi

  "$STRINGS" -a -t x "$src" > "$base/strings-offsets.txt" 2>&1 || "$STRINGS" -a "$src" > "$base/strings.txt" 2>&1 || true
  /usr/bin/grep -iE -C 4 'MaxPayload|dwMaxPayload|SET_CUR|GET_CUR|PROBE|COMMIT|sendDeviceRequest|UVCUSBDeviceStreamingInterface|Invalid stream|Start Stream|Stream Failed|bytesTransferred|IOUSBHost' \
    "$base/strings-offsets.txt" > "$base/strings-UVC-relevant.txt" 2>/dev/null || true

  if [[ -n "$DYLD_INFO" ]]; then
    "$DYLD_INFO" -exports "$src" > "$base/dyld-exports.txt" 2>&1 || true
    "$DYLD_INFO" -imports "$src" > "$base/dyld-imports.txt" 2>&1 || true
    "$DYLD_INFO" -fixups "$src" > "$base/dyld-fixups.txt" 2>&1 || true
    "$DYLD_INFO" -objc "$src" > "$base/dyld-objc.txt" 2>&1 || true
  fi
}

analyze_macho "$UVCA_BIN" "UVCAssistant"
analyze_macho "$UVCE_BIN" "UVCExtension-framework"
analyze_macho "$UVCF_BIN" "UVCFamily-framework"
analyze_macho "$UVCP_BIN" "UVCFrameProcessor-framework"
analyze_macho "$IOUSB_BIN" "IOUSBHost-framework"
[[ -n "$VDC_BIN" && -f "$VDC_BIN" ]] && analyze_macho "$VDC_BIN" "VDC-plugin"
[[ -n "$VDC_ASSISTANT" && -f "$VDC_ASSISTANT" ]] && analyze_macho "$VDC_ASSISTANT" "VDCAssistant"

# Runtime Objective-C introspection of the exact UVC userspace frameworks.
# This captures class ownership and method implementation bytes even when an
# on-disk framework is backed by the dyld shared cache.
UVC_RUNTIME_SRC="$OUT/runtime/UVCRuntimeProbe.m"
UVC_RUNTIME_BIN="$OUT/runtime/UVCRuntimeProbe"
cat > "$UVC_RUNTIME_SRC" <<'OBJC_UVC'
#define _DARWIN_C_SOURCE 1
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#import <mach-o/dyld.h>
#import <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool containsCI(const char *s, const char *needle) {
    if (!s || !needle) return false;
    size_t n = strlen(needle);
    if (n == 0) return true;
    for (const char *p = s; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i]) {
            unsigned char a = (unsigned char)p[i];
            unsigned char b = (unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) break;
            ++i;
        }
        if (i == n) return true;
    }
    return false;
}

static bool interesting(const char *name) {
    static const char *keys[] = {
        "probe", "commit", "payload", "stream", "alternate", "interface",
        "format", "negot", "request", "start", "stop", "frame", "pipe", "endpoint"
    };
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i)
        if (containsCI(name, keys[i])) return true;
    return false;
}

static bool uvcImage(const char *image) {
    if (!image) return false;
    return strstr(image, "/UVCExtension.framework/") ||
           strstr(image, "/UVCFamily.framework/") ||
           strstr(image, "/UVCFrameProcessor.framework/");
}

static uintptr_t textEndForAddress(uintptr_t target, const char **imageNameOut) {
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const struct mach_header *raw = _dyld_get_image_header(i);
        if (!raw || raw->magic != MH_MAGIC_64) continue;
        const struct mach_header_64 *mh = (const struct mach_header_64 *)raw;
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const uint8_t *p = (const uint8_t *)(mh + 1);
        uintptr_t pAddr = (uintptr_t)p;
        if ((uintptr_t)mh->sizeofcmds > UINTPTR_MAX - pAddr) continue;
        const uint8_t *commandsEnd = (const uint8_t *)(pAddr + (uintptr_t)mh->sizeofcmds);
        for (uint32_t c = 0; c < mh->ncmds; ++c) {
            if ((size_t)(commandsEnd - p) < sizeof(struct load_command)) break;
            const struct load_command *lc = (const struct load_command *)p;
            if (lc->cmdsize < sizeof(struct load_command) || (size_t)(commandsEnd - p) < lc->cmdsize) break;
            if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(struct segment_command_64)) {
                const struct segment_command_64 *seg = (const struct segment_command_64 *)p;
                if (strncmp(seg->segname, "__TEXT", 16) == 0) {
                    uintptr_t start = (uintptr_t)(seg->vmaddr + slide);
                    if ((uintptr_t)seg->vmsize <= UINTPTR_MAX - start) {
                        uintptr_t end = start + (uintptr_t)seg->vmsize;
                        if (target >= start && target < end) {
                            if (imageNameOut) *imageNameOut = _dyld_get_image_name(i);
                            return end;
                        }
                    }
                }
            }
            p += lc->cmdsize;
        }
    }
    return 0;
}

static void dumpBytes(const uint8_t *p, size_t n, uintptr_t base) {
    for (size_t i = 0; i < n; i += 16) {
        printf("%016llx  ", (unsigned long long)(base + i));
        size_t line = (n - i < 16) ? (n - i) : 16;
        for (size_t j = 0; j < line; ++j) printf("%02x ", p[i + j]);
        printf("\n");
    }
}

static void inspectMethods(Class cls, bool classMethods) {
    const char *className = class_getName(cls);
    Class target = classMethods ? object_getClass((id)cls) : cls;
    unsigned count = 0;
    Method *methods = class_copyMethodList(target, &count);
    printf("%sMethodCount=%u\n", classMethods ? "class" : "instance", count);
    if (!methods && count != 0) return;
    for (unsigned i = 0; i < count; ++i) {
        SEL sel = method_getName(methods[i]);
        const char *selName = sel_getName(sel);
        const char *types = method_getTypeEncoding(methods[i]);
        IMP imp = method_getImplementation(methods[i]);
        Dl_info di = {0}; dladdr((void *)imp, &di);
        printf("%c %s | types=%s | IMP=%p | image=%s\n",
               classMethods ? 'C' : 'I', selName ? selName : "(null)",
               types ? types : "(null)", imp, di.dli_fname ? di.dli_fname : "(null)");
        if (!interesting(selName) || !uvcImage(di.dli_fname)) continue;
        const char *dyldName = NULL;
        uintptr_t addr = (uintptr_t)imp;
        uintptr_t end = textEndForAddress(addr, &dyldName);
        printf("=== DUMP %s %s %s ===\n", classMethods ? "CLASS" : "INSTANCE",
               className ? className : "(null)", selName ? selName : "(null)");
        printf("dladdr.image=%s\n", di.dli_fname ? di.dli_fname : "(null)");
        printf("dladdr.imageBase=%p\n", di.dli_fbase);
        printf("dladdr.symbol=%s\n", di.dli_sname ? di.dli_sname : "(null)");
        printf("dyld.image=%s\n", dyldName ? dyldName : "(null)");
        if (end > addr) {
            size_t available = (size_t)(end - addr);
            size_t wanted = 512;
            if (selName && strcmp(selName, "doProbeControl:frameRate:probeData:error:") == 0)
                wanted = 8192;
            else if (selName && strcmp(selName, "getStreamInPipeOfType:type:probeData:alternateSetting:") == 0)
                wanted = 8192;
            else if (selName && strcmp(selName, "doCommitControl:error:") == 0)
                wanted = 4096;
            size_t dumpLen = available < wanted ? available : wanted;
            printf("dumpLength=%zu\n", dumpLen);
            dumpBytes((const uint8_t *)imp, dumpLen, addr);
        } else {
            printf("NO_SAFE_TEXT_RANGE\n");
        }
    }
    free(methods);
}

int main(void) {
    @autoreleasepool {
        NSArray<NSString *> *paths = @[
            @"/System/Library/PrivateFrameworks/UVCExtension.framework",
            @"/System/Library/PrivateFrameworks/UVCFamily.framework",
            @"/System/Library/PrivateFrameworks/UVCFrameProcessor.framework"
        ];
        for (NSString *path in paths) {
            NSBundle *bundle = [NSBundle bundleWithPath:path];
            NSError *error = nil;
            BOOL ok = [bundle loadAndReturnError:&error];
            printf("%s load=%s\n", path.UTF8String, ok ? "YES" : "NO");
            if (!ok && error) printf("loadError=%s\n", error.localizedDescription.UTF8String);
        }
        printf("OS=%s\n", NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
        int count = objc_getClassList(NULL, 0);
        printf("objcClassCount=%d\n", count);
        if (count <= 0) return 0;
        Class *classes = (Class *)calloc((size_t)count, sizeof(Class));
        if (!classes) return 2;
        int got = objc_getClassList(classes, count);
        for (int i = 0; i < got; ++i) {
            Class cls = classes[i];
            const char *name = class_getName(cls);
            const char *image = class_getImageName(cls);
            if (!uvcImage(image) && !(name && strncmp(name, "UVC", 3) == 0)) continue;
            printf("\n##### CLASS %s | image=%s #####\n", name ? name : "(null)", image ? image : "(null)");
            inspectMethods(cls, false);
            inspectMethods(cls, true);
        }
        free(classes);
    }
    return 0;
}
OBJC_UVC

if [[ -n "$CLANG" ]]; then
  printf '\nCompiling runtime UVC framework introspector...\n'
  if "$CLANG" -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation "$UVC_RUNTIME_SRC" -o "$UVC_RUNTIME_BIN" > "$OUT/runtime/uvc-runtime-compile.txt" 2>&1; then
    printf 'PASS: runtime UVC framework helper compiled\n'
    if "$UVC_RUNTIME_BIN" > "$OUT/runtime/UVC-framework-runtime-methods.txt" 2>&1; then
      printf 'PASS: runtime UVC framework method/IMP dump collected\n'
    else
      printf 'NOTE: runtime UVC framework helper execution failed; see runtime output\n'
    fi
  else
    printf 'NOTE: runtime UVC framework helper could not compile; see runtime/uvc-runtime-compile.txt\n'
  fi
else
  printf 'NOTE: clang unavailable; runtime UVC framework introspection skipped\n'
fi

# Runtime Objective-C introspection of the exact IOUSBHost.framework image loaded by this OS.
# This is useful even when the framework's on-disk file is only a shared-cache stub.
HELPER_SRC="$OUT/runtime/IOUSBRuntimeProbe.m"
HELPER_BIN="$OUT/runtime/IOUSBRuntimeProbe"
cat > "$HELPER_SRC" <<'OBJC'
#define _DARWIN_C_SOURCE 1
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#import <mach-o/dyld.h>
#import <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uintptr_t textEndForAddress(uintptr_t target, const char **imageNameOut, const struct mach_header_64 **headerOut) {
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const struct mach_header *raw = _dyld_get_image_header(i);
        if (!raw || raw->magic != MH_MAGIC_64) continue;
        const struct mach_header_64 *mh = (const struct mach_header_64 *)raw;
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const uint8_t *p = (const uint8_t *)(mh + 1);
        uintptr_t pAddr = (uintptr_t)p;
        if ((uintptr_t)mh->sizeofcmds > UINTPTR_MAX - pAddr) continue;
        const uint8_t *commandsEnd = (const uint8_t *)(pAddr + (uintptr_t)mh->sizeofcmds);
        for (uint32_t c = 0; c < mh->ncmds; ++c) {
            if ((size_t)(commandsEnd - p) < sizeof(struct load_command)) break;
            const struct load_command *lc = (const struct load_command *)p;
            if (lc->cmdsize < sizeof(struct load_command) || (size_t)(commandsEnd - p) < lc->cmdsize) break;
            if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(struct segment_command_64)) {
                const struct segment_command_64 *seg = (const struct segment_command_64 *)p;
                if (strncmp(seg->segname, "__TEXT", 16) == 0) {
                    uintptr_t start = (uintptr_t)(seg->vmaddr + slide);
                    if ((uintptr_t)seg->vmsize <= UINTPTR_MAX - start) {
                        uintptr_t end = start + (uintptr_t)seg->vmsize;
                        if (target >= start && target < end) {
                            if (imageNameOut) *imageNameOut = _dyld_get_image_name(i);
                            if (headerOut) *headerOut = mh;
                            return end;
                        }
                    }
                }
            }
            p += lc->cmdsize;
        }
    }
    return 0;
}

static void dumpHex(const uint8_t *p, size_t n, uintptr_t base) {
    for (size_t i = 0; i < n; i += 16) {
        printf("%016llx  ", (unsigned long long)(base + i));
        size_t line = (n - i < 16) ? (n - i) : 16;
        for (size_t j = 0; j < line; ++j) printf("%02x ", p[i + j]);
        printf("\n");
    }
}

static void dumpMethod(const char *className, const char *selectorName) {
    Class cls = objc_getClass(className);
    printf("\n=== METHOD %s :: %s ===\n", className, selectorName);
    if (!cls) { printf("CLASS_NOT_FOUND\n"); return; }
    SEL sel = sel_registerName(selectorName);
    Method m = class_getInstanceMethod(cls, sel);
    if (!m) { printf("METHOD_NOT_FOUND\n"); return; }
    IMP imp = method_getImplementation(m);
    const char *types = method_getTypeEncoding(m);
    Dl_info di = {0};
    dladdr((void *)imp, &di);
    printf("typeEncoding=%s\n", types ? types : "(null)");
    printf("IMP=%p\n", imp);
    printf("dladdr.image=%s\n", di.dli_fname ? di.dli_fname : "(null)");
    printf("dladdr.imageBase=%p\n", di.dli_fbase);
    printf("dladdr.symbol=%s\n", di.dli_sname ? di.dli_sname : "(null)");
    printf("dladdr.symbolAddress=%p\n", di.dli_saddr);

    const char *dyldName = NULL;
    const struct mach_header_64 *mh = NULL;
    uintptr_t addr = (uintptr_t)imp;
    uintptr_t textEnd = textEndForAddress(addr, &dyldName, &mh);
    printf("dyld.image=%s\n", dyldName ? dyldName : "(null)");
    printf("dyld.header=%p\n", mh);
    if (textEnd > addr) {
        size_t available = (size_t)(textEnd - addr);
        size_t dumpLen = available < 4096 ? available : 4096;
        printf("dumpLength=%zu\n", dumpLen);
        dumpHex((const uint8_t *)imp, dumpLen, addr);
    } else {
        printf("NO_SAFE_TEXT_RANGE\n");
    }
}

static void listMethods(const char *className) {
    Class cls = objc_getClass(className);
    printf("\n=== CLASS METHODS %s ===\n", className);
    if (!cls) { printf("CLASS_NOT_FOUND\n"); return; }
    unsigned count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    printf("count=%u\n", count);
    if (!methods && count != 0) {
        printf("METHOD_LIST_ALLOCATION_FAILED\n");
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        SEL s = method_getName(methods[i]);
        IMP imp = method_getImplementation(methods[i]);
        const char *types = method_getTypeEncoding(methods[i]);
        Dl_info di = {0}; dladdr((void *)imp, &di);
        printf("%s | IMP=%p | types=%s | image=%s\n",
               sel_getName(s), imp, types ? types : "(null)", di.dli_fname ? di.dli_fname : "(null)");
    }
    free(methods);
}

int main(void) {
    @autoreleasepool {
        NSString *path = @"/System/Library/Frameworks/IOUSBHost.framework";
        NSBundle *bundle = [NSBundle bundleWithPath:path];
        NSError *error = nil;
        BOOL ok = [bundle loadAndReturnError:&error];
        printf("IOUSBHost.framework load=%s\n", ok ? "YES" : "NO");
        if (!ok && error) printf("loadError=%s\n", error.localizedDescription.UTF8String);
        printf("OS=%s\n", NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);

        listMethods("IOUSBHostObject");
        listMethods("IOUSBHostInterface");
        listMethods("IOUSBHostDevice");

        dumpMethod("IOUSBHostObject", "sendDeviceRequest:data:bytesTransferred:completionTimeout:error:");
        dumpMethod("IOUSBHostInterface", "sendDeviceRequest:data:bytesTransferred:completionTimeout:error:");
        dumpMethod("IOUSBHostObject", "sendDeviceRequest:data:bytesTransferred:error:");
        dumpMethod("IOUSBHostInterface", "sendDeviceRequest:data:bytesTransferred:error:");
        dumpMethod("IOUSBHostObject", "enqueueDeviceRequest:data:completionTimeout:error:completionHandler:");
    }
    return 0;
}
OBJC

if [[ -n "$CLANG" ]]; then
  printf '\nCompiling runtime IOUSBHost introspector...\n'
  if "$CLANG" -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation "$HELPER_SRC" -o "$HELPER_BIN" > "$OUT/runtime/compile.txt" 2>&1; then
    printf 'PASS: runtime helper compiled\n'
    if "$HELPER_BIN" > "$OUT/runtime/IOUSBHost-runtime-methods.txt" 2>&1; then
      printf 'PASS: runtime IOUSBHost method/IMP dump collected\n'
    else
      printf 'NOTE: runtime helper execution failed; see runtime output\n'
    fi
  else
    printf 'NOTE: runtime helper could not compile; see runtime/compile.txt\n'
  fi
else
  printf 'NOTE: clang unavailable; runtime IMP introspection skipped\n'
fi

# Focused current unified-log snapshot, no live streaming and therefore no dropped stream issue.
if [[ -x /usr/bin/log ]]; then
  capture "recent-UVC-unified-log.txt" /usr/bin/log show --last 20m --style syslog --info --debug --predicate 'process == "UVCAssistant" OR process == "VDCAssistant" OR eventMessage CONTAINS[c] "UVCUSBDeviceStreamingInterface" OR eventMessage CONTAINS[c] "MaxPayloadSize" OR eventMessage CONTAINS[c] "SET_CURR" OR eventMessage CONTAINS[c] "GET_CUR" OR eventMessage CONTAINS[c] "Start Stream" OR eventMessage CONTAINS[c] "stream failed"'
fi

# Close/reap the logger before hashing. Otherwise EXTRACTION.log could change
# after its digest was calculated, making the manifest internally inconsistent.
printf '\nFinalizing extraction log before manifest/package...\n'
exec 1>&3 2>&4
exec 3>&- 4>&-
if [[ -n "${TEE_PID:-}" ]]; then
  wait "$TEE_PID" 2>/dev/null || true
  TEE_PID=""
fi

# One compact manifest of package payload.  Exclude the manifest itself so its
# digest is deterministic and never races against its own redirection.
rm -f "$OUT/SHA256SUMS.txt"
(
  cd "$OUT"
  /usr/bin/find . -type f ! -name 'SHA256SUMS.txt' -print0 | /usr/bin/sort -z | while IFS= read -r -d '' f; do
    /usr/bin/shasum -a 256 "$f"
  done
) > "$OUT/SHA256SUMS.txt"

printf '\nCreating ZIP...\n'
rm -f "$ZIP"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$OUT" "$ZIP"
if have unzip; then
  if ! /usr/bin/unzip -tq "$ZIP" >/dev/null 2>&1; then
    echo "ERROR: final ZIP verification failed"
    exit 2
  fi
  printf 'PASS: final ZIP integrity verified\n'
fi
ZIP_SHA="$(/usr/bin/shasum -a 256 "$ZIP" | /usr/bin/awk '{print $1}')"
printf '\n============================================================\n'
printf 'DONE\nZIP: %s\nSHA-256: %s\n' "$ZIP" "$ZIP_SHA"
printf 'Upload this ZIP to ChatGPT. It contains the exact 25Gxx UVCAssistant/UVCExtension/UVCFamily/UVCFrameProcessor userspace binaries, runtime method dumps, and kernel metadata needed to build the build-specific UVC shared-cache patch safely.\n'
printf 'No system files were modified.\n'
printf '============================================================\n'
