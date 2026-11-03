#!/bin/bash
# UVCProbeFix 0.2.15 Camera Black Screen Diagnostics
# Copyright © 2026 DNMTechLabs
# dnmtechlabs@gmail.com
#
# Purpose: collect read-only diagnostics for UVC/camera black-screen issues.
# This script does not change NVRAM, kexts, OpenCore, system files, or camera settings.

set -u
umask 077
PATH="/usr/bin:/bin:/usr/sbin:/sbin"
export PATH

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STAMP="$(date '+%Y%m%d-%H%M%S')"
RUN_ID="${STAMP}-$$"
OUT_DIR="$SCRIPT_DIR/UVCProbeFix-Diagnostics-$RUN_ID"
ZIP_PATH="$SCRIPT_DIR/UVCProbeFix-Diagnostics-$RUN_ID.zip"
DURATION="${1:-90}"
MAX_LOG_BYTES=67108864

# Clamp capture duration to a safe/readable range.
case "$DURATION" in
    ''|*[!0-9]*) DURATION=90 ;;
esac
if [ "$DURATION" -lt 15 ]; then DURATION=15; fi
if [ "$DURATION" -gt 600 ]; then DURATION=600; fi

if [ "$(uname -s 2>/dev/null || true)" != "Darwin" ]; then
    echo "ERROR: this diagnostic command must be run on macOS."
    exit 1
fi

if ! mkdir "$OUT_DIR"; then
    echo "ERROR: output directory already exists or cannot be created: $OUT_DIR"
    exit 1
fi

MASTER_LOG="$OUT_DIR/diagnostics.log"
LIVE_LOG="$OUT_DIR/live-camera-usb.log"
STATIC_LOG="$OUT_DIR/recent-camera-usb.log"
WINDOW_LOG="$OUT_DIR/capture-window-camera-usb.log"
IOREG_LOG="$OUT_DIR/ioreg-usb.txt"
TARGET_IOREG_LOG="$OUT_DIR/ioreg-target-interfaces.txt"
SYSTEM_LOG="$OUT_DIR/system-hardware.txt"

# Preserve original console descriptors so the logger can be closed deterministically
# before the payload is archived. This avoids packaging a log while it is still changing.
exec 3>&1 4>&2
exec > >(/usr/bin/tee -a "$MASTER_LOG") 2>&1
TEE_PID="${!:-}"

echo "============================================================"
echo "UVCProbeFix Camera Black Screen Diagnostics"
echo "Copyright © 2026 DNMTechLabs"
echo "dnmtechlabs@gmail.com"
echo "============================================================"
echo "Started: $(date)"
echo "Output:  $OUT_DIR"
echo "Live capture duration: ${DURATION}s"
echo

echo "This is a READ-ONLY diagnostic collector."
echo "It does not modify OpenCore, boot-args, NVRAM, kexts or macOS."
echo "Note: hardware logs may contain model/serial identifiers."
echo

run_section() {
    local title="$1"
    shift
    echo
    echo "-------------------- $title --------------------"
    "$@" 2>&1 || echo "[command returned non-zero: $*]"
}

run_shell_section() {
    local title="$1"
    local command="$2"
    echo
    echo "-------------------- $title --------------------"
    /bin/bash -c "$command" 2>&1 || echo "[command returned non-zero: $command]"
}

# Basic OS / boot context.
run_section "DATE" /bin/date
run_section "SW_VERS" /usr/bin/sw_vers
run_section "UNAME" /usr/bin/uname -a
run_shell_section "HARDWARE MODEL" '/usr/sbin/sysctl -n hw.model 2>/dev/null; /usr/sbin/sysctl -n hw.machine 2>/dev/null; /usr/sbin/sysctl -n kern.osrelease 2>/dev/null; /usr/sbin/sysctl -n kern.osversion 2>/dev/null'

if command -v csrutil >/dev/null 2>&1; then
    run_section "SIP STATUS" csrutil status
fi

if command -v spctl >/dev/null 2>&1; then
    run_section "GATEKEEPER STATUS" spctl --status
fi

if command -v nvram >/dev/null 2>&1; then
    run_shell_section "BOOT ARGS" 'nvram -p 2>/dev/null | grep -E "^boot-args[[:space:]]" || echo "boot-args not readable or empty"'

    BOOT_ARGS_LINE="$(nvram -p 2>/dev/null | grep -E '^boot-args[[:space:]]' || true)"
    echo
    echo "-------------------- UVCPROBEFIX 0.2.x BOOT-ARG CHECK --------------------"
    case " $BOOT_ARGS_LINE " in
        *" -uvcpon "*)
            echo "WARN: obsolete -uvcpon detected. UVCProbeFix 0.2.15 never uses the old USB request hook; remove -uvcpon."
            ;;
        *)
            echo "PASS: obsolete -uvcpon is absent."
            ;;
    esac
    echo "NOTE: -uvcpoff, -uvcpdbg and -uvcpbeta are valid Lilu-plugin controls in 0.2.15."
fi

# Loaded kexts / Lilu / UVCProbeFix state.
if command -v kmutil >/dev/null 2>&1; then
    run_shell_section "KMUTIL LOADED - RELEVANT" 'kmutil showloaded 2>&1 | grep -Ei "UVCProbeFix|Lilu|IOUSBHost|USBVideo|camera" || true'
    run_shell_section "KMUTIL LOADED - FULL" 'kmutil showloaded 2>&1 || true'
fi

if command -v kextstat >/dev/null 2>&1; then
    run_shell_section "KEXTSTAT - RELEVANT" 'kextstat 2>&1 | grep -Ei "UVCProbeFix|Lilu|IOUSBHost|USBVideo|camera" || true'
fi

# Camera and USB inventory.
echo
 echo "-------------------- SYSTEM PROFILER --------------------" | /usr/bin/tee -a "$SYSTEM_LOG"
/usr/sbin/system_profiler -detailLevel full -timeout 45 SPCameraDataType SPUSBDataType >> "$SYSTEM_LOG" 2>&1 || true
echo "Saved full camera/USB system_profiler output to: $SYSTEM_LOG"
/usr/bin/grep -Ei -B 3 -A 10 'Integrated Camera|Sonix|0x174f|0x1176' "$SYSTEM_LOG" | head -n 240 || true

# IORegistry snapshots. These are intentionally written to a dedicated file.
echo "IORegistry USB snapshot - $(date)" > "$IOREG_LOG"
echo >> "$IOREG_LOG"
echo "===== IOUSB plane =====" >> "$IOREG_LOG"
/usr/sbin/ioreg -p IOUSB -l -w 0 >> "$IOREG_LOG" 2>&1 || true

echo >> "$IOREG_LOG"
echo "===== IOUSBHostDevice =====" >> "$IOREG_LOG"
/usr/sbin/ioreg -r -c IOUSBHostDevice -l -w 0 >> "$IOREG_LOG" 2>&1 || true

echo >> "$IOREG_LOG"
echo "===== IOUSBHostInterface =====" >> "$IOREG_LOG"
/usr/sbin/ioreg -r -c IOUSBHostInterface -l -w 0 >> "$IOREG_LOG" 2>&1 || true

echo >> "$IOREG_LOG"
echo "===== Target summary is derived from the bounded USB/class snapshots above =====" >> "$IOREG_LOG"

# Dedicated target-only snapshot used for validation.  Do not infer PASS from a
# global grep: another USB camera could legitimately expose the same UVC key/GUID.
/usr/sbin/ioreg -r -c IOUSBHostInterface -l -w 0 > "$TARGET_IOREG_LOG" 2>&1 || true

check_target_interface() {
    local interface_number="$1"
    local interface_subclass="$2"
    /usr/bin/awk -v wanted="$interface_number" -v wanted_subclass="$interface_subclass" '
        function reset() { vendor=product=config=class=iface=subclass=camera=hidecontrols=hideformats=0 }
        function finish() {
            exact = vendor && product && config && class && iface && subclass && camera && hidecontrols
            if (exact && hideformats) bad=1
            if (exact && !hideformats) found=1
        }
        /^[[:space:]]*\+-o / { finish(); reset() }
        /"idVendor"[[:space:]]*=[[:space:]]*5967/ { vendor=1 }
        /"idProduct"[[:space:]]*=[[:space:]]*4470/ { product=1 }
        /"bConfigurationValue"[[:space:]]*=[[:space:]]*1([^0-9]|$)/ { config=1 }
        /"bInterfaceClass"[[:space:]]*=[[:space:]]*14([^0-9]|$)/ { class=1 }
        $0 ~ "\"bInterfaceNumber\"[[:space:]]*=[[:space:]]*" wanted "([^0-9]|$)" { iface=1 }
        $0 ~ "\"bInterfaceSubClass\"[[:space:]]*=[[:space:]]*" wanted_subclass "([^0-9]|$)" { subclass=1 }
        /"CameraAssistantBundleID"[[:space:]]*=[[:space:]]*"com\.apple\.cmio\.uvcassistantextension"/ { camera=1 }
        /"UVCControlEntityType"[[:space:]]*=[[:space:]]*3/ { hidecontrols=1 }
        /"UVCCameraHideFormats"/ { hideformats=1 }
        END { finish(); exit(found && !bad ? 0 : 1) }
    ' "$TARGET_IOREG_LOG"
}

target_interface_has_hideformats() {
    local interface_number="$1"
    local interface_subclass="$2"
    /usr/bin/awk -v wanted="$interface_number" -v wanted_subclass="$interface_subclass" '
        function reset() { vendor=product=config=class=iface=subclass=hideformats=0 }
        function finish() {
            if (vendor && product && config && class && iface && subclass && hideformats) found=1
        }
        /^[[:space:]]*\+-o / { finish(); reset() }
        /"idVendor"[[:space:]]*=[[:space:]]*5967/ { vendor=1 }
        /"idProduct"[[:space:]]*=[[:space:]]*4470/ { product=1 }
        /"bConfigurationValue"[[:space:]]*=[[:space:]]*1([^0-9]|$)/ { config=1 }
        /"bInterfaceClass"[[:space:]]*=[[:space:]]*14([^0-9]|$)/ { class=1 }
        $0 ~ "\"bInterfaceNumber\"[[:space:]]*=[[:space:]]*" wanted "([^0-9]|$)" { iface=1 }
        $0 ~ "\"bInterfaceSubClass\"[[:space:]]*=[[:space:]]*" wanted_subclass "([^0-9]|$)" { subclass=1 }
        /"UVCCameraHideFormats"/ { hideformats=1 }
        END { finish(); exit(found ? 0 : 1) }
    ' "$TARGET_IOREG_LOG"
}

echo
echo "-------------------- UVCPROBEFIX 0.2.15 PROPERTY-MERGE CHECK --------------------"
if check_target_interface 0 1 && check_target_interface 1 2; then
    echo "PASS: target 174F:1176 interfaces 0 and 1 contain the 0.2.15 preserved control-only property merge and no UVCCameraHideFormats key."
else
    echo "NOTE: the expected UVCProbeFix 0.2.15 preserved control-only property merge was not proven on both 174F:1176 interfaces, or UVCCameraHideFormats is still present."
fi

echo
 echo "-------------------- IOREG RELEVANT SUMMARY --------------------"
/usr/bin/grep -Ei -B 4 -A 8 '174f|1176|UVCProbeFix|USB Video|IOUSBHostInterface' "$IOREG_LOG" | head -n 800 || true

# Current camera-related processes.
run_shell_section "CAMERA PROCESSES" 'ps -axo pid,ppid,user,%cpu,%mem,etime,command | grep -Ei "VDCAssistant|camera|avconferenced|CMIO|UVCAssistant" | grep -v grep || true'

# Recent diagnostic reports relevant to camera/kernel/USB.
echo
 echo "-------------------- RECENT DIAGNOSTIC REPORTS --------------------"
for diagdir in "$HOME/Library/Logs/DiagnosticReports" "/Library/Logs/DiagnosticReports"; do
    if [ -d "$diagdir" ]; then
        echo "Directory: $diagdir"
        /usr/bin/find "$diagdir" -maxdepth 1 -type f -mmin -120 \( \
            -iname '*camera*' -o -iname '*VDCAssistant*' -o -iname '*USB*' -o \
            -iname '*kernel*' -o -iname '*panic*' -o -iname '*CMIO*' \
        \) -print 2>/dev/null | sort || true
    fi
done

# Unified log predicate kept deliberately focused to avoid gigantic captures.
PREDICATE='eventMessage CONTAINS[c] "UVCProbeFix" OR eventMessage CONTAINS[c] "uvcp" OR eventMessage CONTAINS[c] "IOUSBHost" OR eventMessage CONTAINS[c] "AppleUSB" OR eventMessage CONTAINS[c] "USB Video" OR eventMessage CONTAINS[c] "UVC" OR eventMessage CONTAINS[c] "camera" OR eventMessage CONTAINS[c] "CMIO" OR process CONTAINS[c] "VDCAssistant" OR process CONTAINS[c] "camera" OR subsystem CONTAINS[c] "camera" OR subsystem CONTAINS[c] "cmio"'

# Keep every diagnostic log bounded while continuing to drain the source stream.
# Once the budget is exhausted awk discards later lines instead of allowing the
# capture to grow without limit. The initial negotiation remains preserved.
bounded_log_awk='
BEGIN { bytes=0; truncated=0 }
{
    n=length($0)+1
    if (bytes+n <= max) {
        print
        bytes += n
    } else if (!truncated) {
        print "[capture truncated after bounded diagnostic log budget]"
        truncated=1
    }
}'

# Capture recent history before reproducing the issue.
echo
 echo "-------------------- RECENT UNIFIED LOG --------------------"
/usr/bin/log show --last 20m --style compact --info --debug --predicate "$PREDICATE" 2>&1 | LC_ALL=C /usr/bin/awk -v max="$MAX_LOG_BYTES" "$bounded_log_awk" > "$STATIC_LOG" || true
echo "Saved full recent unified log to: $STATIC_LOG"
/usr/bin/grep -Ei 'UVCUSBDeviceStreamingInterface|UVCFamily PROBE recovery applied|shared-cache validator route installed|SET_CUR|GET_CUR|COMMIT|MaxPayloadSize|MaxVideoFrameSize|Start Stream|Start Stream Failed|start stream|stop stream|TotalPackets|TotalFrames|packetDrop|FID_|Stream Alternate Setting|bufferSize|epPacketSize|Setting default format|Filter Format|Unable to set the proper camera format|AVCaptureDeviceFormat|stream.*fail|Invalid stream' "$STATIC_LOG" | tail -n 400 || true

# Live capture while the user reproduces the black screen.
echo
echo "============================================================"
echo "LIVE CAPTURE STARTING"
echo "Open the camera/app now and reproduce the BLACK SCREEN."
echo "Capture will stop automatically after ${DURATION} seconds."
echo "============================================================"
echo

# Record the interval so it can be reconstructed from the persistent unified log.
CAPTURE_START="$(date '+%Y-%m-%d %H:%M:%S')"
/usr/bin/log stream --style compact --level debug --predicate "$PREDICATE" 2>&1 | LC_ALL=C /usr/bin/awk -v max="$MAX_LOG_BYTES" "$bounded_log_awk" > "$LIVE_LOG" &
LOG_PID=$!

# Ensure the background log stream is stopped deterministically on interruption/exit.
# Never wait forever on a diagnostic child process.
cleanup_stream() {
    if [ -n "${LOG_PID:-}" ] && kill -0 "$LOG_PID" >/dev/null 2>&1; then
        kill -INT "$LOG_PID" >/dev/null 2>&1 || true
        /bin/sleep 1
        if kill -0 "$LOG_PID" >/dev/null 2>&1; then
            kill -TERM "$LOG_PID" >/dev/null 2>&1 || true
            /bin/sleep 1
        fi
        if kill -0 "$LOG_PID" >/dev/null 2>&1; then
            kill -KILL "$LOG_PID" >/dev/null 2>&1 || true
        fi
    fi
    if [ -n "${LOG_PID:-}" ]; then
        wait "$LOG_PID" 2>/dev/null || true
        LOG_PID=""
    fi
}
trap cleanup_stream EXIT INT TERM

sleep "$DURATION"
cleanup_stream
trap - EXIT INT TERM
CAPTURE_END="$(date '+%Y-%m-%d %H:%M:%S')"

# Re-read the exact interval after capture. Unlike `log stream`, `log show` does not
# lose lines merely because the terminal stream could not keep up.
/usr/bin/log show --start "$CAPTURE_START" --end "$CAPTURE_END" --style compact --info --debug --predicate "$PREDICATE" 2>&1 | LC_ALL=C /usr/bin/awk -v max="$MAX_LOG_BYTES" "$bounded_log_awk" > "$WINDOW_LOG" || true

echo
echo "-------------------- CAPTURE WINDOW UVC/USB SUMMARY --------------------"
/usr/bin/grep -Ei 'UVCProbeFix|uvcp|UVCUSBDeviceStreamingInterface|UVCFamily PROBE recovery applied|shared-cache validator route installed|SET_CUR|GET_CUR|COMMIT|MaxPayloadSize|MaxVideoFrameSize|Start Stream|Start Stream Failed|start stream|stop stream|TotalPackets|TotalFrames|packetDrop|FID_|Stream Alternate Setting|bufferSize|epPacketSize|Setting default format|Filter Format|Unable to set the proper camera format|AVCaptureDeviceFormat|stream.*fail|Invalid stream|pipe|stall|timeout' "$WINDOW_LOG" | tail -n 1200 || true
if /usr/bin/grep -Fqi 'Unable to set the proper camera format' "$WINDOW_LOG"; then
    echo 'DIAGNOSIS: application format-selection failure was observed during the capture window.'
fi
if /usr/bin/grep -Fqi 'UVCFamily PROBE recovery applied' "$WINDOW_LOG"; then
    echo 'PASS: the 0.2.15 exact known-build PROBE-recovery shared-cache patch was observed at runtime.'
else
    echo 'NOTE: no 0.2.15 patch-application SYSLOG was observed in this capture window. Because it is emitted once when the shared-cache page is first matched, also inspect the recent 20-minute log and loaded-kext sections.'
fi
if /usr/bin/grep -Fqi 'MaxPayloadSize:16842753' "$WINDOW_LOG"; then
    echo 'DIAGNOSIS: an unsanitized 0x01010001 GET_CUR response was logged. On 25G229/25G241, 0.2.15 should remap that exact malformed Frame-4 response to Frame 1 before the one-shot second PROBE; verify patch load/match state.'
fi
if /usr/bin/grep -Fqi 'Stream Alternate Setting 6' "$WINDOW_LOG"; then
    echo 'NOTE: alternate setting 6 was selected. On 25G229/25G241, 0.2.15 is expected to renegotiate Frame 1; inspect PROBE/COMMIT lines to confirm the Frame4->Frame1 fallback actually took effect.'
fi
if /usr/bin/grep -Fqi 'Stream Alternate Setting 5' "$WINDOW_LOG"; then
    echo 'NOTE: alternate setting 5 was selected. 0.2.15 no longer uses ALT selection as its primary recovery criterion on 25G229/25G241; verify the negotiated frame index instead.'
fi
if /usr/bin/grep -Eqi 'packetDropInvalidDataPayloadCount[[:space:]]*=[[:space:]]*[1-9][0-9]*' "$WINDOW_LOG"; then
    echo 'NOTE: non-zero packetDropInvalidDataPayloadCount alone does not prove header-only/zero-length image payloads; the known-good ASUS baseline also records non-zero invalid-payload drops. Correlate with TotalFrames, stream state, and actual image output.'
fi
if /usr/bin/grep -Eqi 'TotalPackets:[[:space:]]*[1-9][0-9]*' "$WINDOW_LOG" && /usr/bin/grep -Eqi 'TotalFrames[[:space:]]*:[[:space:]]*0([^0-9]|$)' "$WINDOW_LOG"; then
    echo 'DIAGNOSIS: packets arrived but no frame was dispatched.'
fi
if /usr/bin/grep -Fqi 'Invalid stream in pipe' "$WINDOW_LOG"; then
    echo 'DIAGNOSIS: UVCFamily still rejected the negotiated stream pipe during this capture window.'
elif /usr/bin/grep -Eqi 'Start Stream format|Start Stream[^F]' "$WINDOW_LOG"; then
    echo 'PASS: a stream-start event was observed without "Invalid stream in pipe" in the authoritative capture window.'
fi
if /usr/bin/grep -Fqi 'Start Stream Failed' "$WINDOW_LOG"; then
    echo 'DIAGNOSIS: UVCFamily reported Start Stream Failed during this capture window.'
fi
if /usr/bin/grep -Eqi 'TotalPackets:[[:space:]]*[1-9][0-9]*|TotalFrames[[:space:]]*:[[:space:]]*[1-9][0-9]*' "$WINDOW_LOG"; then
    echo 'PASS: non-zero UVC packet/frame counters were observed.'
fi
if target_interface_has_hideformats 0 1 || target_interface_has_hideformats 1 2; then
    echo 'DIAGNOSIS: UVCCameraHideFormats is still present on the exact 174F:1176 UVC interfaces; an older 0.2.2-style property merge may still be active.'
fi

echo
 echo "-------------------- LIVE LOG TAIL --------------------"
tail -n 500 "$LIVE_LOG" 2>/dev/null || true

# Snapshot state again after reproduction.
run_shell_section "POST-CAPTURE CAMERA PROCESSES" 'ps -axo pid,ppid,user,%cpu,%mem,etime,command | grep -Ei "VDCAssistant|camera|avconferenced|CMIO|UVCAssistant" | grep -v grep || true'

if command -v kmutil >/dev/null 2>&1; then
    run_shell_section "POST-CAPTURE KMUTIL RELEVANT" 'kmutil showloaded 2>&1 | grep -Ei "UVCProbeFix|Lilu|IOUSBHost|USBVideo|camera" || true'
fi

# Small metadata file for quick triage.
cat > "$OUT_DIR/README.txt" <<META
UVCProbeFix Camera Black Screen Diagnostics
Copyright © 2026 DNMTechLabs
dnmtechlabs@gmail.com

Created: $(date)
Live capture duration: ${DURATION}s

Files:
- diagnostics.log          Main command/output log
- recent-camera-usb.log    Unified logs from the previous 20 minutes
- live-camera-usb.log      Unified live stream while reproducing black screen
- capture-window-camera-usb.log  Reconstructed exact capture interval (authoritative)
- ioreg-usb.txt            USB / IOUSBHostDevice / IOUSBHostInterface registry dump
- ioreg-target-interfaces.txt Dedicated IOUSBHostInterface snapshot used for target-only quirk validation
- system-hardware.txt      system_profiler camera and USB inventory

This collector is read-only and does not modify system configuration.
META

# Finish and reap the tee process before archiving so diagnostics.log is stable.
echo
echo "Finalizing diagnostics log..."
exec 1>&3 2>&4
exec 3>&- 4>&-
if [ -n "${TEE_PID:-}" ]; then
    wait "$TEE_PID" 2>/dev/null || true
    TEE_PID=""
fi

# Package everything into one ZIP beside this .command.
echo
echo "Packaging diagnostics..."
if [ -x /usr/bin/ditto ]; then
    /usr/bin/ditto -c -k --sequesterRsrc --keepParent "$OUT_DIR" "$ZIP_PATH"
elif command -v zip >/dev/null 2>&1; then
    (cd "$SCRIPT_DIR" && zip -qry "$(basename "$ZIP_PATH")" "$(basename "$OUT_DIR")")
else
    echo "ERROR: neither ditto nor zip is available. Logs remain in: $OUT_DIR"
    exit 1
fi

if [ ! -s "$ZIP_PATH" ]; then
    echo "ERROR: diagnostics ZIP was not created correctly."
    exit 1
fi

if [ -x /usr/bin/unzip ]; then
    /usr/bin/unzip -tq "$ZIP_PATH" >/dev/null 2>&1 || {
        echo "ERROR: diagnostics ZIP integrity verification failed."
        exit 1
    }
fi
ZIP_SHA="$(/usr/bin/shasum -a 256 "$ZIP_PATH" | /usr/bin/awk '{print $1}')"

echo
echo "============================================================"
echo "DIAGNOSTICS COMPLETE"
echo "ZIP: $ZIP_PATH"
echo "SHA-256: $ZIP_SHA"
echo "============================================================"

# Reveal the ZIP in Finder when possible.
/usr/bin/open -R "$ZIP_PATH" >/dev/null 2>&1 || true

exit 0
