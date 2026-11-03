// HP EliteBook 850 G7 / Chicony 04F2:B6BF UVC 1.50 GET_CUR length workaround.
// Copyright (c) 2026 DNMTechLabs. All rights reserved.
// No changes to the frozen Lenovo 0.2.15 handshake region/policy.
#include "UVC15ProbeOverrun.hpp"

#ifdef UVCP_KERNEL_BUILD
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <string.h>

#define MODULE_SHORT "uvcp"
namespace UVC15 {
namespace {
using RequestFunction = IOReturn (*)(IOUSBHostDevice *, IOService *,
    StandardUSB::DeviceRequest &, void *, IOMemoryDescriptor *,
    uint32_t &, IOUSBHostCompletion *, uint32_t);
static RequestFunction originalRequest {nullptr};
static bool routeInstalled {false};

// This is an exact C++ symbol selector, NOT a byte-pattern fallback.
// If missing / already patched / unsupported, HP mode remains inactive.
static const char kSymbol[] =
    "__ZN15IOUSBHostDevice21internalDeviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj";
static const char *kPaths[] = {
    "/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily"
};
static KernelPatcher::KextInfo hostInfo {
    "com.apple.iokit.IOUSBHostFamily", kPaths, arrsize(kPaths), {true}, {},
    KernelPatcher::KextInfo::Unloaded
};

// Deliberately reject any unknown prefix or already-routed function.
// It can disable HP recovery on a valid optimized build; never force-hook.
static bool verifiedPrologue(const uint8_t *p) {
    return p != nullptr && p[0] == 0x55 && p[1] == 0x48 &&
           p[2] == 0x89 && p[3] == 0xE5;
}

static IOReturn requestHook(IOUSBHostDevice *device, IOService *client,
                            StandardUSB::DeviceRequest &request,
                            void *raw, IOMemoryDescriptor *descriptor,
                            uint32_t &transferred, IOUSBHostCompletion *completion,
                            uint32_t timeout) {
    if (originalRequest == nullptr) return kIOReturnUnsupported;
    // Avoid descriptor reads and memory allocations for all unrelated USB IO.
    if (device == nullptr || request.bmRequestType != 0xA1 ||
        request.bRequest != 0x81 || request.wLength != kHostLength ||
        (request.wValue != 0x0100 && request.wValue != 0x0200) ||
        (request.wIndex != 1 && request.wIndex != 3) ||
        raw == nullptr || descriptor != nullptr || completion != nullptr) {
        return originalRequest(device, client, request, raw, descriptor,
                               transferred, completion, timeout);
    }

    const auto *identity = device->getDeviceDescriptor();
    Request view {};
    if (identity != nullptr) {
        view.vendor = USBToHost16(identity->idVendor);
        view.product = USBToHost16(identity->idProduct);
    }
    view.bmRequestType = request.bmRequestType;
    view.bRequest = request.bRequest;
    view.wValue = request.wValue;
    view.wIndex = request.wIndex;
    view.wLength = request.wLength;
    view.synchronous = true;
    view.rawBufferPresent = true;
    view.descriptorBufferPresent = false;
    if (!eligible(view)) {
        return originalRequest(device, client, request, raw, descriptor,
                               transferred, completion, timeout);
    }

    // Never issue a 48-byte transfer into Apple's 34-byte receive buffer.
    void *expanded = IOMalloc(kDeviceLength);
    if (expanded == nullptr) {
        return originalRequest(device, client, request, raw, descriptor,
                               transferred, completion, timeout);
    }
    bzero(expanded, kDeviceLength);
    StandardUSB::DeviceRequest enlarged = request;
    enlarged.wLength = kDeviceLength;
    uint32_t actual = 0;
    const IOReturn result = originalRequest(device, client, enlarged,
        expanded, nullptr, actual, nullptr, timeout);
    const bool complete = result == kIOReturnSuccess &&
                          safeTransfer(actual, kDeviceLength);
    if (complete) {
        memcpy(raw, expanded, kHostLength);
        transferred = kHostLength;
    } else {
        transferred = 0;
    }
    bzero(expanded, kDeviceLength);
    IOFree(expanded, kDeviceLength);
    if (complete) return kIOReturnSuccess;
    return result != kIOReturnSuccess ? result : kIOReturnUnderrun;
}

static void loaded(void *, KernelPatcher &patcher,
                   size_t index, mach_vm_address_t slide, size_t size) {
    if (routeInstalled || size == 0 || getKernelVersion() != KernelVersion::Tahoe ||
        getKernelMinorVersion() != 6) return;
    const auto target = patcher.solveSymbol(index, kSymbol, slide, size, false);
    if (target == 0) {
        patcher.clearError();
        SYSLOG(MODULE_SHORT, "HP UVC1.5 symbol unavailable; not installing hook");
        return;
    }
    if (!verifiedPrologue(reinterpret_cast<const uint8_t *>(target))) {
        SYSLOG(MODULE_SHORT, "HP UVC1.5 unverified/already-routed prologue; skipped");
        return;
    }
    KernelPatcher::RouteRequest route {kSymbol, requestHook, originalRequest};
    if (!patcher.routeMultiple(index, &route, 1, slide, size, true, false) ||
        originalRequest == nullptr) {
        patcher.clearError();
        SYSLOG(MODULE_SHORT, "HP UVC1.5 route failed; recovery disabled");
        return;
    }
    routeInstalled = true;
    SYSLOG(MODULE_SHORT, "HP 04F2:B6BF UVC1.5 GET_CUR(34->48) route active");
}
}

bool start() {
    if (checkKernelArgument("-uvc15off")) return true;
    if (getKernelVersion() != KernelVersion::Tahoe || getKernelMinorVersion() != 6)
        return false;
    const auto error = lilu.onKextLoad(&hostInfo, 1, loaded, nullptr);
    if (error != LiluAPI::Error::NoError) {
        SYSLOG(MODULE_SHORT, "HP UVC1.5 failed to register IOUSBHostFamily (%d)",
               static_cast<int>(error));
        return false;
    }
    return true;
}
}
#else
// Host tests compile only the pure policy above.
#endif
