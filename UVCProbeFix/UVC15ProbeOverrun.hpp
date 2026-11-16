// UVC 1.5 probe/commit control-request overrun recovery.
// ov9734 port: AzureWave 13D3:56F8 (Huawei MateBook 14, bcdUVC 1.50) + HP 04F2:B6BF.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace UVC15 {
// Local machine: Huawei MateBook 14 / AzureWave ov9734, bcdUVC 1.50.
static constexpr uint16_t kOvVendor  = 0x13D3;
static constexpr uint16_t kOvProduct = 0x56F8;
// Upstream reference device.
static constexpr uint16_t kHPVendor  = 0x04F2;
static constexpr uint16_t kHPProduct = 0xB6BF;

// macOS allocates a UVC 1.1-sized buffer (26 bytes) for VS_PROBE_CONTROL and a
// UVC 1.5 device answers with 34 (or 48) bytes, which the host controller
// reports as kIOReturnOverrun (0xE00002E8). Instead of hardcoding a guess we
// accept any plausible host length, ask the device for kMaxProbe bytes, let it
// finish, and copy back only what the host asked for.
static constexpr uint16_t kMaxProbe = 128;

struct Request {
    uint16_t vendor, product;
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
    bool synchronous, rawBufferPresent, descriptorBufferPresent;
};

inline bool knownDevice(const Request &r) {
    return (r.vendor == kOvVendor && r.product == kOvProduct) ||
           (r.vendor == kHPVendor && r.product == kHPProduct);
}

// Class/interface IN, GET_CUR, VS_PROBE_CONTROL (0x0100) or VS_COMMIT_CONTROL
// (0x0200). Interface index is deliberately not filtered: the MateBook exposes
// the streaming interface on a different index than the HP reference device and
// an extra constraint here would silently disable the fix.
inline bool probeShaped(const Request &r) {
    return r.bmRequestType == 0xA1 && r.bRequest == 0x81 &&
           (r.wValue == 0x0100 || r.wValue == 0x0200) &&
           r.synchronous && r.rawBufferPresent && !r.descriptorBufferPresent &&
           r.wLength > 0 && r.wLength < kMaxProbe;
}

inline bool eligible(const Request &r) {
    return knownDevice(r) && probeShaped(r);
}

inline bool safeTransfer(uint32_t actual, size_t capacity) {
    return actual > 0 && actual <= capacity;
}

bool start();
}
