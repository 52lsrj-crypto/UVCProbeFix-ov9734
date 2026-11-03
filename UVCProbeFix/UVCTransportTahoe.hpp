#pragma once

#include <stddef.h>
#include <stdint.h>

#include "UVCDescriptorParser.hpp"
#include "UVCProbeEngine.hpp"
#include "UVCRecoveryContext.hpp"
#include "UVCRuntimeMode.hpp"

namespace UVC {

struct ControlRequestView {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};

struct InterfaceView {
    uint8_t interfaceClass;
    uint8_t interfaceSubClass;
    uint8_t interfaceNumber;
};

enum class RequestKind : uint8_t {
    Other,
    SetProbe,
    GetProbe,
    CommitProbe
};

RequestKind classifyRequest(const InterfaceView &interfaceView,
                            const ControlRequestView &request,
                            size_t dataLength,
                            bool &mutationLengthSupported);

// Exact Itanium/Mach-O symbol for the synchronous raw-buffer overload declared
// by the pinned MacKernelSDK IOUSBHostInterface header:
// IOReturn deviceRequest(DeviceRequest&, void*, uint32_t&, uint32_t).
extern const char kTahoeSyncDeviceRequestSymbol[];
bool isAcceptedTahoeDeviceRequestSymbol(const char *symbol);
bool isSupportedTahoeDarwinVersion(uint32_t major, uint32_t minor);
UsbSpeed mapTahoeUsbSpeed(uint8_t speed);
bool isCompleteProbeTransfer(size_t requestedLength, uint32_t bytesTransferred);
bool shouldBypassDuringInternal(RequestKind kind);
bool snapshotExternalSetProbe(RequestKind kind,
                              bool mutationLengthSupported,
                              const uint8_t *bytes,
                              size_t length,
                              ProbePacket &snapshot);

struct ContextLockOps {
    void *context;
    void (*lock)(void *);
    void (*unlock)(void *);
};

struct TransportEnvironment {
    const void *interfaceKey;
    DeviceIdentity identity;
    InterfaceView interfaceView;
    const uint8_t *configuration;
    size_t configurationLength;
    UsbSpeed speed;
};

struct TransportCounters {
    uint64_t probesObserved;
    uint64_t validPassThrough;
    uint64_t descriptorParseFailures;
    uint64_t fieldRepairs;
    uint64_t equivalentRetries;
    uint64_t equivalentRetrySuccesses;
    uint64_t equivalentRetryFailures;
    uint64_t noSafeCandidate;
    uint64_t reentrancyBypass;
};

struct CompletedGetResult {
    RecoveryAction action;
    FieldStatus validation;
    bool mutated;
    bool internalRetryAttempted;
    bool internalRetrySucceeded;
    bool staleDiscarded;
};

// Pure coordinator shared by host tests and the Tahoe kernel adapter.  The
// caller owns the lock; this object never holds it across descriptor parsing
// or transport callbacks.
class TransportCoordinator {
public:
    TransportCoordinator();

    void reset(const ContextLockOps &lockOps);
    bool recordExternalSet(const TransportEnvironment &environment,
                           const uint8_t *probeBytes,
                           size_t probeLength,
                           const ContextLockOps &lockOps);
    bool isInternal(const TransportEnvironment &environment,
                    const ContextLockOps &lockOps) const;
    CompletedGetResult processCompletedGet(RuntimeMode mode,
                                           const TransportEnvironment &environment,
                                           uint8_t *responseBytes,
                                           size_t responseLength,
                                           const ProbeTransportOps &transport,
                                           const ContextLockOps &lockOps);
    void noteReentrancyBypass(const ContextLockOps &lockOps);
    TransportCounters snapshotCounters(const ContextLockOps &lockOps) const;

private:
    RecoveryContextTable contexts_;
    TransportCounters counters_;

    static bool validLockOps(const ContextLockOps &lockOps);
    static bool validEnvironment(const TransportEnvironment &environment);
};

// Kernel-facing adapter.  Host builds intentionally return false; the real
// implementation is compiled only for the kext target via UVCP_KERNEL_BUILD.
bool startTahoeTransport(RuntimeMode mode);

} // namespace UVC
