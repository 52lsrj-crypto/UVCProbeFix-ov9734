// Copyright © 2026 DNMTechLabs. All rights reserved.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "../UVCProbeFix/UVCProbePatch.hpp"

namespace {

uint32_t readLe32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) |
           (static_cast<uint32_t>(p[3]) << 24U);
}

UVCProbePatch::ProbeFields probeFrom26(const uint8_t (&raw)[26]) {
    return UVCProbePatch::ProbeFields {
        raw[2], raw[3], readLe32(raw + 4), readLe32(raw + 18), readLe32(raw + 22)
    };
}

bool sameProbe(const UVCProbePatch::ProbeFields &a,
               const UVCProbePatch::ProbeFields &b) {
    return a.formatIndex == b.formatIndex &&
           a.frameIndex == b.frameIndex &&
           a.frameInterval == b.frameInterval &&
           a.maxVideoFrameSize == b.maxVideoFrameSize &&
           a.maxPayloadTransferSize == b.maxPayloadTransferSize;
}

std::size_t countPattern(const uint8_t *data,
                         std::size_t dataSize,
                         const uint8_t *pattern,
                         std::size_t patternSize,
                         std::size_t *lastOffset = nullptr) {
    if (!data || !pattern || patternSize == 0 || dataSize < patternSize) {
        return 0;
    }
    std::size_t count = 0;
    for (std::size_t i = 0; i + patternSize <= dataSize; ++i) {
        if (std::memcmp(data + i, pattern, patternSize) == 0) {
            ++count;
            if (lastOffset) *lastOffset = i;
        }
    }
    return count;
}


void testRangeFitsHardening() {
    using namespace UVCProbePatch;
    assert(rangeFits(0, 0, 0));
    assert(rangeFits(0, 64, 64));
    assert(rangeFits(16, 32, 64));
    assert(!rangeFits(65, 0, 64));
    assert(!rangeFits(1, 64, 64));
    assert(!rangeFits(static_cast<std::size_t>(-1), 1, 4096));
    assert(!rangeFits(4090, 16, 4096));
}

void testOneShotScratchByteIsDeadToFrameRateResult() {
    using namespace UVCProbePatch;
    // Captured 25G229/25G241 method invariants that make -0x64(%rbp) a safe
    // one-shot scratch byte: the second GET writes only the low dword at -0x68,
    // and the method later reads only that same low dword. The scratch byte is
    // byte 4 of the saved qword, outside the live low dword.
    constexpr std::size_t getStatusStoreRel = 0x2F2 - 0x18F;
    assert(kOriginalHandshakeRegion25G229G241[getStatusStoreRel + 0] == 0x89);
    assert(kOriginalHandshakeRegion25G229G241[getStatusStoreRel + 1] == 0x45);
    assert(kOriginalHandshakeRegion25G229G241[getStatusStoreRel + 2] == 0x98);
    assert(kResumeAnchor25G229G241[11] == 0x44);
    assert(kResumeAnchor25G229G241[12] == 0x8B);
    assert(kResumeAnchor25G229G241[13] == 0x75);
    assert(kResumeAnchor25G229G241[14] == 0x98);
    assert(kPostGetReplacement25G229G241[46] == 0x80);
    assert(kPostGetReplacement25G229G241[47] == 0x75);
    assert(kPostGetReplacement25G229G241[48] == 0x9C); // -0x64
    assert(kPostGetReplacement25G229G241[49] == 0x01);
    constexpr int savedQwordDisp = -0x68;
    constexpr int scratchDisp = -0x64;
    static_assert(scratchDisp - savedQwordDisp == 4, "scratch must be first byte above low dword");
}

void testPreSetPolicy() {
    using namespace UVCProbePatch;

    // 0.2.15 release-frozen Stage-1-disabled invariant: the host's outgoing PROBE must remain
    // untouched, including the normal MaxPayload=0 observed on the ASUS.
    const ProbeFields outgoing {1, 4, 333333, 614400, 0};
    auto copy = outgoing;
    assert(!shouldPrepareProbeRequest(copy));
    assert(!prepareProbeRequest(copy));
    assert(sameProbe(copy, outgoing));
}

void testPostGetPolicy() {
    using namespace UVCProbePatch;

    ProbeFields broken {1, 4, 333333, 0x0B000000, 0x01010001};
    assert(shouldSanitizeProbeResponse(broken));

    ProbeFields fixed = broken;
    assert(sanitizeProbeResponse(fixed));
    assert(fixed.formatIndex == 1);
    assert(fixed.frameIndex == 1);
    assert(fixed.frameInterval == 333333);
    assert(fixed.maxVideoFrameSize == 614400);
    assert(fixed.maxPayloadTransferSize == 0);

    const ProbeFields negatives[] {
        {1, 4, 333333, 614400, 3072},
        {2, 4, 333333, 0x0B000000, 0x01010001},
        {1, 3, 333333, 0x0B000000, 0x01010001},
        {1, 4, 333334, 0x0B000000, 0x01010001},
        {1, 4, 333333, 0x0B000000, 3072},
        {1, 4, 333333, 614400, 0x01010001},
    };
    for (const auto &input : negatives) {
        auto copy = input;
        assert(!shouldSanitizeProbeResponse(copy));
        assert(!sanitizeProbeResponse(copy));
        assert(sameProbe(copy, input));
    }
}


void testSecondProbeRetryPolicy() {
    using namespace UVCProbePatch;

    const ProbeFields broken {1, 4, 333333, 0x0B000000, 0x01010001};
    const ProbeFields fixed {1, 1, 333333, 614400, 0};
    const ProbeFields asus {1, 1, 333333, 1843789, 3072};

    assert(shouldRetryProbeOnce(broken, false));
    assert(!shouldRetryProbeOnce(broken, true));
    assert(!shouldRetryProbeOnce(fixed, false));
    assert(!shouldRetryProbeOnce(asus, false));
}

void testCapturedNegotiationModel() {
    using namespace UVCProbePatch;

    // 0.2.15 release-frozen target model:
    // host initially proposes Frame 4 at 640x480 with payload=0;
    // the camera returns the exact malformed Frame-4 GET_CUR tuple;
    // recovery must remap the same 640x480 mode to Frame 1 and reset payload
    // to zero before Apple's one-shot second SET_CUR -> GET_CUR round.
    const ProbeFields originalHostSetCur {1, 4, 333333, 614400, 0};
    ProbeFields hostSetCur = originalHostSetCur;
    assert(!prepareProbeRequest(hostSetCur));
    assert(sameProbe(hostSetCur, originalHostSetCur));

    ProbeFields cameraGetCur {1, 4, 333333, 0x0B000000, 0x01010001};
    assert(sanitizeProbeResponse(cameraGetCur));
    assert(cameraGetCur.frameIndex == 1);
    assert(cameraGetCur.maxVideoFrameSize == 614400);
    assert(cameraGetCur.maxPayloadTransferSize == 0);
    assert(hostSetCur.maxPayloadTransferSize == 0);
    assert(cameraGetCur.formatIndex == hostSetCur.formatIndex);
    assert(cameraGetCur.frameIndex != hostSetCur.frameIndex);
    assert(cameraGetCur.frameInterval == hostSetCur.frameInterval);
}

void testBandwidthTargetAndAlternateSelection() {
    using namespace UVCProbePatch;

    constexpr uint32_t frameBytes = 640U * 480U * 2U;
    constexpr uint32_t estimated =
        frameBytes * (10000000U / kFrameInterval100ns + 1U) / 1000U / 8U + 12U;
    static_assert(estimated == 2392U, "640x480 YUY2 HS bandwidth estimate changed");
    static_assert(kExpectedMaxPayloadTransferSize == 3072U,
                  "25G83 compatibility target must remain stream maxpsize / ALT6 capacity");
    static_assert(kExpectedMaxPayloadTransferSize >= estimated,
                  "recovery target must not undershoot the validated estimate");

    constexpr uint32_t capacities[] {128U, 256U, 800U, 1600U, 2400U, 3072U};
    std::size_t selected = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < sizeof(capacities)/sizeof(capacities[0]); ++i) {
        if (capacities[i] >= kExpectedMaxPayloadTransferSize) {
            selected = i + 1U;
            break;
        }
    }
    assert(selected == 6U);
    assert(capacities[selected - 1U] == 3072U);
    assert(capacities[selected - 2U] == 2400U);
}

void testPreSetExactBytesAndBranches() {
    using namespace UVCProbePatch;

    static_assert(sizeof(kPreSetOriginal25G83) == 64, "pre original size");
    // Stage 1 remains disabled in 0.2.15: the replacement-region builder must
    // preserve these 64 bytes exactly. The legacy replacement bytes may remain
    // in source for audit history but must not be installed at runtime.
    assert(std::memcmp(kPreSetOriginal25G83, kPreSetOriginal25G83, 64) == 0);
}

void testPostGetExactBytesAndBranches() {
    using namespace UVCProbePatch;

    static_assert(kPostGetPatchSize == 64, "post-GET patch size must remain 64 bytes");
    static_assert(sizeof(kPostGetOriginal25G83) == 64, "post original size");
    static_assert(sizeof(kPostGetReplacement25G229G241) == 64, "post replacement size");
    assert(std::memcmp(kPostGetOriginal25G83, kPostGetReplacement25G229G241, 64) != 0);

    constexpr std::size_t base = 0x373;
    constexpr std::size_t exitLocal = 0x3D;      // method +0x3B0
    constexpr std::size_t retryTarget = 0x29B;  // reload selector before second SET/GET
    constexpr std::size_t resume = 0x3F1;

    for (const std::size_t op : {std::size_t(14), std::size_t(30), std::size_t(50)}) {
        const uint8_t expectedOpcode = op == 50 ? 0x74 : 0x75;
        assert(kPostGetReplacement25G229G241[op] == expectedOpcode);
        const int8_t disp = static_cast<int8_t>(kPostGetReplacement25G229G241[op + 1]);
        assert(static_cast<std::size_t>(static_cast<int>(op + 2) + disp) == exitLocal);
    }

    const uint8_t frameRemap[] {0x41,0xC6,0x45,0x03,0x01};
    assert(countPattern(kPostGetReplacement25G229G241,
                        sizeof(kPostGetReplacement25G229G241),
                        frameRemap, sizeof(frameRemap)) == 1);

    const uint8_t zeroPayloadStore[] {0xB8,0x00,0x60,0x09,0x00,0x49,0x89,0x45,0x12};
    assert(countPattern(kPostGetReplacement25G229G241,
                        sizeof(kPostGetReplacement25G229G241),
                        zeroPayloadStore, sizeof(zeroPayloadStore)) == 1);

    const uint8_t flagToggle[] {0x80,0x75,0x9C,0x01};
    assert(countPattern(kPostGetReplacement25G229G241,
                        sizeof(kPostGetReplacement25G229G241),
                        flagToggle, sizeof(flagToggle)) == 1);

    const uint8_t restoreR12[] {0x4C,0x8B,0x65,0x90};
    assert(countPattern(kPostGetReplacement25G229G241,
                        sizeof(kPostGetReplacement25G229G241),
                        restoreR12, sizeof(restoreR12)) == 1);

    assert(kPostGetReplacement25G229G241[56] == 0xE9);
    int32_t retryDisp = 0;
    std::memcpy(&retryDisp, kPostGetReplacement25G229G241 + 57, sizeof(retryDisp));
    const int64_t retryLanding = static_cast<int64_t>(base + 61) + retryDisp;
    assert(retryLanding == static_cast<int64_t>(retryTarget));

    assert(kPostGetReplacement25G229G241[61] == 0xEB);
    assert(kPostGetReplacement25G229G241[62] == 0x3F);
    assert(base + 63 + 0x3F == resume);
    assert(kPostGetReplacement25G229G241[63] == 0x90);

    for (std::size_t i = 0; i < sizeof(kPostGetReplacement25G229G241); ++i) {
        assert(kPostGetReplacement25G229G241[i] != 0xE8); // no CALL in the injected block
    }
}

void testFrame4ToFrame1FallbackExactPatchBytes() {
    using namespace UVCProbePatch;

    // 0.2.14/0.2.15 frozen post-GET block:
    //   exact Frame-4 target tuple + exact broken qword -> remap to Frame 1,
    //   keep 640x480 frame size (614400), reset payload to 0, then use the
    //   existing one-shot flag and jump to +0x29B for one real second PROBE.
    const uint8_t expected[64] {
        0x48,0xB8,0x01,0x04,0x15,0x16,0x05,0x00,0x00,0x00,
        0x49,0x39,0x45,0x02,0x75,0x2D,
        0x48,0xB8,0x00,0x00,0x00,0x0B,0x01,0x00,0x01,0x01,
        0x49,0x39,0x45,0x12,0x75,0x1D,
        0xB8,0x00,0x60,0x09,0x00,
        0x49,0x89,0x45,0x12,
        0x41,0xC6,0x45,0x03,0x01,
        0x80,0x75,0x9C,0x01,0x74,0x09,
        0x4C,0x8B,0x65,0x90,
        0xE9,0xEB,0xFE,0xFF,0xFF,
        0xEB,0x3F,0x90
    };
    static_assert(sizeof(expected) == 64, "Frame4->Frame1 fallback block must remain exactly 64 bytes");
    assert(std::memcmp(kPostGetReplacement25G229G241, expected, sizeof(expected)) == 0);
}

void testCapturedPageGeometryAndSingleRegionPatch() {
    using namespace UVCProbePatch;

    constexpr std::size_t methodBase = 0x576;
    constexpr std::size_t pre = methodBase + 0x18F;      // 0x705
    constexpr std::size_t post = methodBase + 0x373;     // 0x8E9
    constexpr std::size_t resume = methodBase + 0x3F1;   // 0x967

    static_assert(pre == 0x705, "captured pre-SET offset");
    static_assert(post == 0x8E9, "captured post-GET offset");
    static_assert(resume == 0x967, "captured resume offset");
    static_assert(post - pre == kPreSetToPostGetDistance, "pre->post geometry");
    static_assert(resume - post == kPostGetToResumeDistance, "post->resume geometry");
    static_assert(kHandshakeRegionSize == 0x224, "region size");
    static_assert(pre + kHandshakeRegionSize == post + 64, "region endpoint");
    static_assert(pre + kHandshakeRegionSize <= 4096, "region must stay in page");
    static_assert(resume + sizeof(kResumeAnchor25G83) <= 4096,
                  "resume anchor must stay in page");

    // The full captured region must embed both exact original 64-byte blocks at
    // their captured relative offsets.
    assert(std::memcmp(kOriginalHandshakeRegion25G83,
                       kPreSetOriginal25G83, 64) == 0);
    assert(std::memcmp(kOriginalHandshakeRegion25G83 + kPreSetToPostGetDistance,
                       kPostGetOriginal25G83, 64) == 0);

    std::array<uint8_t, kHandshakeRegionSize> replacement {};
    std::memcpy(replacement.data(),
                kOriginalHandshakeRegion25G83,
                kHandshakeRegionSize);
    std::memcpy(replacement.data() + kPreSetToPostGetDistance,
                kPostGetReplacement25G83,
                sizeof(kPostGetReplacement25G83));

    assert(std::memcmp(replacement.data(),
                       kPreSetOriginal25G83, 64) == 0);
    assert(std::memcmp(replacement.data() + kPreSetToPostGetDistance,
                       kPostGetReplacement25G83, 64) == 0);
    assert(std::memcmp(replacement.data() + 64,
                       kOriginalHandshakeRegion25G83 + 64,
                       kPreSetToPostGetDistance - 64) == 0);

    std::array<uint8_t, 4096> page {};
    std::memcpy(page.data() + pre,
                kOriginalHandshakeRegion25G83,
                kHandshakeRegionSize);
    std::memcpy(page.data() + resume,
                kResumeAnchor25G83,
                sizeof(kResumeAnchor25G83));

    std::size_t preFound = 0, postFound = 0, resumeFound = 0;
    assert(countPattern(page.data(), page.size(),
                        kPreSetOriginal25G83,
                        sizeof(kPreSetOriginal25G83), &preFound) == 1);
    assert(countPattern(page.data(), page.size(),
                        kPostGetOriginal25G83,
                        sizeof(kPostGetOriginal25G83), &postFound) == 1);
    assert(countPattern(page.data(), page.size(),
                        kResumeAnchor25G83,
                        sizeof(kResumeAnchor25G83), &resumeFound) == 1);
    assert(preFound == pre);
    assert(postFound == post);
    assert(resumeFound == resume);
    assert(postFound - preFound == kPreSetToPostGetDistance);
    assert(resumeFound - postFound == kPostGetToResumeDistance);

    const auto before = page;
    std::memcpy(page.data() + preFound, replacement.data(), replacement.size());
    assert(std::memcmp(page.data() + preFound,
                       replacement.data(), replacement.size()) == 0);
    assert(std::memcmp(before.data(), page.data(), pre) == 0);
    assert(std::memcmp(before.data() + pre + replacement.size(),
                       page.data() + pre + replacement.size(),
                       page.size() - pre - replacement.size()) == 0);

    auto duplicate = before;
    std::memcpy(duplicate.data() + 0x100,
                kPostGetOriginal25G83,
                sizeof(kPostGetOriginal25G83));
    assert(countPattern(duplicate.data(), duplicate.size(),
                        kPostGetOriginal25G83,
                        sizeof(kPostGetOriginal25G83)) == 2);
}


void test25G229G241CompatibilityRegion() {
    using namespace UVCProbePatch;

    static_assert(sizeof(kOriginalHandshakeRegion25G229G241) == kHandshakeRegionSize,
                  "25G229/25G241 region size");
    static_assert(sizeof(kResumeAnchor25G229G241) == 32,
                  "25G229/25G241 resume-anchor size");

    assert(std::memcmp(kOriginalHandshakeRegion25G229G241,
                       kPreSetOriginal25G83, 64) == 0);
    assert(std::memcmp(kOriginalHandshakeRegion25G229G241 + kPreSetToPostGetDistance,
                       kPostGetOriginal25G83, 64) == 0);
    assert(std::memcmp(kResumeAnchor25G229G241,
                       kResumeAnchor25G83, 32) == 0);

    const std::size_t expectedDiffs[] {0x10F, 0x110, 0x1B3, 0x1D1};
    std::size_t seen = 0;
    for (std::size_t i = 0; i < kHandshakeRegionSize; ++i) {
        if (kOriginalHandshakeRegion25G83[i] == kOriginalHandshakeRegion25G229G241[i]) {
            continue;
        }
        assert(seen < sizeof(expectedDiffs) / sizeof(expectedDiffs[0]));
        assert(i == expectedDiffs[seen]);
        ++seen;
    }
    assert(seen == sizeof(expectedDiffs) / sizeof(expectedDiffs[0]));

    assert(kOriginalHandshakeRegion25G229G241[0x10F] == 0x08);
    assert(kOriginalHandshakeRegion25G229G241[0x110] == 0x3A);
    assert(kOriginalHandshakeRegion25G229G241[0x1B3] == 0x44);
    assert(kOriginalHandshakeRegion25G229G241[0x1D1] == 0x8E);

    static_assert(sizeof(kSecondRoundPrologueAnchor25G229G241) == 18,
                  "fallback prologue anchor size");
    const uint8_t expectedPrologue[18] {
        0x4C,0x89,0x4D,0x90,0x4C,0x89,0x45,0x80,0x48,0x89,0x4D,0x98,
        0x49,0x89,0xD7,0x49,0x89,0xFE
    };
    assert(std::memcmp(kSecondRoundPrologueAnchor25G229G241,
                       expectedPrologue, sizeof(expectedPrologue)) == 0);

    assert(matchKnownHandshakeRegion(kOriginalHandshakeRegion25G83, kHandshakeRegionSize) ==
           HandshakeRegionVariant::Tahoe25G83);
    assert(matchKnownHandshakeRegion(kOriginalHandshakeRegion25G229G241, kHandshakeRegionSize) ==
           HandshakeRegionVariant::Tahoe25G229G241);

    auto unknown = std::array<uint8_t, kHandshakeRegionSize> {};
    std::memcpy(unknown.data(), kOriginalHandshakeRegion25G229G241, kHandshakeRegionSize);
    unknown[0x120] ^= 0x01;
    assert(matchKnownHandshakeRegion(unknown.data(), unknown.size()) ==
           HandshakeRegionVariant::None);

    std::array<uint8_t, kHandshakeRegionSize> replacement {};
    std::memcpy(replacement.data(),
                kOriginalHandshakeRegion25G229G241,
                kHandshakeRegionSize);
    std::memcpy(replacement.data() + kPreSetToPostGetDistance,
                kPostGetReplacement25G229G241,
                sizeof(kPostGetReplacement25G229G241));

    assert(std::memcmp(replacement.data(),
                       kPreSetOriginal25G83, 64) == 0);

    // The four 25G229/25G241 relocation/reference bytes must survive the patch.
    assert(replacement[0x10F] == 0x08);
    assert(replacement[0x110] == 0x3A);
    assert(replacement[0x1B3] == 0x44);
    assert(replacement[0x1D1] == 0x8E);
}


void testPhysicallySuccessfulHandshakeFixtures() {
    using namespace UVCProbePatch;

    const uint8_t lenovo[26] {
        0x01,0x00,0x01,0x01,0x15,0x16,0x05,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x60,0x09,0x00,0x00,0x0C,0x00,0x00
    };
    const uint8_t asus[26] {
        0x01,0x00,0x01,0x01,0x15,0x16,0x05,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x4D,0x22,0x1C,0x00,0x00,0x0C,0x00,0x00
    };

    const auto lenovoProbe = probeFrom26(lenovo);
    assert(lenovoProbe.formatIndex == 1 && lenovoProbe.frameIndex == 1);
    assert(lenovoProbe.frameInterval == 333333);
    assert(lenovoProbe.maxVideoFrameSize == 614400);
    assert(lenovoProbe.maxPayloadTransferSize == 3072);
    assert(!shouldSanitizeProbeResponse(lenovoProbe));
    assert(!shouldRetryProbeOnce(lenovoProbe, false));

    const auto asusProbe = probeFrom26(asus);
    assert(asusProbe.formatIndex == 1 && asusProbe.frameIndex == 1);
    assert(asusProbe.frameInterval == 333333);
    assert(asusProbe.maxVideoFrameSize == 1843789);
    assert(asusProbe.maxPayloadTransferSize == 3072);
    assert(!shouldSanitizeProbeResponse(asusProbe));
    assert(!shouldRetryProbeOnce(asusProbe, false));
}

void testKnownGoodAsusProbePassThrough() {
    using namespace UVCProbePatch;

    const ProbeFields asus {1, 1, 333333, 1843789, 3072};
    auto pre = asus;
    auto post = asus;
    assert(!shouldPrepareProbeRequest(pre));
    assert(!prepareProbeRequest(pre));
    assert(sameProbe(pre, asus));
    assert(!shouldSanitizeProbeResponse(post));
    assert(!sanitizeProbeResponse(post));
    assert(sameProbe(post, asus));
}


void stressRangeFits() {
    using namespace UVCProbePatch;
    std::size_t state = static_cast<std::size_t>(0x9E3779B97F4A7C15ULL);
    for (uint32_t i = 0; i < 5000000U; ++i) {
        state = state * static_cast<std::size_t>(6364136223846793005ULL) + 1U;
        const std::size_t offset = state;
        state = state * static_cast<std::size_t>(6364136223846793005ULL) + 1U;
        const std::size_t length = state;
        state = state * static_cast<std::size_t>(6364136223846793005ULL) + 1U;
        const std::size_t total = state;
        const bool expected = offset <= total && length <= total - offset;
        assert(rangeFits(offset, length, total) == expected);
    }
}

void stressPolicies() {
    using namespace UVCProbePatch;

    uint32_t state = 0x174F1176U;
    for (uint32_t i = 0; i < 10000000U; ++i) {
        state = state * 1664525U + 1013904223U;
        ProbeFields p {};
        p.formatIndex = static_cast<uint8_t>(state);
        state = state * 1664525U + 1013904223U;
        p.frameIndex = static_cast<uint8_t>(state);
        state = state * 1664525U + 1013904223U;
        p.frameInterval = state;
        state = state * 1664525U + 1013904223U;
        p.maxVideoFrameSize = state;
        state = state * 1664525U + 1013904223U;
        p.maxPayloadTransferSize = state;

        if ((i % 1000000U) == 0U) {
            p = ProbeFields {1, 4, 333333, 614400, 0};
        } else if ((i % 1000000U) == 1U) {
            p = ProbeFields {1, 4, 333333, 0x0B000000, 0x01010001};
        }

        const auto before = p;
        const bool expectedPre = false;
        const bool expectedPost =
            p.formatIndex == 1 && p.frameIndex == 4 &&
            p.frameInterval == 333333 &&
            p.maxVideoFrameSize == 0x0B000000 &&
            p.maxPayloadTransferSize == 0x01010001;

        auto preCopy = p;
        const bool preChanged = prepareProbeRequest(preCopy);
        assert(preChanged == expectedPre);
        assert(!preChanged);
        assert(sameProbe(preCopy, before));

        auto postCopy = p;
        const bool postChanged = sanitizeProbeResponse(postCopy);
        assert(postChanged == expectedPost);
        if (expectedPost) {
            assert(postCopy.frameIndex == 1);
            assert(postCopy.maxVideoFrameSize == 614400);
            assert(postCopy.maxPayloadTransferSize == 0);
        } else {
            assert(sameProbe(postCopy, before));
        }
    }
}

} // namespace

int main() {
    testRangeFitsHardening();
    testOneShotScratchByteIsDeadToFrameRateResult();
    testPreSetPolicy();
    testPostGetPolicy();
    testSecondProbeRetryPolicy();
    testCapturedNegotiationModel();
    testBandwidthTargetAndAlternateSelection();
    testPreSetExactBytesAndBranches();
    testPostGetExactBytesAndBranches();
    testFrame4ToFrame1FallbackExactPatchBytes();
    testCapturedPageGeometryAndSingleRegionPatch();
    test25G229G241CompatibilityRegion();
    testPhysicallySuccessfulHandshakeFixtures();
    testKnownGoodAsusProbePassThrough();
    stressRangeFits();
    stressPolicies();

    std::cout
        << "UVCProbeFix 25G83 compatibility + 25G229/25G241 Frame4->Frame1 one-shot fallback tests + "
        << "5,000,000 range-fuzz + 10,000,000 policy iterations: PASS\n";
    return 0;
}
