//
// UVCProbeFix 0.3.1 release-candidate dispatcher
// Copyright © 2026 DNMTechLabs. All rights reserved.
// Contact: dnmtechlabs@gmail.com
//
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>

#include "UVCLegacy0215.hpp"
#include "UVC15ProbeOverrun.hpp"
#include "UVCSequoia.hpp" // SEQUOIA_INTEGRATION_INCLUDE
#if !defined(UVCP_RELEASE_LEGACY_ONLY)
#include "UVCRuntimeMode.hpp"
#include "UVCTransportTahoe.hpp"
#endif

#define MODULE_SHORT "uvcp"

namespace {

#if !defined(UVCP_RELEASE_LEGACY_ONLY)
// This stays false until the generic Tahoe transport adapter passes the
// hardware gates defined by the 0.3.0 implementation plan.
static constexpr bool kGenericDefaultEnabled = false;
#endif

static void pluginStart() {
    // SEQUOIA_INTEGRATION_BEGIN
    if (getKernelVersion() == KernelVersion::Sequoia) {
        if (!UVCSequoia::start()) {
            SYSLOG(MODULE_SHORT, "Sequoia PROBE recovery inactive");
        }
        return;
    }
    // SEQUOIA_INTEGRATION_END
#if defined(UVCP_RELEASE_LEGACY_ONLY)
    if (!UVC15::start()) SYSLOG(MODULE_SHORT, "HP UVC1.5 module inactive");
    // Production Release deliberately contains only the known-good 0.2.15
    // shared-cache recovery implementation. Generic transport/engine code is
    // built only in Debug/Test until both physical hardware gates pass.
    if (!UVCLegacy0215::start()) {
        SYSLOG(MODULE_SHORT, "0.2.15 legacy recovery failed to start");
    }
#else
    const UVC::BootArgState args {
        checkKernelArgument("-uvcpoff"),
        checkKernelArgument("-uvcplegacy"),
        checkKernelArgument("-uvcpobserve"),
        checkKernelArgument("-uvcpfield"),
        checkKernelArgument("-uvcprecover")
    };
#ifdef DEBUG
    constexpr bool debugBuild = true;
#else
    constexpr bool debugBuild = false;
#endif
    const UVC::RuntimeMode mode = UVC::selectRuntimeMode(args, debugBuild, kGenericDefaultEnabled);
    if (mode != UVC::RuntimeMode::Off && !UVC15::start()) {
        SYSLOG(MODULE_SHORT, "HP UVC1.5 module inactive");
    }
    switch (mode) {
        case UVC::RuntimeMode::Off:
            DBGLOG(MODULE_SHORT, "runtime mode Off; plugin inactive");
            return;
        case UVC::RuntimeMode::Legacy0215:
            if (!UVCLegacy0215::start()) {
                SYSLOG(MODULE_SHORT, "0.2.15 legacy recovery failed to start");
            }
            return;
        case UVC::RuntimeMode::GenericObserve:
        case UVC::RuntimeMode::GenericFieldRepairDev:
        case UVC::RuntimeMode::GenericFull:
            if (!UVC::startTahoeTransport(mode)) {
                SYSLOG(MODULE_SHORT, "generic Tahoe transport failed to start; plugin inactive fail-closed");
            }
            return;
    }
#endif
}

static const char *bootargOff[] { "-uvcpoff" };
static const char *bootargDebug[] { "-uvcpdbg" };
static const char *bootargBeta[] { "-uvcpbeta" };

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
