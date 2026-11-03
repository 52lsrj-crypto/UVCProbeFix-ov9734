// UVC 1.5 HP control-request overrun recovery. Independent of the Lenovo legacy path.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace UVC15 {
static constexpr uint16_t kHPVendor = 0x04F2;
static constexpr uint16_t kHPProduct = 0xB6BF;
static constexpr uint16_t kHostLength = 34;
static constexpr uint16_t kDeviceLength = 48;

struct Request {
    uint16_t vendor, product;
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
    bool synchronous, rawBufferPresent, descriptorBufferPresent;
};

inline bool eligible(const Request &r) {
    // Class/interface IN, GET_CUR, VS_PROBE_CONTROL or VS_COMMIT_CONTROL.
    // Only HP 04F2:B6BF streaming interfaces 1 and 3 measured in the report.
    return r.vendor == kHPVendor && r.product == kHPProduct &&
           r.bmRequestType == 0xA1 && r.bRequest == 0x81 &&
           r.wLength == kHostLength && r.synchronous &&
           r.rawBufferPresent && !r.descriptorBufferPresent &&
           (r.wIndex == 1 || r.wIndex == 3) &&
           (r.wValue == 0x0100 || r.wValue == 0x0200);
}

inline bool safeTransfer(uint32_t actual, size_t capacity) {
    return actual == kDeviceLength && capacity >= kDeviceLength;
}

// Uses the existing UVCProbeFix Lilu plugin registration. No second kext.
bool start();
}
