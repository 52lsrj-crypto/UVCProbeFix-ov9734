#include "UVCProbeEngine.hpp"

#include <string.h>

namespace UVC {
namespace {
bool proveUncompressedFrameSize(const FrameInfo &frame, uint32_t &out) {
    if (frame.kind != FormatKind::Uncompressed) return false;
    if (frame.maxFrameBufferSize != 0U) {
        out = frame.maxFrameBufferSize;
        return true;
    }
    if (frame.width == 0U || frame.height == 0U || frame.bitsPerPixel == 0U || (frame.bitsPerPixel % 8U) != 0U) {
        return false;
    }
    const uint64_t bytesPerPixel = static_cast<uint64_t>(frame.bitsPerPixel / 8U);
    const uint64_t total = static_cast<uint64_t>(frame.width) * static_cast<uint64_t>(frame.height) * bytesPerPixel;
    if (total == 0U || total > UINT32_MAX) return false;
    out = static_cast<uint32_t>(total);
    return true;
}

void markUnknown(ValidationReport &r, uint32_t field, bool &unknown) {
    r.invalidMask |= field;
    unknown = true;
}

void markRepairable(ValidationReport &r, uint32_t field, bool &repairable) {
    r.invalidMask |= field;
    repairable = true;
}
} // namespace

ValidationReport validateProbe(const DescriptorView &view,
                               const ProbePacket &lastSet,
                               const ProbePacket &response) {
    ValidationReport report {};
    report.overall = FieldStatus::Unknown;
    report.invalidMask = InvalidNone;
    report.maxPayloadKnown = false;
    bool unknown = false;
    bool repairable = false;

    FrameInfo frame {};
    const DescriptorStatus frameStatus = view.findFrame(response.formatIndex(), response.frameIndex(), frame);
    if (frameStatus != DescriptorStatus::Ok) {
        if (response.formatIndex() == lastSet.formatIndex()) markUnknown(report, InvalidFrame, unknown);
        else markUnknown(report, InvalidFormat, unknown);
    } else {
        report.resolvedFrame = frame;
        if (view.supportsInterval(frame, response.frameInterval()) != DescriptorStatus::Ok) {
            markUnknown(report, InvalidInterval, unknown);
        }

        if (frame.kind == FormatKind::Uncompressed) {
            uint32_t proven = 0U;
            if (!proveUncompressedFrameSize(frame, proven)) {
                markUnknown(report, InvalidFrameSize, unknown);
            } else {
                report.provenFrameSize = proven;
                if (response.maxVideoFrameSize() != proven) {
                    markRepairable(report, InvalidFrameSize, repairable);
                }
            }
        } else if (frame.kind == FormatKind::Mjpeg || frame.kind == FormatKind::FrameBased) {
            if (response.maxVideoFrameSize() == 0U) {
                markUnknown(report, InvalidFrameSize, unknown);
            }
        } else {
            markUnknown(report, InvalidFormat, unknown);
        }
    }

    uint32_t maximumPayload = 0U;
    if (view.maxPeriodicPayload(maximumPayload) == DescriptorStatus::Ok && maximumPayload != 0U) {
        report.maxPayloadKnown = true;
        report.provenMaxPayload = maximumPayload;
        const uint32_t payload = response.maxPayloadTransferSize();
        if (payload == 0U) {
            markUnknown(report, InvalidPayload, unknown);
        } else if ((payload & 0xffff0000U) == 0xffff0000U) {
            const uint32_t low = payload & 0xffffU;
            if (low != 0U && low <= maximumPayload) {
                markRepairable(report, InvalidPayload, repairable);
            } else {
                markUnknown(report, InvalidPayload, unknown);
            }
        } else if (payload > maximumPayload) {
            markRepairable(report, InvalidPayload, repairable);
        }
    } else {
        markUnknown(report, InvalidPayload, unknown);
    }

    report.overall = unknown ? FieldStatus::Unknown : (repairable ? FieldStatus::Repairable : FieldStatus::Valid);
    return report;
}



RecoveryResult executeRecovery(const DescriptorView &view,
                               const ProbePacket &lastSet,
                               const ProbePacket &originalResponse,
                               const ProbeTransportOps &transport) {
    RecoveryResult result {};
    result.action = RecoveryAction::PassThrough;
    result.callerVisible = originalResponse;
    result.mutated = false;
    result.internalRetryAttempted = false;
    result.internalRetrySucceeded = false;

    const ValidationReport report = validateProbe(view, lastSet, originalResponse);
    FrameInfo candidate {};
    const RecoveryAction action = chooseRecoveryAction(view, lastSet, originalResponse, report, &candidate);
    result.action = action;

    if (action == RecoveryAction::PassThrough) return result;
    if (action == RecoveryAction::FieldRepair) {
        ProbePacket repaired {};
        if (buildFieldRepair(view, originalResponse, report, repaired)) {
            result.callerVisible = repaired;
            result.mutated = memcmp(repaired.bytes, originalResponse.bytes, originalResponse.length) != 0;
        }
        return result;
    }

    result.internalRetryAttempted = true;
    if (transport.setProbe == nullptr || transport.getProbe == nullptr) return result;

    ProbePacket retrySet {};
    if (!buildEquivalentRetrySet(lastSet, candidate, report.provenFrameSize, retrySet)) return result;
    if (transport.setProbe(transport.context, retrySet.bytes, retrySet.length) != TransportStatus::Success) return result;

    uint8_t raw[48] {};
    size_t actualLength = 0U;
    if (transport.getProbe(transport.context, raw, sizeof(raw), &actualLength) != TransportStatus::Success) return result;
    if (actualLength != retrySet.length || !isCanonicalProbeLength(actualLength)) return result;

    ProbePacket retryResponse {};
    if (parseProbePacket(raw, actualLength, retryResponse) != ProbeParseStatus::Ok) return result;
    const ValidationReport retryReport = validateProbe(view, retrySet, retryResponse);
    if (retryReport.overall != FieldStatus::Valid) return result;

    result.callerVisible = retryResponse;
    result.mutated = memcmp(retryResponse.bytes, originalResponse.bytes, originalResponse.length) != 0;
    result.internalRetrySucceeded = true;
    return result;
}

bool buildEquivalentRetrySet(const ProbePacket &lastSet,
                             const FrameInfo &candidate,
                             uint32_t provenFrameSize,
                             ProbePacket &retrySet) {
    if (!isCanonicalProbeLength(lastSet.length) || candidate.frameIndex == 0U ||
        candidate.formatIndex != lastSet.formatIndex() || candidate.frameIndex == lastSet.frameIndex()) {
        return false;
    }
    retrySet = lastSet;
    retrySet.setFrameIndex(candidate.frameIndex);
    if (candidate.kind == FormatKind::Uncompressed) {
        if (provenFrameSize == 0U) return false;
        retrySet.setMaxVideoFrameSize(provenFrameSize);
    }
    retrySet.setMaxPayloadTransferSize(0U);
    return true;
}

RecoveryAction chooseRecoveryAction(const DescriptorView &view,
                                    const ProbePacket &lastSet,
                                    const ProbePacket &response,
                                    const ValidationReport &report,
                                    FrameInfo *candidateOut) {
    (void)response;
    if (candidateOut != nullptr) memset(candidateOut, 0, sizeof(*candidateOut));
    if (report.overall == FieldStatus::Valid) return RecoveryAction::PassThrough;

    if (report.overall == FieldStatus::Repairable && report.resolvedFrame.frameIndex != 0U) {
        FrameInfo candidate {};
        if (view.findEquivalentFrame(report.resolvedFrame, lastSet.frameInterval(), candidate) == DescriptorStatus::Ok) {
            if (candidateOut != nullptr) *candidateOut = candidate;
            return RecoveryAction::RetryEquivalent;
        }
        ProbePacket ignored {};
        if (buildFieldRepair(view, response, report, ignored)) return RecoveryAction::FieldRepair;
    }
    return RecoveryAction::PassThrough;
}

bool buildFieldRepair(const DescriptorView &view,
                      const ProbePacket &response,
                      const ValidationReport &report,
                      ProbePacket &repaired) {
    (void)view;
    if (report.overall != FieldStatus::Repairable || report.invalidMask == InvalidNone) return false;
    const uint32_t supportedRepairMask = InvalidFrameSize | InvalidPayload;
    if ((report.invalidMask & ~supportedRepairMask) != 0U) return false;

    repaired = response;
    if ((report.invalidMask & InvalidFrameSize) != 0U) {
        if (report.provenFrameSize == 0U) return false;
        repaired.setMaxVideoFrameSize(report.provenFrameSize);
    }
    if ((report.invalidMask & InvalidPayload) != 0U) {
        if (!report.maxPayloadKnown || report.provenMaxPayload == 0U) return false;
        const uint32_t payload = response.maxPayloadTransferSize();
        if ((payload & 0xffff0000U) == 0xffff0000U) {
            const uint32_t low = payload & 0xffffU;
            if (low == 0U || low > report.provenMaxPayload) return false;
            repaired.setMaxPayloadTransferSize(low);
        } else if (payload > report.provenMaxPayload) {
            repaired.setMaxPayloadTransferSize(report.provenMaxPayload);
        } else {
            return false;
        }
    }
    return true;
}

} // namespace UVC
