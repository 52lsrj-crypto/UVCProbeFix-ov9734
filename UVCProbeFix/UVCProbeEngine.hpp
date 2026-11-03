#pragma once

#include <stdint.h>

#include "UVCDescriptorParser.hpp"
#include "UVCProbeControl.hpp"

namespace UVC {

enum class FieldStatus : uint8_t { Valid, Repairable, Unknown };

enum InvalidField : uint32_t {
    InvalidNone      = 0U,
    InvalidFormat    = 1U << 0,
    InvalidFrame     = 1U << 1,
    InvalidInterval  = 1U << 2,
    InvalidFrameSize = 1U << 3,
    InvalidPayload   = 1U << 4
};

struct ValidationReport {
    FieldStatus overall;
    uint32_t invalidMask;
    FrameInfo resolvedFrame;
    uint32_t provenFrameSize;
    uint32_t provenMaxPayload;
    bool maxPayloadKnown;
};

ValidationReport validateProbe(const DescriptorView &view,
                               const ProbePacket &lastSet,
                               const ProbePacket &response);

enum class RecoveryAction : uint8_t { PassThrough, FieldRepair, RetryEquivalent };

enum class TransportStatus : uint8_t { Success, Error };

struct ProbeTransportOps {
    void *context;
    TransportStatus (*setProbe)(void *, const uint8_t *, size_t);
    TransportStatus (*getProbe)(void *, uint8_t *, size_t, size_t *);
};

struct RecoveryResult {
    RecoveryAction action;
    ProbePacket callerVisible;
    bool mutated;
    bool internalRetryAttempted;
    bool internalRetrySucceeded;
};

RecoveryResult executeRecovery(const DescriptorView &view,
                               const ProbePacket &lastSet,
                               const ProbePacket &originalResponse,
                               const ProbeTransportOps &transport);

bool buildEquivalentRetrySet(const ProbePacket &lastSet,
                             const FrameInfo &candidate,
                             uint32_t provenFrameSize,
                             ProbePacket &retrySet);

RecoveryAction chooseRecoveryAction(const DescriptorView &view,
                                    const ProbePacket &lastSet,
                                    const ProbePacket &response,
                                    const ValidationReport &report,
                                    FrameInfo *candidateOut);

bool buildFieldRepair(const DescriptorView &view,
                      const ProbePacket &response,
                      const ValidationReport &report,
                      ProbePacket &repaired);

} // namespace UVC
