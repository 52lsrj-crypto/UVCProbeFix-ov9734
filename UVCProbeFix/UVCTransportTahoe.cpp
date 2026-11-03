#include "UVCTransportTahoe.hpp"

#include <string.h>

#ifdef UVCP_KERNEL_BUILD
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLocks.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#endif

namespace UVC {
namespace {
constexpr uint8_t kVideoClass = 14U;
constexpr uint8_t kVideoStreamingSubclass = 2U;
constexpr uint8_t kSetClassInterface = 0x21U;
constexpr uint8_t kGetClassInterface = 0xA1U;
constexpr uint8_t kSetCur = 0x01U;
constexpr uint8_t kGetCur = 0x81U;
constexpr uint8_t kProbeSelector = 0x01U;
constexpr uint8_t kCommitSelector = 0x02U;

bool canonicalLength(size_t length) {
    return length == 26U || length == 34U || length == 48U;
}

CycleState terminalState(const ValidationReport &report, const RecoveryResult &result) {
    if (result.mutated || result.internalRetrySucceeded) return CycleState::Recovered;
    if (report.overall == FieldStatus::Valid) return CycleState::Idle;
    return CycleState::Failed;
}
} // namespace

const char kTahoeSyncDeviceRequestSymbol[] =
    "__ZN18IOUSBHostInterface13deviceRequestERN11StandardUSB13DeviceRequestEPvRjj";

bool isAcceptedTahoeDeviceRequestSymbol(const char *symbol) {
    return symbol != nullptr && strcmp(symbol, kTahoeSyncDeviceRequestSymbol) == 0;
}

bool isSupportedTahoeDarwinVersion(uint32_t major, uint32_t minor) {
    return major == 25U && minor == 6U;
}

UsbSpeed mapTahoeUsbSpeed(uint8_t speed) {
    // IOUSBHostFamily tInternalUSBHostConnectionSpeed: Full=1, High=2,
    // Super=3, SuperPlus=4, SuperPlusBy2=5. 0.3.1 does not parse the
    // SuperSpeedPlus isochronous companion descriptor, so those modes must
    // remain capacity-unknown rather than being treated as plain SuperSpeed.
    if (speed == 1U) return UsbSpeed::Full;
    if (speed == 2U) return UsbSpeed::High;
    if (speed == 3U) return UsbSpeed::Super;
    return UsbSpeed::Unknown;
}

bool isCompleteProbeTransfer(size_t requestedLength, uint32_t bytesTransferred) {
    return isCanonicalProbeLength(requestedLength) &&
           requestedLength == static_cast<size_t>(bytesTransferred);
}

bool shouldBypassDuringInternal(RequestKind kind) {
    // A new external SET must always be allowed through so it can advance the
    // sequence and invalidate an older recovery. If an unexpectedly recursive
    // internal SET reaches the hook, treating it as a new cycle is fail-closed.
    return kind != RequestKind::SetProbe;
}

bool snapshotExternalSetProbe(RequestKind kind,
                              bool mutationLengthSupported,
                              const uint8_t *bytes,
                              size_t length,
                              ProbePacket &snapshot) {
    if (kind != RequestKind::SetProbe || !mutationLengthSupported) return false;
    return parseProbePacket(bytes, length, snapshot) == ProbeParseStatus::Ok;
}

RequestKind classifyRequest(const InterfaceView &interfaceView,
                            const ControlRequestView &request,
                            size_t dataLength,
                            bool &mutationLengthSupported) {
    mutationLengthSupported = false;
    if (interfaceView.interfaceClass != kVideoClass ||
        interfaceView.interfaceSubClass != kVideoStreamingSubclass) {
        return RequestKind::Other;
    }
    if (dataLength < 26U || dataLength > 0xffffU ||
        request.wLength != static_cast<uint16_t>(dataLength) ||
        request.wIndex != static_cast<uint16_t>(interfaceView.interfaceNumber)) {
        return RequestKind::Other;
    }

    if ((request.wValue & 0x00ffU) != 0U) return RequestKind::Other;
    const uint8_t selector = static_cast<uint8_t>((request.wValue >> 8U) & 0xffU);
    const bool isSet = request.bmRequestType == kSetClassInterface && request.bRequest == kSetCur;
    const bool isGet = request.bmRequestType == kGetClassInterface && request.bRequest == kGetCur;
    if (!isSet && !isGet) return RequestKind::Other;

    mutationLengthSupported = canonicalLength(dataLength);
    if (selector == kProbeSelector) return isSet ? RequestKind::SetProbe : RequestKind::GetProbe;
    if (selector == kCommitSelector) return RequestKind::CommitProbe;
    mutationLengthSupported = false;
    return RequestKind::Other;
}

TransportCoordinator::TransportCoordinator() : contexts_(), counters_{} {}

bool TransportCoordinator::validLockOps(const ContextLockOps &lockOps) {
    return lockOps.lock != nullptr && lockOps.unlock != nullptr;
}

bool TransportCoordinator::validEnvironment(const TransportEnvironment &environment) {
    return environment.interfaceKey != nullptr && environment.configuration != nullptr &&
           environment.configurationLength >= 9U &&
           environment.interfaceView.interfaceClass == kVideoClass &&
           environment.interfaceView.interfaceSubClass == kVideoStreamingSubclass &&
           environment.identity.interfaceNumber == environment.interfaceView.interfaceNumber;
}

void TransportCoordinator::reset(const ContextLockOps &lockOps) {
    if (!validLockOps(lockOps)) return;
    lockOps.lock(lockOps.context);
    contexts_.reset();
    memset(&counters_, 0, sizeof(counters_));
    lockOps.unlock(lockOps.context);
}

bool TransportCoordinator::recordExternalSet(const TransportEnvironment &environment,
                                             const uint8_t *probeBytes,
                                             size_t probeLength,
                                             const ContextLockOps &lockOps) {
    if (!validLockOps(lockOps) || !validEnvironment(environment)) return false;
    ProbePacket packet {};
    if (parseProbePacket(probeBytes, probeLength, packet) != ProbeParseStatus::Ok) return false;
    ContextTicket ignored {};
    lockOps.lock(lockOps.context);
    const bool recorded = contexts_.recordExternalSet(environment.interfaceKey, environment.identity, packet, ignored);
    lockOps.unlock(lockOps.context);
    return recorded;
}

bool TransportCoordinator::isInternal(const TransportEnvironment &environment,
                                      const ContextLockOps &lockOps) const {
    if (!validLockOps(lockOps) || !validEnvironment(environment)) return false;
    lockOps.lock(lockOps.context);
    const bool internal = contexts_.isInternal(environment.interfaceKey, environment.identity);
    lockOps.unlock(lockOps.context);
    return internal;
}

void TransportCoordinator::noteReentrancyBypass(const ContextLockOps &lockOps) {
    if (!validLockOps(lockOps)) return;
    lockOps.lock(lockOps.context);
    ++counters_.reentrancyBypass;
    lockOps.unlock(lockOps.context);
}

TransportCounters TransportCoordinator::snapshotCounters(const ContextLockOps &lockOps) const {
    TransportCounters result {};
    if (!validLockOps(lockOps)) return result;
    lockOps.lock(lockOps.context);
    result = counters_;
    lockOps.unlock(lockOps.context);
    return result;
}

CompletedGetResult TransportCoordinator::processCompletedGet(RuntimeMode mode,
                                                             const TransportEnvironment &environment,
                                                             uint8_t *responseBytes,
                                                             size_t responseLength,
                                                             const ProbeTransportOps &transport,
                                                             const ContextLockOps &lockOps) {
    CompletedGetResult result {};
    result.action = RecoveryAction::PassThrough;
    result.validation = FieldStatus::Unknown;

    if (!validLockOps(lockOps) || !validEnvironment(environment) || responseBytes == nullptr ||
        !isCanonicalProbeLength(responseLength) ||
        (mode != RuntimeMode::GenericObserve && mode != RuntimeMode::GenericFieldRepairDev &&
         mode != RuntimeMode::GenericFull)) {
        return result;
    }

    ProbePacket response {};
    if (parseProbePacket(responseBytes, responseLength, response) != ProbeParseStatus::Ok) return result;

    ContextTicket ticket {};
    lockOps.lock(lockOps.context);
    ++counters_.probesObserved;
    const bool haveTicket = contexts_.snapshotForGet(environment.interfaceKey, environment.identity, ticket);
    lockOps.unlock(lockOps.context);
    if (!haveTicket) return result;

    DescriptorView view;
    const DescriptorStatus descriptorStatus = view.init(environment.configuration,
                                                        environment.configurationLength,
                                                        environment.interfaceView.interfaceNumber,
                                                        environment.speed);
    if (descriptorStatus != DescriptorStatus::Ok) {
        lockOps.lock(lockOps.context);
        if (contexts_.isCurrent(ticket)) {
            ++counters_.descriptorParseFailures;
            contexts_.finish(ticket, CycleState::Failed);
        }
        lockOps.unlock(lockOps.context);
        return result;
    }

    const ValidationReport report = validateProbe(view, ticket.lastSet, response);
    result.validation = report.overall;
    FrameInfo candidate {};
    result.action = chooseRecoveryAction(view, ticket.lastSet, response, report, &candidate);

    if (mode == RuntimeMode::GenericObserve) {
        lockOps.lock(lockOps.context);
        if (contexts_.isCurrent(ticket)) {
            if (report.overall == FieldStatus::Valid) ++counters_.validPassThrough;
            else ++counters_.noSafeCandidate;
            contexts_.finish(ticket, report.overall == FieldStatus::Valid ? CycleState::Idle : CycleState::Failed);
        }
        lockOps.unlock(lockOps.context);
        return result;
    }

    if (mode == RuntimeMode::GenericFieldRepairDev) {
        if (result.action != RecoveryAction::FieldRepair) {
            lockOps.lock(lockOps.context);
            if (contexts_.isCurrent(ticket)) {
                if (report.overall == FieldStatus::Valid) ++counters_.validPassThrough;
                else ++counters_.noSafeCandidate;
                contexts_.finish(ticket, report.overall == FieldStatus::Valid ? CycleState::Idle : CycleState::Failed);
            }
            lockOps.unlock(lockOps.context);
            return result;
        }

        ProbePacket repaired {};
        if (!buildFieldRepair(view, response, report, repaired)) return result;
        lockOps.lock(lockOps.context);
        if (!contexts_.isCurrent(ticket)) {
            result.staleDiscarded = true;
            lockOps.unlock(lockOps.context);
            return result;
        }
        if (!copyProbePacket(repaired, responseBytes, responseLength)) {
            contexts_.finish(ticket, CycleState::Failed);
            lockOps.unlock(lockOps.context);
            return result;
        }
        result.mutated = memcmp(repaired.bytes, response.bytes, response.length) != 0;
        if (result.mutated) ++counters_.fieldRepairs;
        contexts_.finish(ticket, result.mutated ? CycleState::Recovered : CycleState::Failed);
        lockOps.unlock(lockOps.context);
        return result;
    }

    // A retry must use the same control-block length as the caller's GET.
    // Otherwise a valid shorter reply cannot be copied back, while the
    // internal SET would still change the device's negotiated state.
    if (result.action == RecoveryAction::RetryEquivalent &&
        ticket.lastSet.length != response.length) {
        lockOps.lock(lockOps.context);
        if (contexts_.isCurrent(ticket)) {
            ++counters_.noSafeCandidate;
            contexts_.finish(ticket, CycleState::Failed);
        } else {
            result.staleDiscarded = true;
        }
        lockOps.unlock(lockOps.context);
        result.action = RecoveryAction::PassThrough;
        return result;
    }

    bool internalMarked = false;
    if (result.action == RecoveryAction::RetryEquivalent) {
        lockOps.lock(lockOps.context);
        if (contexts_.isCurrent(ticket)) {
            internalMarked = contexts_.setInternal(ticket, true);
            if (internalMarked) ++counters_.equivalentRetries;
        }
        lockOps.unlock(lockOps.context);
        if (!internalMarked) {
            result.staleDiscarded = true;
            return result;
        }
    }

    const RecoveryResult recovery = executeRecovery(view, ticket.lastSet, response, transport);
    result.action = recovery.action;
    result.internalRetryAttempted = recovery.internalRetryAttempted;
    result.internalRetrySucceeded = recovery.internalRetrySucceeded;

    lockOps.lock(lockOps.context);
    if (!contexts_.isCurrent(ticket)) {
        result.staleDiscarded = true;
        lockOps.unlock(lockOps.context);
        return result;
    }

    if (recovery.mutated && recovery.callerVisible.length == response.length &&
        copyProbePacket(recovery.callerVisible, responseBytes, responseLength)) {
        result.mutated = true;
    }

    if (recovery.action == RecoveryAction::PassThrough && report.overall == FieldStatus::Valid) {
        ++counters_.validPassThrough;
    } else if (recovery.action == RecoveryAction::FieldRepair && result.mutated) {
        ++counters_.fieldRepairs;
    } else if (recovery.action == RecoveryAction::RetryEquivalent) {
        if (recovery.internalRetrySucceeded && result.mutated) ++counters_.equivalentRetrySuccesses;
        else ++counters_.equivalentRetryFailures;
    } else if (report.overall != FieldStatus::Valid) {
        ++counters_.noSafeCandidate;
    }

    contexts_.finish(ticket, terminalState(report, recovery));
    lockOps.unlock(lockOps.context);
    return result;
}

#ifndef UVCP_KERNEL_BUILD
bool startTahoeTransport(RuntimeMode) {
    return false;
}
#else
namespace {
#define UVCP_TAHOE_LOG "uvcp"

using DeviceRequestRawFn = IOReturn (*)(IOUSBHostInterface *,
                                        StandardUSB::DeviceRequest &,
                                        void *,
                                        uint32_t &,
                                        uint32_t);

static DeviceRequestRawFn gOriginalDeviceRequest {nullptr};
static RuntimeMode gRuntimeMode {RuntimeMode::Off};
static IOLock *gContextLock {nullptr};
static TransportCoordinator gCoordinator;
static bool gRouteInstalled {false};

static const char *gIOUSBHostFamilyPaths[] {
    "/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily"
};
static KernelPatcher::KextInfo gIOUSBHostFamily {
    "com.apple.iokit.IOUSBHostFamily",
    gIOUSBHostFamilyPaths,
    arrsize(gIOUSBHostFamilyPaths),
    {true},
    {},
    KernelPatcher::KextInfo::Unloaded
};

void kernelLock(void *context) {
    if (context != nullptr) IOLockLock(static_cast<IOLock *>(context));
}

void kernelUnlock(void *context) {
    if (context != nullptr) IOLockUnlock(static_cast<IOLock *>(context));
}

ContextLockOps kernelLockOps() {
    return ContextLockOps{gContextLock, kernelLock, kernelUnlock};
}

void disableUnroutedTransport() {
    gOriginalDeviceRequest = nullptr;
    gRuntimeMode = RuntimeMode::Off;
    if (gContextLock != nullptr) {
        IOLockFree(gContextLock);
        gContextLock = nullptr;
    }
}

bool snapshotEnvironment(IOUSBHostInterface *interface, TransportEnvironment &environment) {
    if (interface == nullptr) return false;
    const auto *interfaceDescriptor = interface->getInterfaceDescriptor();
    const auto *configuration = interface->getConfigurationDescriptor();
    IOUSBHostDevice *device = interface->getDevice();
    const auto *deviceDescriptor = device != nullptr ? device->getDeviceDescriptor() : nullptr;
    if (interfaceDescriptor == nullptr || configuration == nullptr || device == nullptr || deviceDescriptor == nullptr) {
        return false;
    }

    const uint16_t totalLength = USBToHost16(configuration->wTotalLength);
    if (totalLength < 9U || totalLength > 4096U) return false;

    environment.interfaceKey = interface;
    environment.identity.vendorId = USBToHost16(deviceDescriptor->idVendor);
    environment.identity.productId = USBToHost16(deviceDescriptor->idProduct);
    environment.identity.bcdDevice = USBToHost16(deviceDescriptor->bcdDevice);
    environment.identity.configurationValue = configuration->bConfigurationValue;
    environment.identity.interfaceNumber = interfaceDescriptor->bInterfaceNumber;
    environment.interfaceView.interfaceClass = interfaceDescriptor->bInterfaceClass;
    environment.interfaceView.interfaceSubClass = interfaceDescriptor->bInterfaceSubClass;
    environment.interfaceView.interfaceNumber = interfaceDescriptor->bInterfaceNumber;
    environment.configuration = reinterpret_cast<const uint8_t *>(configuration);
    environment.configurationLength = totalLength;
    environment.speed = mapTahoeUsbSpeed(device->getSpeed());
    return true;
}

ControlRequestView requestView(const StandardUSB::DeviceRequest &request) {
    return ControlRequestView{request.bmRequestType, request.bRequest, request.wValue,
                              request.wIndex, request.wLength};
}

struct KernelProbeTransportContext {
    IOUSBHostInterface *interface;
    uint8_t interfaceNumber;
    uint8_t probeLength;
    uint32_t timeoutMs;
};

TransportStatus internalSetProbe(void *context, const uint8_t *bytes, size_t length) {
    auto *state = static_cast<KernelProbeTransportContext *>(context);
    if (state == nullptr || state->interface == nullptr || gOriginalDeviceRequest == nullptr ||
        bytes == nullptr || !isCanonicalProbeLength(length) || length > 0xffU) {
        return TransportStatus::Error;
    }
    StandardUSB::DeviceRequest request {};
    request.bmRequestType = kSetClassInterface;
    request.bRequest = kSetCur;
    request.wValue = static_cast<uint16_t>(kProbeSelector) << 8U;
    request.wIndex = state->interfaceNumber;
    request.wLength = static_cast<uint16_t>(length);
    uint32_t transferred = 0U;
    const IOReturn ret = gOriginalDeviceRequest(state->interface, request,
                                                 const_cast<uint8_t *>(bytes),
                                                 transferred, state->timeoutMs);
    if (ret != kIOReturnSuccess || !isCompleteProbeTransfer(length, transferred)) {
        return TransportStatus::Error;
    }
    state->probeLength = static_cast<uint8_t>(length);
    return TransportStatus::Success;
}

TransportStatus internalGetProbe(void *context, uint8_t *bytes, size_t capacity, size_t *actualLength) {
    auto *state = static_cast<KernelProbeTransportContext *>(context);
    if (state == nullptr || state->interface == nullptr || gOriginalDeviceRequest == nullptr ||
        bytes == nullptr || actualLength == nullptr || !isCanonicalProbeLength(state->probeLength) ||
        capacity < state->probeLength) {
        return TransportStatus::Error;
    }
    StandardUSB::DeviceRequest request {};
    request.bmRequestType = kGetClassInterface;
    request.bRequest = kGetCur;
    request.wValue = static_cast<uint16_t>(kProbeSelector) << 8U;
    request.wIndex = state->interfaceNumber;
    request.wLength = state->probeLength;
    uint32_t transferred = 0U;
    const IOReturn ret = gOriginalDeviceRequest(state->interface, request, bytes, transferred, state->timeoutMs);
    if (ret != kIOReturnSuccess) return TransportStatus::Error;
    *actualLength = transferred;
    return TransportStatus::Success;
}

IOReturn deviceRequestHook(IOUSBHostInterface *interface,
                           StandardUSB::DeviceRequest &request,
                           void *dataBuffer,
                           uint32_t &bytesTransferred,
                           uint32_t completionTimeoutMs) {
    if (gOriginalDeviceRequest == nullptr) return kIOReturnUnsupported;

    TransportEnvironment environment {};
    if (!snapshotEnvironment(interface, environment)) {
        return gOriginalDeviceRequest(interface, request, dataBuffer, bytesTransferred, completionTimeoutMs);
    }

    const size_t requestedLength = request.wLength;
    bool mutationLengthSupported = false;
    const RequestKind kind = classifyRequest(environment.interfaceView, requestView(request),
                                             requestedLength, mutationLengthSupported);
    if (kind == RequestKind::Other || dataBuffer == nullptr) {
        return gOriginalDeviceRequest(interface, request, dataBuffer, bytesTransferred, completionTimeoutMs);
    }

    const bool internalRequest = gCoordinator.isInternal(environment, kernelLockOps());
    if (internalRequest && shouldBypassDuringInternal(kind)) {
        gCoordinator.noteReentrancyBypass(kernelLockOps());
        return gOriginalDeviceRequest(interface, request, dataBuffer, bytesTransferred, completionTimeoutMs);
    }

    ProbePacket externalSetSnapshot {};
    const bool haveExternalSetSnapshot = snapshotExternalSetProbe(
        kind, mutationLengthSupported, static_cast<const uint8_t *>(dataBuffer),
        requestedLength, externalSetSnapshot);

    const IOReturn originalResult =
        gOriginalDeviceRequest(interface, request, dataBuffer, bytesTransferred, completionTimeoutMs);
    if (originalResult != kIOReturnSuccess) return originalResult;

    if (!mutationLengthSupported) return originalResult;

    if (kind == RequestKind::SetProbe) {
        if (!isCompleteProbeTransfer(requestedLength, bytesTransferred)) return originalResult;
        if (haveExternalSetSnapshot) {
            gCoordinator.recordExternalSet(environment, externalSetSnapshot.bytes,
                                           externalSetSnapshot.length, kernelLockOps());
        }
        return originalResult;
    }

    if (kind == RequestKind::CommitProbe) {
        return originalResult;
    }

    if (kind == RequestKind::GetProbe &&
        isCompleteProbeTransfer(requestedLength, bytesTransferred)) {
        KernelProbeTransportContext transportContext {interface, environment.interfaceView.interfaceNumber,
                                                       static_cast<uint8_t>(bytesTransferred), completionTimeoutMs};
        ProbeTransportOps transport {&transportContext, internalSetProbe, internalGetProbe};
        const CompletedGetResult recovery = gCoordinator.processCompletedGet(
            gRuntimeMode, environment, static_cast<uint8_t *>(dataBuffer), bytesTransferred,
            transport, kernelLockOps());
        if (recovery.internalRetrySucceeded) {
            DBGLOG(UVCP_TAHOE_LOG, "generic equivalent-frame recovery succeeded on interface %u",
                   environment.interfaceView.interfaceNumber);
        } else if (recovery.mutated && recovery.action == RecoveryAction::FieldRepair) {
            DBGLOG(UVCP_TAHOE_LOG, "generic field repair applied on interface %u",
                   environment.interfaceView.interfaceNumber);
        }
    }
    return originalResult;
}

void onIOUSBHostFamilyLoad(void *, KernelPatcher &patcher, size_t id, mach_vm_address_t slide, size_t size) {
    if (gRouteInstalled || size == 0U) return;

    const mach_vm_address_t exact = patcher.solveSymbol(id, kTahoeSyncDeviceRequestSymbol, slide, size, false);
    if (exact == 0U) {
        patcher.clearError();
        disableUnroutedTransport();
        SYSLOG(UVCP_TAHOE_LOG, "exact synchronous IOUSBHostInterface::deviceRequest symbol unavailable; generic mode disabled");
        return;
    }

    KernelPatcher::RouteRequest request {kTahoeSyncDeviceRequestSymbol, deviceRequestHook, gOriginalDeviceRequest};
    if (!patcher.routeMultiple(id, &request, 1U, slide, size, true, false) ||
        gOriginalDeviceRequest == nullptr) {
        patcher.clearError();
        disableUnroutedTransport();
        SYSLOG(UVCP_TAHOE_LOG, "failed to route exact synchronous deviceRequest; generic mode disabled");
        return;
    }

    gRouteInstalled = true;
    SYSLOG(UVCP_TAHOE_LOG, "generic Tahoe UVC transport route installed");
}
} // namespace

bool startTahoeTransport(RuntimeMode mode) {
    if (mode != RuntimeMode::GenericObserve && mode != RuntimeMode::GenericFieldRepairDev &&
        mode != RuntimeMode::GenericFull) {
        return false;
    }
    if (!isSupportedTahoeDarwinVersion(static_cast<uint32_t>(getKernelVersion()),
                                       static_cast<uint32_t>(getKernelMinorVersion()))) {
        SYSLOG(UVCP_TAHOE_LOG, "generic transport supports Darwin 25.6 only; plugin inactive fail-closed");
        return false;
    }
    if (gContextLock == nullptr) {
        gContextLock = IOLockAlloc();
        if (gContextLock == nullptr) {
            SYSLOG(UVCP_TAHOE_LOG, "failed to allocate transport context lock");
            return false;
        }
    }
    gRuntimeMode = mode;
    gCoordinator.reset(kernelLockOps());
    const auto error = lilu.onKextLoad(&gIOUSBHostFamily, 1U, onIOUSBHostFamilyLoad, nullptr);
    if (error != LiluAPI::Error::NoError) {
        disableUnroutedTransport();
        SYSLOG(UVCP_TAHOE_LOG, "failed to register IOUSBHostFamily load callback: %d", static_cast<int>(error));
        return false;
    }
    return true;
}
#endif

} // namespace UVC
