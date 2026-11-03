//
// UVCProbeFix 0.2.15
// Copyright © 2026 DNMTechLabs. All rights reserved.
// Contact: dnmtechlabs@gmail.com
//
// Tahoe 25G83 / 25G229 / 25G241 exact UVC PROBE recovery patch.
//
// Safety boundary:
//   - no global USB request interception;
//   - no IOUSBHostInterface::deviceRequest hook;
//   - one Lilu _cs_validate_page route targets only exact known UVCFamily
//     shared-cache fingerprints;
//   - complete 548-byte handshake region and resume anchor must match;
//   - 25G229/25G241 additionally requires an exact prologue/stack-layout anchor;
//   - pre-SET probe bytes remain original (Stage 1 disabled);
//   - fallback remains restricted to 1/4/333333 and the exact broken
//     frame/payload values;
//   - 25G229/25G241 remaps only that broken duplicate Frame 4 to equivalent
//     640x480 Frame 1 with payload zero, then performs at most one second
//     SET_CUR -> GET_CUR round by re-entering Apple's existing request path
//     at doProbeControl +0x29B;
//   - 25G83 retains the 0.2.12 Stage-2-only behavior.
//
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_user.hpp>

#include <sys/vnode.h>

#include "UVCProbePatch.hpp"
#include "UVC15ProbeOverrun.hpp" // HP_INTEGRATION_INCLUDE
#include "UVCSequoia.hpp" // SEQUOIA_INTEGRATION_INCLUDE

#define MODULE_SHORT "uvcp"

namespace {

enum : uint32_t {
    PatchWaiting = 0,
    PatchApplying = 1,
    PatchApplied = 2,
    PatchFailed = 3
};

static mach_vm_address_t originalCsValidatePage {};
static uint32_t sharedCachePatchState {PatchWaiting};

static uint32_t loadPatchState() {
    return __atomic_load_n(&sharedCachePatchState, __ATOMIC_ACQUIRE);
}

static void storePatchState(uint32_t state) {
    __atomic_store_n(&sharedCachePatchState, state, __ATOMIC_RELEASE);
}

static bool tryBeginPatch() {
    uint32_t expected = PatchWaiting;
    return __atomic_compare_exchange_n(
        &sharedCachePatchState,
        &expected,
        PatchApplying,
        false,
        __ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE
    );
}

static bool findUniqueExactMatch(const uint8_t *page,
                                 size_t pageSize,
                                 const uint8_t *pattern,
                                 size_t patternSize,
                                 size_t &matchOffset) {
    if (page == nullptr || pattern == nullptr ||
        patternSize == 0 || pageSize < patternSize) {
        return false;
    }

    bool found = false;
    const size_t last = pageSize - patternSize;
    for (size_t offset = 0; offset <= last; ++offset) {
        if (memcmp(page + offset, pattern, patternSize) != 0) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        matchOffset = offset;
    }
    return found;
}

static bool validateKnownPageGeometry(
    const uint8_t *page,
    size_t pageSize,
    size_t &regionOffset,
    UVCProbePatch::HandshakeRegionVariant &variant) {
    size_t preSetOffset = 0;
    size_t postGetOffset = 0;
    size_t resumeOffset = 0;

    if (page == nullptr ||
        pageSize < UVCProbePatch::kHandshakeRegionSize ||
        pageSize < sizeof(UVCProbePatch::kResumeAnchor25G83)) {
        return false;
    }

    // The three local anchors are byte-identical on 25G83 and the captured
    // 25G229/25G241 method. They locate the candidate geometry; the complete
    // 548-byte region below is the actual exact-build fingerprint.
    if (!findUniqueExactMatch(
            page, pageSize,
            UVCProbePatch::kPreSetOriginal25G83,
            sizeof(UVCProbePatch::kPreSetOriginal25G83),
            preSetOffset) ||
        !findUniqueExactMatch(
            page, pageSize,
            UVCProbePatch::kPostGetOriginal25G83,
            sizeof(UVCProbePatch::kPostGetOriginal25G83),
            postGetOffset) ||
        !findUniqueExactMatch(
            page, pageSize,
            UVCProbePatch::kResumeAnchor25G83,
            sizeof(UVCProbePatch::kResumeAnchor25G83),
            resumeOffset)) {
        return false;
    }

    if (postGetOffset < preSetOffset || resumeOffset < postGetOffset) {
        return false;
    }
    if (postGetOffset - preSetOffset != UVCProbePatch::kPreSetToPostGetDistance ||
        resumeOffset - postGetOffset != UVCProbePatch::kPostGetToResumeDistance) {
        return false;
    }

    if (!UVCProbePatch::rangeFits(
            preSetOffset, UVCProbePatch::kHandshakeRegionSize, pageSize) ||
        !UVCProbePatch::rangeFits(
            resumeOffset, sizeof(UVCProbePatch::kResumeAnchor25G83), pageSize)) {
        return false;
    }

    variant = UVCProbePatch::matchKnownHandshakeRegion(
        page + preSetOffset,
        UVCProbePatch::kHandshakeRegionSize
    );
    if (variant == UVCProbePatch::HandshakeRegionVariant::None) {
        return false;
    }

    // 0.2.15's release-frozen Frame4->Frame1 fallback depends on the captured 25G229/25G241 stack
    // layout. Require the exact prologue save sequence and its geometry before
    // selecting the Frame4->Frame1 fallback replacement. 25G83 keeps 0.2.12 behavior.
    if (variant == UVCProbePatch::HandshakeRegionVariant::Tahoe25G229G241) {
        size_t prologueOffset = 0;
        if (!findUniqueExactMatch(
                page, pageSize,
                UVCProbePatch::kSecondRoundPrologueAnchor25G229G241,
                sizeof(UVCProbePatch::kSecondRoundPrologueAnchor25G229G241),
                prologueOffset) ||
            !UVCProbePatch::rangeFits(
                prologueOffset,
                sizeof(UVCProbePatch::kSecondRoundPrologueAnchor25G229G241),
                pageSize) ||
            preSetOffset < prologueOffset ||
            preSetOffset - prologueOffset != UVCProbePatch::kPrologueToPreSetDistance) {
            return false;
        }
    }

    // Keep an independent resume-anchor check tied to the selected fingerprint.
    const uint8_t *resumeAnchor =
        variant == UVCProbePatch::HandshakeRegionVariant::Tahoe25G229G241
            ? UVCProbePatch::kResumeAnchor25G229G241
            : UVCProbePatch::kResumeAnchor25G83;
    if (memcmp(page + resumeOffset,
               resumeAnchor,
               sizeof(UVCProbePatch::kResumeAnchor25G83)) != 0) {
        return false;
    }

    regionOffset = preSetOffset;
    return true;
}

static bool buildReplacementRegion(
    uint8_t *replacement,
    size_t replacementSize,
    UVCProbePatch::HandshakeRegionVariant variant) {
    if (replacement == nullptr ||
        replacementSize != UVCProbePatch::kHandshakeRegionSize) {
        return false;
    }

    const uint8_t *original =
        UVCProbePatch::originalHandshakeRegionForVariant(variant);
    if (original == nullptr) {
        return false;
    }

    memcpy(replacement,
           original,
           UVCProbePatch::kHandshakeRegionSize);

    const uint8_t *postReplacement =
        UVCProbePatch::postGetReplacementForVariant(variant);
    if (postReplacement == nullptr) {
        return false;
    }

    memcpy(replacement + UVCProbePatch::kPreSetToPostGetDistance,
           postReplacement,
           UVCProbePatch::kPostGetPatchSize);
    return true;
}

static void patchedCsValidatePage(vnode_t vnode,
                                  memory_object_t pager,
                                  memory_object_offset_t pageOffset,
                                  const void *data,
                                  int *validated,
                                  int *tainted,
                                  int *nx) {
    // Preserve Apple's original code-signature validation semantics first.
    FunctionCast(patchedCsValidatePage, originalCsValidatePage)(
        vnode,
        pager,
        pageOffset,
        data,
        validated,
        tainted,
        nx
    );

    if (vnode == nullptr || data == nullptr ||
        loadPatchState() != PatchWaiting) {
        return;
    }

    char path[PATH_MAX] {};
    int pathLength = static_cast<int>(sizeof(path));
    if (vn_getpath(vnode, path, &pathLength) != 0 ||
        !UserPatcher::matchSharedCachePath(path)) {
        return;
    }

    const auto *page = static_cast<const uint8_t *>(data);
    size_t regionOffset = 0;
    UVCProbePatch::HandshakeRegionVariant variant =
        UVCProbePatch::HandshakeRegionVariant::None;
    if (!validateKnownPageGeometry(page, PAGE_SIZE, regionOffset, variant)) {
        return;
    }

    if (!tryBeginPatch()) {
        return;
    }

    auto *regionAddress = const_cast<uint8_t *>(page) + regionOffset;
    const uint8_t *originalRegion =
        UVCProbePatch::originalHandshakeRegionForVariant(variant);
    if (originalRegion == nullptr ||
        memcmp(regionAddress,
               originalRegion,
               UVCProbePatch::kHandshakeRegionSize) != 0) {
        storePatchState(PatchFailed);
        SYSLOG(
            MODULE_SHORT,
            "%s UVCFamily PROBE original-region recheck failed; patch disabled fail-closed",
            UVCProbePatch::handshakeRegionVariantName(variant)
        );
        return;
    }

    uint8_t replacementRegion[UVCProbePatch::kHandshakeRegionSize] {};
    if (!buildReplacementRegion(
            replacementRegion,
            sizeof(replacementRegion),
            variant)) {
        storePatchState(PatchFailed);
        SYSLOG(
            MODULE_SHORT,
            "%s UVCFamily PROBE replacement construction failed; patch disabled fail-closed",
            UVCProbePatch::handshakeRegionVariantName(variant)
        );
        return;
    }

    // A single exact-region replacement preserves the pre-SET bytes and selects
    // the variant-specific post-GET block. Search size equals pattern size, so
    // this operation can only target the already validated regionAddress.
    const bool replaced = KernelPatcher::findAndReplace(
        regionAddress,
        UVCProbePatch::kHandshakeRegionSize,
        originalRegion,
        UVCProbePatch::kHandshakeRegionSize,
        replacementRegion,
        sizeof(replacementRegion)
    );

    if (!replaced ||
        memcmp(regionAddress,
               replacementRegion,
               sizeof(replacementRegion)) != 0) {
        storePatchState(PatchFailed);
        SYSLOG(
            MODULE_SHORT,
            "%s UVCFamily PROBE replacement/read-back failed; patch disabled fail-closed",
            UVCProbePatch::handshakeRegionVariantName(variant)
        );
        return;
    }

    storePatchState(PatchApplied);
    SYSLOG(
        MODULE_SHORT,
        "%s UVCFamily PROBE recovery applied (25G83 compatibility or 25G229/25G241 Frame4->Frame1 one-shot fallback) in shared cache page 0x%llx",
        UVCProbePatch::handshakeRegionVariantName(variant),
        static_cast<unsigned long long>(pageOffset)
    );
}

static void patcherLoaded(void *, KernelPatcher &patcher) {
    // OpenCore independently restricts this kext to Darwin 25.6.0. Keep a
    // runtime guard as well; exact page bytes provide the stronger build-family gate.
    if (getKernelVersion() != KernelVersion::Tahoe ||
        getKernelMinorVersion() != UVCProbePatch::kDarwinMinor) {
        storePatchState(PatchFailed);
        SYSLOG(
            MODULE_SHORT,
            "unsupported Darwin version for known Tahoe 25.6 UVCFamily fingerprints; plugin is inactive"
        );
        return;
    }

    KernelPatcher::RouteRequest request(
        "_cs_validate_page",
        patchedCsValidatePage,
        originalCsValidatePage
    );

    if (!patcher.routeMultipleLong(
            KernelPatcher::KernelID,
            &request,
            1)) {
        SYSLOG(
            MODULE_SHORT,
            "failed to route code-signature page validation; plugin is inactive"
        );
        patcher.clearError();
        originalCsValidatePage = 0;
        storePatchState(PatchFailed);
        return;
    }

    DBGLOG(
        MODULE_SHORT,
        "shared-cache validator route installed; waiting for exact 25G83 or 25G229/25G241 PROBE region"
    );
}

static void pluginStart() {
    // SEQUOIA_INTEGRATION_BEGIN
    if (getKernelVersion() == KernelVersion::Sequoia) {
        if (!UVCSequoia::start()) {
            SYSLOG(MODULE_SHORT, "Sequoia PROBE recovery inactive");
        }
        return;
    }
    // SEQUOIA_INTEGRATION_END
    // HP_INTEGRATION_BEGIN: independent UVC 1.5 request-length recovery.
    if (!UVC15::start()) {
        SYSLOG(MODULE_SHORT, "HP UVC1.5 module inactive; Lenovo recovery unchanged");
    }
    // HP_INTEGRATION_END
    storePatchState(PatchWaiting);
    originalCsValidatePage = 0;

    const auto error = lilu.onPatcherLoad(patcherLoaded, nullptr);
    if (error != LiluAPI::Error::NoError) {
        storePatchState(PatchFailed);
        SYSLOG(
            MODULE_SHORT,
            "failed to register Lilu patcher callback (%d); plugin is inactive",
            static_cast<int>(error)
        );
    }
}

static const char *bootargOff[] {
    "-uvcpoff"
};

static const char *bootargDebug[] {
    "-uvcpdbg"
};

static const char *bootargBeta[] {
    "-uvcpbeta"
};

} // namespace

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Sequoia,
    KernelVersion::Tahoe,
    pluginStart
};
