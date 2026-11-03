#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../UVCProbeFix/UVCDescriptorParser.hpp"

namespace {

[[noreturn]] void invariantFailure() {
    __builtin_trap();
}

bool sameFrame(const UVC::FrameInfo &a, const UVC::FrameInfo &b) {
    return a.formatIndex == b.formatIndex &&
           a.frameIndex == b.frameIndex &&
           a.defaultFrameIndex == b.defaultFrameIndex &&
           a.kind == b.kind &&
           a.bitsPerPixel == b.bitsPerPixel &&
           std::memcmp(a.guid, b.guid, sizeof(a.guid)) == 0 &&
           a.width == b.width && a.height == b.height &&
           a.maxFrameBufferSize == b.maxFrameBufferSize &&
           a.descriptorOffset == b.descriptorOffset;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const uint8_t interfaceNumber = size > 0U ? static_cast<uint8_t>(data[0] & 0x0FU) : 0U;
    const UVC::UsbSpeed speed = size > 1U
        ? static_cast<UVC::UsbSpeed>(1U + (data[1] % 3U))
        : UVC::UsbSpeed::High;

    UVC::DescriptorView first;
    UVC::DescriptorView second;
    const UVC::DescriptorStatus s1 = first.init(data, size, interfaceNumber, speed);
    const UVC::DescriptorStatus s2 = second.init(data, size, interfaceNumber, speed);
    if (s1 != s2) invariantFailure();
    if (s1 != UVC::DescriptorStatus::Ok) return 0;

    if (first.length() != second.length() ||
        first.configurationValue() != second.configurationValue() ||
        first.interfaceNumber() != second.interfaceNumber() ||
        first.streamEndpointAddress() != second.streamEndpointAddress()) {
        invariantFailure();
    }

    uint32_t payloadA = 0U;
    uint32_t payloadB = 0U;
    const UVC::DescriptorStatus pa = first.maxPeriodicPayload(payloadA);
    const UVC::DescriptorStatus pb = second.maxPeriodicPayload(payloadB);
    if (pa != pb || (pa == UVC::DescriptorStatus::Ok && payloadA != payloadB)) invariantFailure();

    const uint8_t formatIndex = size > 2U ? data[2] : 1U;
    const uint8_t frameIndex = size > 3U ? data[3] : 1U;
    UVC::FrameInfo frameA {};
    UVC::FrameInfo frameB {};
    const UVC::DescriptorStatus fa = first.findFrame(formatIndex, frameIndex, frameA);
    const UVC::DescriptorStatus fb = second.findFrame(formatIndex, frameIndex, frameB);
    if (fa != fb) invariantFailure();
    if (fa != UVC::DescriptorStatus::Ok) return 0;
    if (!sameFrame(frameA, frameB)) invariantFailure();

    uint32_t interval = 0U;
    if (size >= 8U) {
        interval = static_cast<uint32_t>(data[4]) |
                   (static_cast<uint32_t>(data[5]) << 8U) |
                   (static_cast<uint32_t>(data[6]) << 16U) |
                   (static_cast<uint32_t>(data[7]) << 24U);
    }

    const UVC::DescriptorStatus ia = first.supportsInterval(frameA, interval);
    const UVC::DescriptorStatus ib = second.supportsInterval(frameB, interval);
    if (ia != ib) invariantFailure();

    UVC::FrameInfo candidate {};
    const UVC::DescriptorStatus eq = first.findEquivalentFrame(frameA, interval, candidate);
    if (eq == UVC::DescriptorStatus::Ok) {
        if (candidate.formatIndex != frameA.formatIndex ||
            candidate.frameIndex == frameA.frameIndex ||
            candidate.kind != frameA.kind ||
            candidate.width != frameA.width || candidate.height != frameA.height ||
            std::memcmp(candidate.guid, frameA.guid, sizeof(candidate.guid)) != 0) {
            invariantFailure();
        }
    }
    return 0;
}
