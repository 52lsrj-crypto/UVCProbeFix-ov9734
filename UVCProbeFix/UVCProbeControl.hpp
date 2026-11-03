#pragma once

#include <stddef.h>
#include <stdint.h>

namespace UVC {

enum class ProbeParseStatus : uint8_t {
    Ok,
    InvalidArgument,
    UnsupportedLength
};

struct ProbePacket {
    uint8_t bytes[48];
    uint8_t length;

    uint16_t bmHint() const;
    uint8_t formatIndex() const;
    uint8_t frameIndex() const;
    uint32_t frameInterval() const;
    uint32_t maxVideoFrameSize() const;
    uint32_t maxPayloadTransferSize() const;

    void setFrameIndex(uint8_t value);
    void setMaxVideoFrameSize(uint32_t value);
    void setMaxPayloadTransferSize(uint32_t value);
};

ProbeParseStatus parseProbePacket(const uint8_t *data, size_t length, ProbePacket &out);
bool copyProbePacket(const ProbePacket &packet, uint8_t *out, size_t capacity);
bool isCanonicalProbeLength(size_t length);

} // namespace UVC
