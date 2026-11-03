#pragma once

#include <stddef.h>
#include <stdint.h>

namespace UVC {

enum class UsbSpeed : uint8_t { Unknown, Full, High, Super };
enum class FormatKind : uint8_t { Unsupported, Uncompressed, Mjpeg, FrameBased };
enum class DescriptorStatus : uint8_t { Ok, NotFound, Unsupported, Malformed, BoundsExceeded, CapacityUnknown };

struct FrameInfo {
    uint8_t formatIndex;
    uint8_t frameIndex;
    uint8_t defaultFrameIndex;
    FormatKind kind;
    uint8_t bitsPerPixel;
    uint8_t guid[16];
    uint16_t width;
    uint16_t height;
    uint32_t maxFrameBufferSize;
    size_t descriptorOffset;
};

class DescriptorView {
public:
    DescriptorView();

    DescriptorStatus init(const uint8_t *configuration,
                          size_t availableBytes,
                          uint8_t targetInterfaceNumber,
                          UsbSpeed speed);

    DescriptorStatus findFrame(uint8_t formatIndex,
                               uint8_t frameIndex,
                               FrameInfo &out) const;
    DescriptorStatus supportsInterval(const FrameInfo &frame,
                                      uint32_t requestedInterval) const;
    DescriptorStatus maxPeriodicPayload(uint32_t &bytesPerServiceInterval) const;
    DescriptorStatus findEquivalentFrame(const FrameInfo &source,
                                         uint32_t requestedInterval,
                                         FrameInfo &candidate) const;

    uint8_t configurationValue() const { return configurationValue_; }
    uint8_t interfaceNumber() const { return interfaceNumber_; }
    uint8_t streamEndpointAddress() const { return streamEndpointAddress_; }
    UsbSpeed speed() const { return speed_; }
    const uint8_t *data() const { return data_; }
    size_t length() const { return length_; }

private:
    const uint8_t *data_;
    size_t length_;
    uint8_t configurationValue_;
    uint8_t interfaceNumber_;
    uint8_t streamEndpointAddress_;
    UsbSpeed speed_;
    bool initialized_;
};

} // namespace UVC
