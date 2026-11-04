# UVCProbeFix

[![Build](https://github.com/danielnmaldonado/UVCProbeFix/actions/workflows/ci.yml/badge.svg)](https://github.com/danielnmaldonado/UVCProbeFix/actions/workflows/ci.yml)

**Laptop UVC camera black screen fix for macOS.**

A Lilu plugin that repairs malformed UVC negotiation responses which prevent supported laptop cameras from starting video. Includes the Sonix `174F:1176` recovery and the existing targeted HP/Chicony `04F2:B6BF` workaround.

Developed for the **Lenovo S145 (Ice Lake)**. Other laptop models may require adaptation.

## Compatibility

Intel/x86_64 only.

| macOS | Kernel target |
| --- | --- |
| Sequoia | Any Darwin `24.x` kernel |
| Tahoe | Any Darwin `25.x` kernel |

The S145 recovery has no minor-kernel restriction. It still verifies the UVC driver bytes before applying the patch. Physical camera testing of the Sequoia adaptation is pending. The separate HP workaround retains its existing ABI checks.

## Install

1. Install [Lilu](https://github.com/acidanthera/Lilu) `1.7.2` or newer.
2. Copy `UVCProbeFix.kext` to `EFI/OC/Kexts` and add it after Lilu in OpenCore `Kernel -> Add`.
3. Use `Arch=x86_64`, `MinKernel=24.0.0`, `MaxKernel=25.99.99`, and `Enabled=true`. An example entry is included in [OpenCore-Kernel-Add-entry.plist](OpenCore-Kernel-Add-entry.plist).
4. Reboot and test the camera. Run `Tools/PostBoot-UVCProbeFix-Verify.command` to inspect loading and patch logs.

Keep a backup of the previous kext. The boot argument `-uvcpoff` disables the plugin.

## Build

On an Intel Mac with full Xcode selected:

```sh
./Build.command
```

The builder verifies pinned dependencies, runs tests and static analysis, and creates an ad-hoc signed Release ZIP in `dist/`. It does not install the kext.

[GitHub Actions](https://github.com/danielnmaldonado/UVCProbeFix/actions) builds and validates each change. Successful builds on `main` automatically publish versioned release packages. Automated checks do not replace physical camera testing.

## License

[BSD 3-Clause](LICENSE). Copyright © 2026 DNMTechLabs.
