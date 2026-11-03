# UVCProbeFix — HP UVC 1.5 addition to the SAME kext

Single `UVCProbeFix.kext`, identifier `com.dnmtechlabs.UVCProbeFix`, one Lilu `ADDPR(config)` and the existing OpenCore entry. The Lenovo exact 0.2.15 recovery body is preserved verbatim; two additive call points invoke `UVC15::start()` in Release and Debug. The original generic engine remains Debug/Test-only. This does not contain a second kext.

## Behavior

For Chicony `04F2:B6BF` (HP EliteBook 850 G7): synchronously intercept ONLY class/interface `GET_CUR` (`0xA1/0x81`) `VS_PROBE`/`VS_COMMIT` (`wValue=0x0100/0x0200`) on interfaces 1 or 3 when `wLength=34`, with raw buffer and no memory descriptor. Submit a single 48-byte request into a distinct 48-byte kernel buffer; copy 34 bytes into the original 34-byte buffer only if 48 bytes were actually received. Original `SET_CUR` and unrelated USB transactions are passed through.

A missing symbol, incompatible build, or unknown/already-routed prologue disables HP recovery (fail closed). `-uvc15off` disables only the HP module; `-uvcpoff` disables the entire plugin. The HP handler does not modify the Lenovo shared-cache patch and has no generic camera allowlist.

## Tests and limitations

Host-side tests check request isolation, 34/48 length logic and byte-for-byte preservation of the original Lenovo entrypoint after subtracting the HP additions. A valid kernel binary cannot be produced on Linux: run the original `Build.command` on macOS with Xcode/Lilu 1.7.2/MacKernelSDK, verify its target ABI and inspect the startup log before a hardware test. The published success applies to deeveedee's own UVC15ProbeFix, not to this newly integrated source. No kernel/hardware validation of this adaptation is claimed.

Sources: InsanelyMac topic comment #2855079, macOS 25G241 report; pinned MacKernelSDK IOUSBHostDevice.h declaration.
