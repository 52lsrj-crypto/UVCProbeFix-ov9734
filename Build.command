#!/bin/bash
#
# UVCProbeFix 0.3.2 builder / auditor
# Copyright © 2026 DNMTechLabs. All rights reserved.
# Contact: dnmtechlabs@gmail.com
#
# Builds only inside this source tree. It never installs a kext, edits EFI,
# writes NVRAM, or modifies macOS system files.

set -euo pipefail
IFS=$'\n\t'
PATH="/usr/bin:/bin:/usr/sbin:/sbin"
export PATH

ROOT="$(cd "$(dirname "$0")" && pwd -P)"
DEPS="${ROOT}/.deps"
BUILD="${ROOT}/build"
DIST="${ROOT}/dist"
TEST_BUILD="${ROOT}/.test-build"
LOG="${BUILD}/Build-UVCProbeFix.log"
VERSION="0.3.2"
BUILD_NUMBER="302"
CONFIGURATION="${UVCP_CONFIGURATION:-Release}"
case "$CONFIGURATION" in Release|Debug) ;; *) fail_config=1 ;; esac
[ "${fail_config:-0}" -eq 0 ] || { echo "ERROR: UVCP_CONFIGURATION must be Release or Debug" >&2; exit 1; }

LILU_VERSION="1.7.2"
LILU_DEBUG_URL="https://github.com/acidanthera/Lilu/releases/download/${LILU_VERSION}/Lilu-${LILU_VERSION}-DEBUG.zip"
LILU_DEBUG_SHA256="e95df95d82e8b151047abf359ecd9c853755c59293e19dfcc3907da8fbd8cf89"
SDK_REPO="https://github.com/acidanthera/MacKernelSDK.git"
SDK_COMMIT="05094e5e88cec7caedbfb35e8449ed0db94bf95b"

pause_if_interactive() {
    if [ -t 0 ]; then
        printf '\nPress ENTER to close...'
        IFS= read -r _ || true
    fi
}

fail() {
    printf '\nERROR: %s\n' "$*" >&2
    pause_if_interactive
    exit 1
}

safe_remove_dir() {
    local target="$1"
    case "$target" in
        "${BUILD}"|"${DIST}"|"${TEST_BUILD}"|"${DEPS}/lilu-unpack"|"${DEPS}/MacKernelSDK")
            /bin/rm -rf -- "$target"
            ;;
        *)
            fail "refusing to remove unexpected path: $target"
            ;;
    esac
}

[ "$(/usr/bin/uname -s)" = "Darwin" ] || fail "this build command must run on macOS"

for tool in \
    /usr/bin/xcodebuild /usr/bin/xcode-select /usr/bin/curl /usr/bin/unzip \
    /usr/bin/git /usr/bin/plutil /usr/bin/clang++ /usr/bin/shasum \
    /usr/bin/ditto /usr/bin/zip /usr/bin/find /usr/bin/head /usr/bin/grep \
    /usr/bin/awk /usr/bin/nm /usr/bin/file /usr/bin/cmp /usr/bin/tee \
    /usr/bin/diff /usr/bin/readlink /usr/bin/xcrun /usr/bin/codesign; do
    [ -x "$tool" ] || fail "required tool not found: $tool"
done

/usr/bin/xcode-select -p >/dev/null 2>&1 || fail "Xcode command-line tools are not selected"
PYTHON=""
for candidate in /usr/bin/python3 "$(/usr/bin/xcrun -f python3 2>/dev/null || true)" "$(command -v python3 2>/dev/null || true)"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then
        PYTHON="$candidate"
        break
    fi
done
[ -n "$PYTHON" ] || fail "python3 is required by the source regression validator"
printf 'Python validator: %s\n' "$PYTHON"

[ -f "${ROOT}/SOURCE.sha256" ] || fail "SOURCE.sha256 is missing"
printf '>>> Verifying immutable source manifest\n'
(
    cd "$ROOT"
    /usr/bin/shasum -a 256 -c SOURCE.sha256
) || fail "source manifest verification failed; source tree is modified or incomplete"

safe_remove_dir "$BUILD"
safe_remove_dir "$DIST"
safe_remove_dir "$TEST_BUILD"
/bin/mkdir -p "$BUILD" "$DIST" "$TEST_BUILD" "$DEPS"

# Keep the build log stable and deterministic at exit.
exec 3>&1 4>&2
exec > >(/usr/bin/tee "$LOG") 2>&1
TEE_PID="${!:-}"

printf '============================================================\n'
printf ' UVCProbeFix %s %s build / audit\n' "$VERSION" "$CONFIGURATION"
printf '============================================================\n\n'

# ---------------------------------------------------------------------------
# 1. Dependency preparation (pinned)
# ---------------------------------------------------------------------------
LILU_LINK="${ROOT}/Lilu.kext"
LILU_ZIP="${DEPS}/Lilu-${LILU_VERSION}-DEBUG.zip"
LILU_UNPACK="${DEPS}/lilu-unpack"

validate_lilu_sdk() {
    [ -f "${LILU_LINK}/Contents/Resources/Headers/plugin_start.hpp" ] || return 1
    [ -f "${LILU_LINK}/Contents/Resources/Library/plugin_start.cpp" ] || return 1
    [ -f "${LILU_LINK}/Contents/Info.plist" ] || return 1
    local observed bundle_id
    observed="$(/usr/bin/plutil -extract CFBundleShortVersionString raw -o - "${LILU_LINK}/Contents/Info.plist" 2>/dev/null || true)"
    [ "$observed" = "$LILU_VERSION" ] || return 1
    bundle_id="$(/usr/bin/plutil -extract CFBundleIdentifier raw -o - "${LILU_LINK}/Contents/Info.plist" 2>/dev/null || true)"
    [ "$bundle_id" = "as.vit9696.Lilu" ] || return 1
}

if [ -e "$LILU_LINK" ] && [ ! -L "$LILU_LINK" ]; then
    fail "$LILU_LINK exists and is not a symlink; remove it so the pinned SDK can be prepared"
fi

printf '>>> Preparing verified Lilu %s Debug SDK\n' "$LILU_VERSION"
if [ -s "$LILU_ZIP" ]; then
    ACTUAL_LILU_SHA="$(/usr/bin/shasum -a 256 "$LILU_ZIP" | /usr/bin/awk '{print $1}')"
    if [ "$ACTUAL_LILU_SHA" != "$LILU_DEBUG_SHA256" ]; then
        printf 'Cached Lilu archive hash mismatch; discarding cache and downloading pinned asset.\n'
        /bin/rm -f -- "$LILU_ZIP"
    fi
fi
if [ ! -s "$LILU_ZIP" ]; then
    /usr/bin/curl -L --fail --retry 3 --connect-timeout 20 \
        -o "${LILU_ZIP}.part" "$LILU_DEBUG_URL" || fail "failed to download Lilu Debug SDK"
    /bin/mv -f -- "${LILU_ZIP}.part" "$LILU_ZIP"
fi
ACTUAL_LILU_SHA="$(/usr/bin/shasum -a 256 "$LILU_ZIP" | /usr/bin/awk '{print $1}')"
[ "$ACTUAL_LILU_SHA" = "$LILU_DEBUG_SHA256" ] || fail "Lilu Debug archive SHA-256 mismatch"
safe_remove_dir "$LILU_UNPACK"
/bin/mkdir -p "$LILU_UNPACK"
/usr/bin/unzip -q "$LILU_ZIP" -d "$LILU_UNPACK" || fail "failed to extract Lilu Debug SDK"
LILU_COUNT="$(/usr/bin/find "$LILU_UNPACK" -type d -name Lilu.kext -print | /usr/bin/awk 'END { print NR + 0 }')"
[ "$LILU_COUNT" -eq 1 ] || fail "expected exactly one Lilu.kext in verified Debug archive, found $LILU_COUNT"
FOUND_LILU="$(/usr/bin/find "$LILU_UNPACK" -type d -name Lilu.kext -print | /usr/bin/head -n 1)"
[ -n "$FOUND_LILU" ] || fail "Lilu.kext not found in verified Debug archive"
[ -f "${FOUND_LILU}/Contents/Resources/Headers/plugin_start.hpp" ] || fail "Lilu Debug SDK headers missing"
[ -f "${FOUND_LILU}/Contents/Resources/Library/plugin_start.cpp" ] || fail "Lilu plugin_start.cpp missing"
/bin/ln -sfn "$FOUND_LILU" "$LILU_LINK"
validate_lilu_sdk || fail "prepared Lilu SDK validation failed"

SDK_LINK="${ROOT}/MacKernelSDK"
SDK_CLONE="${DEPS}/MacKernelSDK"

validate_kernel_sdk() {
    [ -d "${SDK_LINK}/Headers" ] || return 1
    [ -f "${SDK_LINK}/Library/x86_64/libkmod.a" ] || return 1
    local observed origin dirty
    observed="$(/usr/bin/git -C "$SDK_LINK" rev-parse HEAD 2>/dev/null || true)"
    [ "$observed" = "$SDK_COMMIT" ] || return 1
    origin="$(/usr/bin/git -C "$SDK_LINK" remote get-url origin 2>/dev/null || true)"
    [ "$origin" = "$SDK_REPO" ] || return 1
    dirty="$(/usr/bin/git -C "$SDK_LINK" status --porcelain --untracked-files=no 2>/dev/null || true)"
    [ -z "$dirty" ] || return 1
}

if [ -e "$SDK_LINK" ] && [ ! -L "$SDK_LINK" ]; then
    fail "$SDK_LINK exists and is not a symlink; remove it so the pinned SDK can be prepared"
fi
if ! validate_kernel_sdk; then
    printf '>>> Preparing pinned MacKernelSDK %s\n' "$SDK_COMMIT"
    safe_remove_dir "$SDK_CLONE"
    /usr/bin/git init -q "$SDK_CLONE" || fail "failed to initialise MacKernelSDK repository"
    /usr/bin/git -C "$SDK_CLONE" remote add origin "$SDK_REPO" || fail "failed to add MacKernelSDK remote"
    /usr/bin/git -C "$SDK_CLONE" fetch -q --depth 1 origin "$SDK_COMMIT" || fail "failed to fetch pinned MacKernelSDK commit"
    /usr/bin/git -C "$SDK_CLONE" checkout -q --detach FETCH_HEAD || fail "failed to checkout pinned MacKernelSDK commit"
    [ "$(/usr/bin/git -C "$SDK_CLONE" rev-parse HEAD)" = "$SDK_COMMIT" ] || fail "MacKernelSDK commit mismatch"
    [ -d "${SDK_CLONE}/Headers" ] || fail "MacKernelSDK headers missing"
    [ -f "${SDK_CLONE}/Library/x86_64/libkmod.a" ] || fail "MacKernelSDK libkmod.a missing"
    /bin/ln -sfn "$SDK_CLONE" "$SDK_LINK"
    validate_kernel_sdk || fail "prepared MacKernelSDK validation failed"
fi

# ---------------------------------------------------------------------------
# 2. Target Tahoe IOUSBHost ABI gate (Debug/Test generic transport only)
# ---------------------------------------------------------------------------
if [ "$CONFIGURATION" = "Debug" ]; then
    case "$(/usr/bin/uname -r)" in
        24.*)
            printf '>>> Sequoia exact shared-cache path: skipping Tahoe generic IOUSBHost ABI gate\n'
            ;;
        *)
            printf '>>> Auditing Tahoe synchronous IOUSBHost ABI\n'
            "${ROOT}/Tools/Tahoe-IOUSBHost-ABI-Audit.command" > "${BUILD}/Tahoe-IOUSBHost-ABI.json" || \
                fail "Tahoe IOUSBHost ABI audit failed"
            [ -s "${BUILD}/Tahoe-IOUSBHost-ABI.json" ] || fail "Tahoe ABI audit produced no JSON"
            ;;
    esac
else
    printf '>>> Release exact shared-cache build: skipping generic IOUSBHost ABI gate\n'
    "$PYTHON" "${ROOT}/Tests/test_release_entrypoint_isolation.py" || \
        fail "Release Lenovo legacy source logic changed or HP module integration invalid"
fi

# ---------------------------------------------------------------------------
# 3. Host-side generic/legacy regression suite
# ---------------------------------------------------------------------------
printf '>>> Running complete host regression suite\n'
"${ROOT}/Tests/run_host_tests.sh" || fail "host regression tests failed"
printf '>>> Running ASan/UBSan generic suite\n'
"${ROOT}/Tests/run_sanitizers.sh" || fail "sanitizer suite failed"
printf '>>> Running bounded libFuzzer smoke\n'
"${ROOT}/Tests/run_fuzz_smoke.sh" || fail "fuzz smoke failed"
"$PYTHON" "${ROOT}/Tests/validate_fixtures.py" || fail "fixture manifest validation failed"

# ---------------------------------------------------------------------------
# 4. Source / plist / project regression validation
# ---------------------------------------------------------------------------
printf '>>> Running 0.3.2 source validator\n'
"${ROOT}/Tests/validate_source.sh" || fail "source validation failed"
"$PYTHON" "${ROOT}/Tests/validate_release_zip.py" --self-test || fail "ZIP validator self-test failed"
/usr/bin/plutil -lint "${ROOT}/UVCProbeFix/Info.plist" >/dev/null || fail "source Info.plist is invalid"
/usr/bin/plutil -lint "${ROOT}/OpenCore-Kernel-Add-entry.plist" >/dev/null || fail "OpenCore entry is invalid"

# ---------------------------------------------------------------------------
# 5. Xcode static analysis + build
# ---------------------------------------------------------------------------
printf '>>> Running Xcode %s static analysis\n' "$CONFIGURATION"
(
    cd "$ROOT"
    /usr/bin/xcodebuild -project UVCProbeFix.xcodeproj -target UVCProbeFix \
        -configuration "$CONFIGURATION" -arch x86_64 \
        CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO \
        SYMROOT="${BUILD}/products" OBJROOT="${BUILD}/obj-analyze" analyze
) || fail "xcodebuild analyze failed"

printf '>>> Building %s\n' "$CONFIGURATION"
(
    cd "$ROOT"
    /usr/bin/xcodebuild -project UVCProbeFix.xcodeproj -target UVCProbeFix \
        -configuration "$CONFIGURATION" -arch x86_64 \
        CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO \
        SYMROOT="${BUILD}/products" OBJROOT="${BUILD}/obj" build
) || fail "xcodebuild build failed"

KEXT_COUNT="$(/usr/bin/find "${BUILD}/products" -type d -name UVCProbeFix.kext -print | /usr/bin/awk 'END { print NR + 0 }')"
[ "$KEXT_COUNT" -eq 1 ] || fail "expected exactly one built UVCProbeFix.kext, found $KEXT_COUNT"
KEXT="$(/usr/bin/find "${BUILD}/products" -type d -name UVCProbeFix.kext -print | /usr/bin/head -n 1)"
[ -n "$KEXT" ] || fail "xcodebuild succeeded but UVCProbeFix.kext was not found"
[ -f "${KEXT}/Contents/Info.plist" ] || fail "built kext has no Info.plist"
[ -s "${KEXT}/Contents/MacOS/UVCProbeFix" ] || fail "built kext executable missing or empty"
BUILT_EXEC_COUNT="$(/usr/bin/find "${KEXT}/Contents/MacOS" -type f -print | /usr/bin/awk 'END { print NR + 0 }')"
[ "$BUILT_EXEC_COUNT" -eq 1 ] || fail "built kext must contain exactly one executable file, found $BUILT_EXEC_COUNT"
/usr/bin/plutil -lint "${KEXT}/Contents/Info.plist" >/dev/null || fail "built Info.plist is invalid"

plist_raw() {
    /usr/bin/plutil -extract "$1" raw -o - "$2" 2>/dev/null
}
BUILT_PLIST="${KEXT}/Contents/Info.plist"
[ "$(plist_raw CFBundleIdentifier "$BUILT_PLIST")" = "com.dnmtechlabs.UVCProbeFix" ] || fail "built bundle identifier mismatch"
[ "$(plist_raw CFBundleShortVersionString "$BUILT_PLIST")" = "$VERSION" ] || fail "built short version mismatch"
[ "$(plist_raw CFBundleVersion "$BUILT_PLIST")" = "$BUILD_NUMBER" ] || fail "built bundle version/build mismatch"
[ "$(plist_raw CFBundleExecutable "$BUILT_PLIST")" = "UVCProbeFix" ] || fail "built executable name mismatch"
[ "$(plist_raw IOKitPersonalities.UVCProbeFix.IOClass "$BUILT_PLIST")" = "UVCProbeFix" ] || fail "built Lilu retention personality missing"
[ "$(plist_raw IOKitPersonalities.UVCProbeFix.IOProviderClass "$BUILT_PLIST")" = "IOResources" ] || fail "built retention provider mismatch"
[ "$(plist_raw IOKitPersonalities.UVCProbeFix.IOResourceMatch "$BUILT_PLIST")" = "IOKit" ] || fail "built retention resource match mismatch"
[ "$(plist_raw IOKitPersonalities.UVCProbeFix-174F1176-Control.idVendor "$BUILT_PLIST")" = "5967" ] || fail "built control VID scope mismatch"
[ "$(plist_raw IOKitPersonalities.UVCProbeFix-174F1176-Streaming.idProduct "$BUILT_PLIST")" = "4470" ] || fail "built streaming PID scope mismatch"

BUILT_BIN="${KEXT}/Contents/MacOS/UVCProbeFix"
BUILT_ARCH="$(/usr/bin/file "$BUILT_BIN")"
printf '%s\n' "$BUILT_ARCH" | /usr/bin/grep -q 'x86_64' || fail "built executable is not x86_64"

# Validate the binary header numerically. This avoids relying on the human
# presentation used by `otool -hv` (which prints KEXT_BUNDLE, not the C macro
# name MH_KEXT_BUNDLE, on current Xcode/macOS toolchains).
MACHO_AUDIT="$("$PYTHON" "${ROOT}/Tests/validate_macho.py" "$BUILT_BIN")" || fail "built Mach-O header validation failed"
printf '%s\n' "$MACHO_AUDIT"
NM_GLOBAL="$(/usr/bin/nm -g "${KEXT}/Contents/MacOS/UVCProbeFix" 2>/dev/null || true)"
printf '%s\n' "$NM_GLOBAL" | /usr/bin/grep -q '_UVCProbeFix_kern_start' || fail "built kmod start symbol missing"
printf '%s\n' "$NM_GLOBAL" | /usr/bin/grep -q '_UVCProbeFix_kern_stop' || fail "built kmod stop symbol missing"

NM_UNDEFINED="$(/usr/bin/nm -u "${KEXT}/Contents/MacOS/UVCProbeFix" 2>/dev/null || true)"
printf '%s\n' "$NM_UNDEFINED" | /usr/bin/grep -q '_cs_validate_page' && fail "plugin unexpectedly imports _cs_validate_page directly instead of routing by symbol"
if printf '%s\n' "$NM_UNDEFINED" | /usr/bin/grep -Ei 'IOUSBHostFamily|deviceRequest'; then
    fail "built executable unexpectedly imports old USB request-hook symbols"
fi

# Release must preserve the known-good 0.2.15 load-time footprint: generic
# IOUSBHost transport code is not linked into the production kext until the
# physical generic hardware gates pass. Debug/Test must still contain it.
# Both Debug and Release MUST contain the HP module within THIS binary.
HP_ROUTE_SYMBOL='__ZN15IOUSBHostDevice21internalDeviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj'
LC_ALL=C /usr/bin/grep -aFq "$HP_ROUTE_SYMBOL" "$BUILT_BIN" || fail "UVCProbeFix executable is missing integrated HP UVC 1.5 module"
GENERIC_ROUTE_SYMBOL='__ZN18IOUSBHostInterface13deviceRequestERN11StandardUSB13DeviceRequestEPvRjj'
if [ "$CONFIGURATION" = "Release" ]; then
    if LC_ALL=C /usr/bin/grep -aFq "$GENERIC_ROUTE_SYMBOL" "$BUILT_BIN"; then
        fail "Release executable unexpectedly contains generic IOUSBHost deviceRequest route"
    fi
    LC_ALL=C /usr/bin/grep -aFq '_cs_validate_page' "$BUILT_BIN" || fail "Release executable is missing legacy shared-cache route selector"
else
    LC_ALL=C /usr/bin/grep -aFq "$GENERIC_ROUTE_SYMBOL" "$BUILT_BIN" || fail "Debug/Test executable is missing generic IOUSBHost deviceRequest route"
fi
if printf '%s\n' "$NM_UNDEFINED" | /usr/bin/grep -E '__cxa_|_Unwind_|__gxx_personality'; then
    fail "built executable unexpectedly imports C++ exception/runtime-unwind symbols"
fi

# ---------------------------------------------------------------------------
# 6. Ad-hoc code signing (requested release invariant)
# ---------------------------------------------------------------------------
printf '>>> Applying ad-hoc signatures to executable and kext bundle\n'
/usr/bin/codesign --force --sign - --timestamp=none "$BUILT_BIN" || fail "failed to ad-hoc sign kext executable"
/usr/bin/codesign --force --sign - --timestamp=none "$KEXT" || fail "failed to ad-hoc sign kext bundle"
/usr/bin/codesign --verify --strict --verbose=2 "$BUILT_BIN" || fail "ad-hoc executable signature verification failed"
/usr/bin/codesign --verify --strict --verbose=2 "$KEXT" || fail "ad-hoc kext signature verification failed"
EXEC_SIGNATURE_INFO="$(/usr/bin/codesign -dv --verbose=4 "$BUILT_BIN" 2>&1 || true)"
printf '%s\n' "$EXEC_SIGNATURE_INFO" | /usr/bin/grep -q 'Signature=adhoc' || fail "kext executable signature is not ad-hoc"
SIGNATURE_INFO="$(/usr/bin/codesign -dv --verbose=4 "$KEXT" 2>&1 || true)"
printf '%s\n' "$SIGNATURE_INFO" | /usr/bin/grep -q 'Signature=adhoc' || fail "kext signature is not ad-hoc"

# ---------------------------------------------------------------------------
# 7. Package selected configuration and verify extraction
# ---------------------------------------------------------------------------
printf '>>> Packaging %s\n' "$CONFIGURATION"
/usr/bin/ditto "$KEXT" "${DIST}/UVCProbeFix.kext" || fail "failed to copy built kext"
if [ "$CONFIGURATION" = "Release" ]; then
    PACKAGE_TAG="RELEASE"
else
    PACKAGE_TAG="TEST-DEBUG"
fi
RELEASE_ZIP="${DIST}/UVCProbeFix-${VERSION}-${PACKAGE_TAG}.zip"
(
    cd "$DIST"
    /usr/bin/zip -qry "$(basename "$RELEASE_ZIP")" UVCProbeFix.kext
) || fail "failed to create Release ZIP"
/usr/bin/unzip -tq "$RELEASE_ZIP" >/dev/null || fail "Release ZIP integrity test failed"
"$PYTHON" "${ROOT}/Tests/validate_release_zip.py" "$RELEASE_ZIP" "$VERSION" "$BUILD_NUMBER" || fail "Release ZIP structural audit failed"
VERIFY_DIR="${BUILD}/archive-verify"
/bin/mkdir -p "$VERIFY_DIR"
/usr/bin/ditto -x -k "$RELEASE_ZIP" "$VERIFY_DIR" || fail "Release ZIP extraction failed"
[ -s "${VERIFY_DIR}/UVCProbeFix.kext/Contents/MacOS/UVCProbeFix" ] || fail "verified archive executable missing"
/usr/bin/cmp -s "${DIST}/UVCProbeFix.kext/Contents/MacOS/UVCProbeFix" \
    "${VERIFY_DIR}/UVCProbeFix.kext/Contents/MacOS/UVCProbeFix" || fail "archive executable differs from built product"
/usr/bin/cmp -s "${DIST}/UVCProbeFix.kext/Contents/Info.plist" \
    "${VERIFY_DIR}/UVCProbeFix.kext/Contents/Info.plist" || fail "archive Info.plist differs from built product"
/usr/bin/diff -qr "${DIST}/UVCProbeFix.kext" "${VERIFY_DIR}/UVCProbeFix.kext" >/dev/null || fail "verified archive kext differs from built product"
/usr/bin/codesign --verify --strict --verbose=2 "${VERIFY_DIR}/UVCProbeFix.kext/Contents/MacOS/UVCProbeFix" || fail "archived executable signature verification failed"
/usr/bin/codesign --verify --strict --verbose=2 "${VERIFY_DIR}/UVCProbeFix.kext" || fail "archived kext signature verification failed"

ZIP_SHA="$(/usr/bin/shasum -a 256 "$RELEASE_ZIP" | /usr/bin/awk '{print $1}')"
printf '\n============================================================\n'
printf ' BUILD/AUDIT PASS\n'
printf ' Kext:   %s\n' "${DIST}/UVCProbeFix.kext"
printf ' ZIP:    %s\n' "$RELEASE_ZIP"
printf ' SHA256: %s\n' "$ZIP_SHA"
printf ' No installation, EFI, NVRAM or macOS system change was performed.\n'
printf '============================================================\n'

# Stop tee before returning so the build log cannot change after PASS.
exec 1>&3 2>&4
exec 3>&- 4>&-
if [ -n "${TEE_PID:-}" ]; then
    wait "$TEE_PID" 2>/dev/null || true
    TEE_PID=""
fi
pause_if_interactive
