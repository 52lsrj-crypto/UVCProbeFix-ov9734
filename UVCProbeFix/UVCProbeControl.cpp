#include "UVCProbeControl.hpp"

#include <string.h>

namespace UVC {
namespace {
uint16_t readLe16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8U);
}

uint32_t readLe32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) |
           (static_cast<uint32_t>(p[3]) << 24U);
}

void writeLe32(uint8_t *p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value & 0xffU);
    p[1] = static_cast<uint8_t>((value >> 8U) & 0xffU);
    p[2] = static_cast<uint8_t>((value >> 16U) & 0xffU);
    p[3] = static_cast<uint8_t>((value >> 24U) & 0xffU);
}
} // namespace

bool isCanonicalProbeLength(size_t length) {
    return length == 26U || length == 34U || length == 48U;
}

ProbeParseStatus parseProbePacket(const uint8_t *data, size_t length, ProbePacket &out) {
    if (data == nullptr || length == 0U) {
        return ProbeParseStatus::InvalidArgument;
    }
    if (!isCanonicalProbeLength(length)) {
        return ProbeParseStatus::UnsupportedLength;
    }
    memset(out.bytes, 0, sizeof(out.bytes));
    memcpy(out.bytes, data, length);
    out.length = static_cast<uint8_t>(length);
    return ProbeParseStatus::Ok;
}

bool copyProbePacket(const ProbePacket &packet, uint8_t *out, size_t capacity) {
    const size_t length = packet.length;
    if (out == nullptr || !isCanonicalProbeLength(length) || capacity < length) {
        return false;
    }
    memcpy(out, packet.bytes, length);
    return true;
}

uint16_t ProbePacket::bmHint() const { return readLe16(bytes); }
uint8_t ProbePacket::formatIndex() const { return bytes[2]; }
uint8_t ProbePacket::frameIndex() const { return bytes[3]; }
uint32_t ProbePacket::frameInterval() const { return readLe32(bytes + 4); }
uint32_t ProbePacket::maxVideoFrameSize() const { return readLe32(bytes + 18); }
uint32_t ProbePacket::maxPayloadTransferSize() const { return readLe32(bytes + 22); }
void ProbePacket::setFrameIndex(uint8_t value) { bytes[3] = value; }
void ProbePacket::setMaxVideoFrameSize(uint32_t value) { writeLe32(bytes + 18, value); }
void ProbePacket::setMaxPayloadTransferSize(uint32_t value) { writeLe32(bytes + 22, value); }

} // namespace UVC
