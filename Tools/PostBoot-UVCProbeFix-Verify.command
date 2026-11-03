#!/bin/bash
# Read-only post-boot verification for UVCProbeFix.
set -u
PATH="/usr/bin:/bin:/usr/sbin:/sbin"
export PATH
BUNDLE_ID="com.dnmtechlabs.UVCProbeFix"
FOUND=0
SOURCE="none"
LINE=""

if [ -x /usr/bin/kmutil ]; then
    LINE="$(/usr/bin/kmutil showloaded 2>/dev/null | /usr/bin/grep -F "$BUNDLE_ID" | /usr/bin/head -n 1 || true)"
    if [ -n "$LINE" ]; then FOUND=1; SOURCE="kmutil"; fi
fi
if [ "$FOUND" -eq 0 ] && [ -x /usr/sbin/kextstat ]; then
    LINE="$(/usr/sbin/kextstat 2>/dev/null | /usr/bin/grep -F "$BUNDLE_ID" | /usr/bin/head -n 1 || true)"
    if [ -n "$LINE" ]; then FOUND=1; SOURCE="kextstat"; fi
fi

printf 'UVCProbeFixDetected=%s\n' "$FOUND"
printf 'DetectionSource=%s\n' "$SOURCE"
printf 'BundleID=%s\n' "$BUNDLE_ID"
printf 'Darwin=%s\n' "$(/usr/bin/uname -r)"
printf 'Architecture=%s\n' "$(/usr/bin/uname -m)"
printf 'BootArgs='
/usr/sbin/nvram boot-args 2>/dev/null | /usr/bin/sed 's/^boot-args[[:space:]]*//' || true
if [ -n "$LINE" ]; then printf 'LoadedLine=%s\n' "$LINE"; fi

printf '\nRecent UVCProbeFix/Lilu boot log lines:\n'
if [ -x /usr/bin/log ]; then
    /usr/bin/log show --last boot --style compact \
        --predicate 'eventMessage CONTAINS[c] "UVCProbeFix" OR eventMessage CONTAINS[c] "uvcp" OR eventMessage CONTAINS[c] "Lilu"' \
        2>/dev/null | /usr/bin/tail -n 200 || true
fi

if [ "$FOUND" -eq 1 ]; then
    printf '\nRESULT: LOADED\n'
    exit 0
fi
printf '\nRESULT: NOT LOADED\n'
exit 2
