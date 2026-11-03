// Sequoia 24A335 adaptation of the supplied Tahoe Frame4->Frame1 fix.
// Copyright © 2026 DNMTechLabs. All rights reserved.
#include "UVCSequoia.hpp"
#include "UVCSequoiaProbePatch.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_user.hpp>
#include <sys/vnode.h>

#define MODULE_SHORT "uvcp"

namespace UVCSequoia {
namespace {
enum : uint32_t { Waiting, Applying, Applied, Failed };
static uint32_t state {Waiting};
static mach_vm_address_t originalCsValidatePage {};

static void storeState(uint32_t value) {
    __atomic_store_n(&state, value, __ATOMIC_RELEASE);
}

static void patchedCsValidatePage(vnode_t vnode,
                                  memory_object_t pager,
                                  memory_object_offset_t pageOffset,
                                  const void *data,
                                  int *validated,
                                  int *tainted,
                                  int *nx) {
    // As in the Tahoe implementation, preserve Apple's validation first.
    FunctionCast(patchedCsValidatePage, originalCsValidatePage)(
        vnode, pager, pageOffset, data, validated, tainted, nx);
    if (vnode == nullptr || data == nullptr ||
        __atomic_load_n(&state, __ATOMIC_ACQUIRE) != Waiting) {
        return;
    }

    char path[PATH_MAX] {};
    int pathLength = static_cast<int>(sizeof(path));
    if (vn_getpath(vnode, path, &pathLength) != 0 ||
        !UserPatcher::matchSharedCachePath(path)) {
        return;
    }

    const auto *page = static_cast<const uint8_t *>(data);
    size_t methodOffset = 0;
    if (!UVCSequoiaProbePatch::findUniqueMethod(page, PAGE_SIZE, methodOffset)) {
        return;
    }
    uint32_t expected = Waiting;
    if (!__atomic_compare_exchange_n(&state, &expected, Applying, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return;
    }

    auto *method = const_cast<uint8_t *>(page) + methodOffset;
    if (memcmp(method, UVCSequoiaProbePatch::kOriginalMethod,
               UVCSequoiaProbePatch::kMethodSize) != 0) {
        storeState(Failed);
        SYSLOG(MODULE_SHORT, "Sequoia 24A335 PROBE original recheck failed; inactive");
        return;
    }

    uint8_t replacement[UVCSequoiaProbePatch::kMethodSize] {};
    if (!UVCSequoiaProbePatch::buildReplacementMethod(replacement, sizeof(replacement))) {
        storeState(Failed);
        SYSLOG(MODULE_SHORT, "Sequoia 24A335 PROBE replacement construction failed; inactive");
        return;
    }

    const bool replaced = KernelPatcher::findAndReplace(
        method, UVCSequoiaProbePatch::kMethodSize,
        UVCSequoiaProbePatch::kOriginalMethod, UVCSequoiaProbePatch::kMethodSize,
        replacement, sizeof(replacement));
    if (!replaced || memcmp(method, replacement, sizeof(replacement)) != 0) {
        storeState(Failed);
        SYSLOG(MODULE_SHORT, "Sequoia 24A335 PROBE replacement/read-back failed; inactive");
        return;
    }
    storeState(Applied);
    SYSLOG(MODULE_SHORT,
           "Sequoia 24A335 UVCFamily PROBE recovery applied (Frame4->Frame1 one-shot fallback) in shared cache page 0x%llx",
           static_cast<unsigned long long>(pageOffset));
}

static void patcherLoaded(void *, KernelPatcher &patcher) {
    if (!UVCSequoiaProbePatch::isSupportedDarwinVersion(
            getKernelVersion(), getKernelMinorVersion())) {
        storeState(Failed);
        SYSLOG(MODULE_SHORT, "unsupported Darwin version for Sequoia 24A335 fingerprint; inactive");
        return;
    }
    KernelPatcher::RouteRequest request {
        "_cs_validate_page", patchedCsValidatePage, originalCsValidatePage
    };
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1)) {
        patcher.clearError();
        originalCsValidatePage = 0;
        storeState(Failed);
        SYSLOG(MODULE_SHORT, "Sequoia shared-cache validator route failed; inactive");
        return;
    }
    SYSLOG(MODULE_SHORT, "Sequoia 24A335 route installed; waiting for exact UVCFamily PROBE method");
}
}

bool start() {
    if (!UVCSequoiaProbePatch::isSupportedDarwinVersion(
            getKernelVersion(), getKernelMinorVersion())) {
        SYSLOG(MODULE_SHORT, "Sequoia build unsupported; no patch installed");
        return false;
    }
    storeState(Waiting);
    originalCsValidatePage = 0;
    const auto error = lilu.onPatcherLoad(patcherLoaded, nullptr);
    if (error != LiluAPI::Error::NoError) {
        storeState(Failed);
        SYSLOG(MODULE_SHORT, "Sequoia failed to register Lilu patcher callback (%d)",
               static_cast<int>(error));
        return false;
    }
    return true;
}
}
