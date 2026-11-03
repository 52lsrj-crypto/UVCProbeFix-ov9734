# UVCProbeFix

[![Build](https://github.com/danielnmaldonado/UVCProbeFix/actions/workflows/ci.yml/badge.svg)](https://github.com/danielnmaldonado/UVCProbeFix/actions/workflows/ci.yml)

**Laptop UVC camera black screen fix for macOS.**

A Lilu plugin that repairs malformed UVC negotiation responses which prevent supported laptop cameras from starting video. Includes the Sonix `174F:1176` recovery and the existing targeted HP/Chicony `04F2:B6BF` workaround.

## Compatibility

Intel/x86_64 only. Sonix recovery requires an exact supported binary fingerprint.

| macOS | Target | Status |
| --- | --- | --- |
| Sequoia 15.0 | `24A335` / Darwin `24.0` | Adapted from the Tahoe fix; physical camera testing pending |
| Tahoe | `25G83`, `25G229`, `25G241` / Darwin `25.6` | Existing working Sonix recovery preserved |

The HP workaround remains limited to Tahoe/Darwin `25.6`. Other builds and cameras are not assumed compatible.

## Install

1. Install [Lilu](https://github.com/acidanthera/Lilu) `1.7.2` or newer.
2. Copy `UVCProbeFix.kext` to `EFI/OC/Kexts` and add it after Lilu in OpenCore `Kernel -> Add`.
3. Use `Arch=x86_64`, `MinKernel=24.0.0`, `MaxKernel=25.6.0`, and `Enabled=true`. An example entry is included in [OpenCore-Kernel-Add-entry.plist](OpenCore-Kernel-Add-entry.plist).
4. Reboot and test the camera. Run `Tools/PostBoot-UVCProbeFix-Verify.command` to inspect loading and patch logs.

Keep a backup of the previous kext. The boot argument `-uvcpoff` disables the plugin.

## Build

On an Intel Mac with full Xcode selected:

```sh
./Build.command
```

The builder verifies pinned dependencies, runs tests and static analysis, and creates an ad-hoc signed Release ZIP in `dist/`. It does not install the kext.

[GitHub Actions](https://github.com/danielnmaldonado/UVCProbeFix/actions) builds and validates each change. Version tags publish release packages. Automated checks do not replace physical camera testing.

## License

[BSD 3-Clause](LICENSE). Copyright © 2026 DNMTechLabs.
