# Third-party notices

UVCProbeFix builds as a Lilu plugin and depends on:

- Lilu 1.7.2 — Acidanthera — BSD 3-Clause. The build script downloads the official Debug SDK archive and verifies its SHA-256.
- MacKernelSDK — Acidanthera — pinned to commit `05094e5e88cec7caedbfb35e8449ed0db94bf95b` for kernel headers and `libkmod.a`.

The production Release preserves the legacy 0.2.15 Tahoe shared-cache recovery and adds a separate Sequoia 24A335 adaptation. Its exact binary fingerprints and replacement bytes were derived from supplied runtime evidence. The descriptor-driven generic transport remains isolated in the Debug/Test configuration.

The 0.3.x generic engine was designed with the same broad class of defensive principles used by Linux `uvcvideo`: descriptor-based format/frame validation, descriptor-derived uncompressed frame size, conservative correction of malformed payload values, endpoint-capacity bounds, and bounded renegotiation. No Linux source code is copied into this project.

Apple's IOUSBHostFamily/UVCFamily interfaces are referenced for compatibility and routing. This repository includes the small method-byte fingerprints and fixtures needed for exact matching and regression tests; it does not bundle complete Apple executables or user diagnostic archives.
