#include "UVCDescriptorParser.hpp"

#include <string.h>

namespace UVC {
namespace {
constexpr uint8_t kDescriptorTypeConfiguration = 0x02;
constexpr uint8_t kDescriptorTypeInterface = 0x04;
constexpr uint8_t kDescriptorTypeClassSpecificInterface = 0x24;
constexpr uint8_t kClassVideo = 14;
constexpr uint8_t kSubclassVideoStreaming = 2;
constexpr uint8_t kVsInputHeader = 0x01;
constexpr uint8_t kVsFormatUncompressed = 0x04;
constexpr uint8_t kVsFrameUncompressed = 0x05;
constexpr uint8_t kVsFormatMjpeg = 0x06;
constexpr uint8_t kVsFrameMjpeg = 0x07;
constexpr uint8_t kVsFormatFrameBased = 0x10;
constexpr uint8_t kVsFrameFrameBased = 0x11;
constexpr size_t kMaxConfigurationBytes = 4096;
constexpr size_t kMaxDescriptorRecords = 512;

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

FormatKind kindForFormatSubtype(uint8_t subtype) {
    switch (subtype) {
        case kVsFormatUncompressed: return FormatKind::Uncompressed;
        case kVsFormatMjpeg: return FormatKind::Mjpeg;
        case kVsFormatFrameBased: return FormatKind::FrameBased;
        default: return FormatKind::Unsupported;
    }
}

bool subtypeMatchesFrameKind(uint8_t subtype, FormatKind kind) {
    return (kind == FormatKind::Uncompressed && subtype == kVsFrameUncompressed) ||
           (kind == FormatKind::Mjpeg && subtype == kVsFrameMjpeg) ||
           (kind == FormatKind::FrameBased && subtype == kVsFrameFrameBased);
}

bool validateIntervalBlock(const uint8_t *descriptor,
                           size_t descriptorLength,
                           FormatKind kind) {
    const size_t typeOffset = kind == FormatKind::FrameBased ? 21U : 25U;
    const size_t valuesOffset = 26U;
    if (descriptorLength <= typeOffset || descriptorLength < valuesOffset) return false;
    const uint8_t count = descriptor[typeOffset];
    if (count == 0U) {
        if (descriptorLength < valuesOffset + 12U) return false;
        const uint32_t minInterval = readLe32(descriptor + valuesOffset);
        const uint32_t maxInterval = readLe32(descriptor + valuesOffset + 4U);
        const uint32_t step = readLe32(descriptor + valuesOffset + 8U);
        (void)step;
        return minInterval != 0U && maxInterval != 0U && minInterval <= maxInterval;
    }
    const size_t needed = valuesOffset + static_cast<size_t>(count) * 4U;
    if (needed < valuesOffset || descriptorLength < needed) return false;
    for (uint8_t i = 0; i < count; ++i) {
        if (readLe32(descriptor + valuesOffset + static_cast<size_t>(i) * 4U) == 0U) return false;
    }
    return true;
}

bool validateFormatDescriptor(size_t n, FormatKind kind) {
    if (kind == FormatKind::Uncompressed) return n >= 27U;
    if (kind == FormatKind::Mjpeg) return n >= 11U;
    if (kind == FormatKind::FrameBased) return n >= 28U;
    return true;
}

bool validateFrameDescriptor(const uint8_t *d, size_t n, FormatKind kind) {
    if (kind == FormatKind::Uncompressed || kind == FormatKind::Mjpeg) {
        if (n < 26U) return false;
    } else if (kind == FormatKind::FrameBased) {
        if (n < 26U) return false;
    } else {
        return true;
    }
    const uint16_t width = readLe16(d + 5U);
    const uint16_t height = readLe16(d + 7U);
    if (width == 0U || height == 0U) return false;
    return validateIntervalBlock(d, n, kind);
}

struct FormatCursor {
    FormatKind kind;
    uint8_t index;
    uint8_t defaultFrameIndex;
    uint8_t bitsPerPixel;
    uint8_t guid[16];
    bool valid;
};

void clearFormat(FormatCursor &f) {
    memset(&f, 0, sizeof(f));
    f.kind = FormatKind::Unsupported;
    f.valid = false;
}

bool populateFormatCursor(const uint8_t *d, size_t n, FormatCursor &f) {
    clearFormat(f);
    if (n < 4U) return false;
    f.kind = kindForFormatSubtype(d[2]);
    f.index = d[3];
    if (f.index == 0U) return false;
    if (f.kind == FormatKind::Uncompressed) {
        if (n < 27U) return false;
        memcpy(f.guid, d + 5U, 16U);
        f.bitsPerPixel = d[21];
        f.defaultFrameIndex = d[22];
    } else if (f.kind == FormatKind::Mjpeg) {
        if (n < 11U) return false;
        memset(f.guid, 0, sizeof(f.guid));
        f.bitsPerPixel = 0U;
        f.defaultFrameIndex = d[6];
    } else if (f.kind == FormatKind::FrameBased) {
        if (n < 28U) return false;
        memcpy(f.guid, d + 5U, 16U);
        f.bitsPerPixel = d[21];
        f.defaultFrameIndex = d[22];
    } else {
        f.defaultFrameIndex = 0U;
    }
    f.valid = true;
    return true;
}
} // namespace

DescriptorView::DescriptorView()
    : data_(nullptr), length_(0), configurationValue_(0), interfaceNumber_(0),
      streamEndpointAddress_(0), speed_(UsbSpeed::Unknown), initialized_(false) {}

DescriptorStatus DescriptorView::init(const uint8_t *configuration,
                                      size_t availableBytes,
                                      uint8_t targetInterfaceNumber,
                                      UsbSpeed speed) {
    data_ = nullptr;
    length_ = 0;
    configurationValue_ = 0;
    interfaceNumber_ = targetInterfaceNumber;
    streamEndpointAddress_ = 0;
    speed_ = speed;
    initialized_ = false;

    if (configuration == nullptr || availableBytes < 9U) return DescriptorStatus::Malformed;
    if (configuration[0] < 9U || configuration[1] != kDescriptorTypeConfiguration) return DescriptorStatus::Malformed;
    const uint16_t totalLength = readLe16(configuration + 2U);
    if (totalLength > kMaxConfigurationBytes) return DescriptorStatus::BoundsExceeded;
    if (totalLength < 9U || totalLength > availableBytes) return DescriptorStatus::Malformed;

    size_t offset = 0;
    size_t records = 0;
    bool inTargetVs = false;
    bool targetSeen = false;
    FormatCursor format {};
    clearFormat(format);

    while (offset < totalLength) {
        if (++records > kMaxDescriptorRecords) return DescriptorStatus::BoundsExceeded;
        if (totalLength - offset < 2U) return DescriptorStatus::Malformed;
        const uint8_t bLength = configuration[offset];
        const uint8_t type = configuration[offset + 1U];
        if (bLength < 2U || static_cast<size_t>(bLength) > totalLength - offset) return DescriptorStatus::Malformed;
        const uint8_t *d = configuration + offset;
        const size_t n = bLength;

        if (type == kDescriptorTypeInterface) {
            if (n < 9U) return DescriptorStatus::Malformed;
            inTargetVs = d[2] == targetInterfaceNumber && d[5] == kClassVideo && d[6] == kSubclassVideoStreaming;
            if (inTargetVs) targetSeen = true;
            clearFormat(format);
        } else if (inTargetVs && type == kDescriptorTypeClassSpecificInterface) {
            if (n < 3U) return DescriptorStatus::Malformed;
            const uint8_t subtype = d[2];
            if (subtype == kVsInputHeader) {
                if (n < 7U) return DescriptorStatus::Malformed;
                if (streamEndpointAddress_ == 0U) streamEndpointAddress_ = d[6];
            } else {
                const FormatKind formatKind = kindForFormatSubtype(subtype);
                if (formatKind != FormatKind::Unsupported) {
                    if (!validateFormatDescriptor(n, formatKind) || !populateFormatCursor(d, n, format)) {
                        return DescriptorStatus::Malformed;
                    }
                } else if (subtype == kVsFrameUncompressed || subtype == kVsFrameMjpeg || subtype == kVsFrameFrameBased) {
                    if (!format.valid || !subtypeMatchesFrameKind(subtype, format.kind) || !validateFrameDescriptor(d, n, format.kind)) {
                        return DescriptorStatus::Malformed;
                    }
                }
            }
        }
        offset += n;
    }

    if (offset != totalLength) return DescriptorStatus::Malformed;
    if (!targetSeen) return DescriptorStatus::NotFound;
    data_ = configuration;
    length_ = totalLength;
    configurationValue_ = configuration[5];
    initialized_ = true;
    return DescriptorStatus::Ok;
}

DescriptorStatus DescriptorView::findFrame(uint8_t formatIndex,
                                           uint8_t frameIndex,
                                           FrameInfo &out) const {
    if (!initialized_ || data_ == nullptr) return DescriptorStatus::Malformed;
    size_t offset = 0;
    bool inTargetVs = false;
    FormatCursor format {};
    clearFormat(format);
    bool requestedFormatSeen = false;

    while (offset < length_) {
        const uint8_t n8 = data_[offset];
        if (n8 < 2U || static_cast<size_t>(n8) > length_ - offset) return DescriptorStatus::Malformed;
        const size_t n = n8;
        const uint8_t type = data_[offset + 1U];
        const uint8_t *d = data_ + offset;
        if (type == kDescriptorTypeInterface) {
            inTargetVs = n >= 9U && d[2] == interfaceNumber_ && d[5] == kClassVideo && d[6] == kSubclassVideoStreaming;
            clearFormat(format);
        } else if (inTargetVs && type == kDescriptorTypeClassSpecificInterface && n >= 3U) {
            const uint8_t subtype = d[2];
            const FormatKind k = kindForFormatSubtype(subtype);
            if (k != FormatKind::Unsupported) {
                if (!populateFormatCursor(d, n, format)) return DescriptorStatus::Malformed;
                if (format.index == formatIndex) requestedFormatSeen = true;
            } else if (format.valid && subtypeMatchesFrameKind(subtype, format.kind)) {
                if (n < 4U) return DescriptorStatus::Malformed;
                if (format.index == formatIndex && d[3] == frameIndex) {
                    if (!validateFrameDescriptor(d, n, format.kind)) return DescriptorStatus::Malformed;
                    memset(&out, 0, sizeof(out));
                    out.formatIndex = format.index;
                    out.frameIndex = d[3];
                    out.defaultFrameIndex = format.defaultFrameIndex;
                    out.kind = format.kind;
                    out.bitsPerPixel = format.bitsPerPixel;
                    memcpy(out.guid, format.guid, sizeof(out.guid));
                    out.width = readLe16(d + 5U);
                    out.height = readLe16(d + 7U);
                    out.maxFrameBufferSize = format.kind == FormatKind::FrameBased ? 0U : readLe32(d + 17U);
                    out.descriptorOffset = offset;
                    return DescriptorStatus::Ok;
                }
            }
        }
        offset += n;
    }
    return requestedFormatSeen ? DescriptorStatus::NotFound : DescriptorStatus::NotFound;
}

DescriptorStatus DescriptorView::supportsInterval(const FrameInfo &frame,
                                                   uint32_t requestedInterval) const {
    if (!initialized_ || requestedInterval == 0U || frame.descriptorOffset >= length_) return DescriptorStatus::Malformed;
    const size_t offset = frame.descriptorOffset;
    if (length_ - offset < 2U) return DescriptorStatus::Malformed;
    const uint8_t n8 = data_[offset];
    if (n8 < 2U || static_cast<size_t>(n8) > length_ - offset) return DescriptorStatus::Malformed;
    const uint8_t *d = data_ + offset;
    const size_t n = n8;
    if (d[1] != kDescriptorTypeClassSpecificInterface || n < 4U || !subtypeMatchesFrameKind(d[2], frame.kind)) {
        return DescriptorStatus::Malformed;
    }
    if (!validateFrameDescriptor(d, n, frame.kind)) return DescriptorStatus::Malformed;
    const size_t typeOffset = frame.kind == FormatKind::FrameBased ? 21U : 25U;
    const size_t valuesOffset = 26U;
    const uint8_t count = d[typeOffset];
    if (count == 0U) {
        const uint32_t minInterval = readLe32(d + valuesOffset);
        const uint32_t maxInterval = readLe32(d + valuesOffset + 4U);
        const uint32_t step = readLe32(d + valuesOffset + 8U);
        if (requestedInterval < minInterval || requestedInterval > maxInterval) return DescriptorStatus::Unsupported;
        if (step != 0U && ((requestedInterval - minInterval) % step) != 0U) return DescriptorStatus::Unsupported;
        return DescriptorStatus::Ok;
    }
    for (uint8_t i = 0; i < count; ++i) {
        if (readLe32(d + valuesOffset + static_cast<size_t>(i) * 4U) == requestedInterval) return DescriptorStatus::Ok;
    }
    return DescriptorStatus::Unsupported;
}



DescriptorStatus DescriptorView::findEquivalentFrame(const FrameInfo &source,
                                                     uint32_t requestedInterval,
                                                     FrameInfo &candidate) const {
    if (!initialized_ || data_ == nullptr || source.formatIndex == 0U ||
        source.frameIndex == 0U || requestedInterval == 0U) {
        return DescriptorStatus::NotFound;
    }

    uint32_t sourceSize = 0U;
    if (source.kind == FormatKind::Uncompressed) {
        if (source.maxFrameBufferSize != 0U) {
            sourceSize = source.maxFrameBufferSize;
        } else if (source.bitsPerPixel != 0U && (source.bitsPerPixel % 8U) == 0U) {
            const uint64_t computed = static_cast<uint64_t>(source.width) *
                                      static_cast<uint64_t>(source.height) *
                                      static_cast<uint64_t>(source.bitsPerPixel / 8U);
            if (computed != 0U && computed <= 0xffffffffULL) {
                sourceSize = static_cast<uint32_t>(computed);
            }
        }
        if (sourceSize == 0U) return DescriptorStatus::NotFound;
    }

    size_t offset = 0U;
    bool inTargetVs = false;
    bool have = false;
    FrameInfo best {};
    FormatCursor format {};
    clearFormat(format);

    while (offset < length_) {
        if (length_ - offset < 2U) return DescriptorStatus::Malformed;
        const uint8_t n8 = data_[offset];
        if (n8 < 2U || static_cast<size_t>(n8) > length_ - offset) {
            return DescriptorStatus::Malformed;
        }
        const size_t n = n8;
        const uint8_t *d = data_ + offset;
        const uint8_t type = d[1];

        if (type == kDescriptorTypeInterface) {
            if (n < 9U) return DescriptorStatus::Malformed;
            inTargetVs = d[2] == interfaceNumber_ && d[5] == kClassVideo &&
                         d[6] == kSubclassVideoStreaming;
            clearFormat(format);
        } else if (inTargetVs && type == kDescriptorTypeClassSpecificInterface) {
            if (n < 3U) return DescriptorStatus::Malformed;
            const uint8_t subtype = d[2];
            const FormatKind kind = kindForFormatSubtype(subtype);
            if (kind != FormatKind::Unsupported) {
                if (!populateFormatCursor(d, n, format)) return DescriptorStatus::Malformed;
            } else if (format.valid && format.index == source.formatIndex &&
                       subtypeMatchesFrameKind(subtype, format.kind)) {
                if (!validateFrameDescriptor(d, n, format.kind) || n < 4U) {
                    return DescriptorStatus::Malformed;
                }
                if (d[3] != source.frameIndex && format.kind == source.kind &&
                    memcmp(format.guid, source.guid, sizeof(format.guid)) == 0) {
                    FrameInfo current {};
                    current.formatIndex = format.index;
                    current.frameIndex = d[3];
                    current.defaultFrameIndex = format.defaultFrameIndex;
                    current.kind = format.kind;
                    current.bitsPerPixel = format.bitsPerPixel;
                    memcpy(current.guid, format.guid, sizeof(current.guid));
                    current.width = readLe16(d + 5U);
                    current.height = readLe16(d + 7U);
                    current.maxFrameBufferSize = format.kind == FormatKind::FrameBased
                        ? 0U : readLe32(d + 17U);
                    current.descriptorOffset = offset;

                    if (current.width == source.width && current.height == source.height &&
                        supportsInterval(current, requestedInterval) == DescriptorStatus::Ok) {
                        bool sizeCompatible = true;
                        if (source.kind == FormatKind::Uncompressed) {
                            uint32_t currentSize = current.maxFrameBufferSize;
                            if (currentSize == 0U && current.bitsPerPixel != 0U &&
                                (current.bitsPerPixel % 8U) == 0U) {
                                const uint64_t computed = static_cast<uint64_t>(current.width) *
                                                          static_cast<uint64_t>(current.height) *
                                                          static_cast<uint64_t>(current.bitsPerPixel / 8U);
                                if (computed != 0U && computed <= 0xffffffffULL) {
                                    currentSize = static_cast<uint32_t>(computed);
                                }
                            }
                            sizeCompatible = currentSize != 0U && currentSize == sourceSize;
                        }
                        if (sizeCompatible) {
                            if (current.frameIndex == source.defaultFrameIndex) {
                                candidate = current;
                                return DescriptorStatus::Ok;
                            }
                            if (!have || current.frameIndex < best.frameIndex) {
                                best = current;
                                have = true;
                            }
                        }
                    }
                }
            }
        }
        offset += n;
    }

    if (!have) return DescriptorStatus::NotFound;
    candidate = best;
    return DescriptorStatus::Ok;
}


DescriptorStatus DescriptorView::maxPeriodicPayload(uint32_t &bytesPerServiceInterval) const {
    bytesPerServiceInterval = 0U;
    if (!initialized_ || data_ == nullptr || streamEndpointAddress_ == 0U || speed_ == UsbSpeed::Unknown) {
        return DescriptorStatus::CapacityUnknown;
    }

    constexpr uint8_t kDescriptorTypeEndpoint = 0x05;
    constexpr uint8_t kDescriptorTypeSsEndpointCompanion = 0x30;
    size_t offset = 0U;
    bool inTargetVs = false;
    bool found = false;
    uint32_t maximum = 0U;

    while (offset < length_) {
        const uint8_t n8 = data_[offset];
        if (n8 < 2U || static_cast<size_t>(n8) > length_ - offset) return DescriptorStatus::CapacityUnknown;
        const size_t n = n8;
        const uint8_t type = data_[offset + 1U];
        const uint8_t *d = data_ + offset;
        if (type == kDescriptorTypeInterface) {
            if (n < 9U) return DescriptorStatus::CapacityUnknown;
            inTargetVs = d[2] == interfaceNumber_ && d[5] == kClassVideo && d[6] == kSubclassVideoStreaming;
        } else if (inTargetVs && type == kDescriptorTypeEndpoint) {
            if (n < 7U) return DescriptorStatus::CapacityUnknown;
            if (d[2] == streamEndpointAddress_) {
                if ((d[3] & 0x03U) != 0x01U) return DescriptorStatus::CapacityUnknown;
                const uint16_t raw = readLe16(d + 4U);
                uint32_t capacity = 0U;
                if (speed_ == UsbSpeed::Full) {
                    if ((raw & 0xf800U) != 0U) return DescriptorStatus::CapacityUnknown;
                    const uint32_t base = raw & 0x07ffU;
                    if (base == 0U || base > 1023U) return DescriptorStatus::CapacityUnknown;
                    capacity = base;
                } else if (speed_ == UsbSpeed::High) {
                    if ((raw & 0xe000U) != 0U) return DescriptorStatus::CapacityUnknown;
                    const uint32_t base = raw & 0x07ffU;
                    const uint32_t multiplierBits = (raw >> 11U) & 0x03U;
                    if (base == 0U || base > 1024U || multiplierBits == 3U) return DescriptorStatus::CapacityUnknown;
                    const uint32_t transactions = multiplierBits + 1U;
                    if (base > 0xffffffffU / transactions) return DescriptorStatus::CapacityUnknown;
                    capacity = base * transactions;
                } else if (speed_ == UsbSpeed::Super) {
                    if ((raw & 0xf800U) != 0U) return DescriptorStatus::CapacityUnknown;
                    const uint32_t base = raw & 0x07ffU;
                    if (base == 0U || base > 1024U) return DescriptorStatus::CapacityUnknown;
                    const size_t next = offset + n;
                    if (next >= length_ || length_ - next < 2U) return DescriptorStatus::CapacityUnknown;
                    const uint8_t cn = data_[next];
                    if (cn < 6U || static_cast<size_t>(cn) > length_ - next ||
                        data_[next + 1U] != kDescriptorTypeSsEndpointCompanion) {
                        return DescriptorStatus::CapacityUnknown;
                    }
                    const uint32_t maxBurst = data_[next + 2U];
                    const uint32_t attributes = data_[next + 3U];
                    const uint32_t multBits = attributes & 0x03U;
                    if (maxBurst > 15U || (attributes & ~0x03U) != 0U || multBits == 3U) {
                        return DescriptorStatus::CapacityUnknown;
                    }
                    const uint32_t bursts = maxBurst + 1U;
                    const uint32_t mult = multBits + 1U;
                    if (base > 0xffffffffU / bursts || base * bursts > 0xffffffffU / mult) {
                        return DescriptorStatus::CapacityUnknown;
                    }
                    const uint32_t physicalMaximum = base * bursts * mult;
                    capacity = readLe16(data_ + next + 4U);
                    if (capacity == 0U || capacity > physicalMaximum) {
                        return DescriptorStatus::CapacityUnknown;
                    }
                } else {
                    return DescriptorStatus::CapacityUnknown;
                }
                if (capacity > maximum) maximum = capacity;
                found = true;
            }
        }
        offset += n;
    }
    if (!found || maximum == 0U) return DescriptorStatus::CapacityUnknown;
    bytesPerServiceInterval = maximum;
    return DescriptorStatus::Ok;
}

} // namespace UVC
