# Changelog

## 0.3.2 (302)

- Adapt the existing Tahoe Frame 4 to Frame 1, single-retry PROBE recovery to Sequoia 15.0 build `24A335`.
- Match the complete captured Sequoia method before applying its own instruction offsets and stack/register layout.
- Preserve the Tahoe patch bytes, recovery policy, and HP module.
- Admit the same kext through OpenCore on Darwin `24.0.0` through `25.6.0`, with exact runtime and fingerprint checks.
- Add native x86_64 patch execution, fingerprint rejection, byte-preservation, and retry-limit tests.
- Add GitHub Actions validation, Release builds, downloadable artifacts, and version-tag release automation.

Physical camera operation on Sequoia is awaiting hardware validation. The generic transport remains excluded from Release.
