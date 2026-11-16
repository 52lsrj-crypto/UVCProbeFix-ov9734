// UVC 1.5 GET_CUR probe/commit length workaround.
// Upstream: HP EliteBook 850 G7 / Chicony 04F2:B6BF (Tahoe-only).
// Ported here to AzureWave ov9734 (13D3:56F8, Huawei MateBook 14, bcdUVC 1.50)
// and relaxed to run on Darwin 24 (Sequoia) as well as Darwin 25 (Tahoe).
// The mechanism is a mangled-symbol hook, so it is build-independent; the
// original Tahoe gate was a tested-scope marker, not a technical limit.
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
static uint32_t logBudget {4};

// Exact C++ symbol selector, NOT a byte-pattern fallback.
static const char kSymbol[] =
    "__ZN15IOUSBHostDevice21internalDeviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj";
static const char *kPaths[] = {
    "/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily"
};
static KernelPatcher::KextInfo hostInfo {
    "com.apple.iokit.IOUSBHostFamily", kPaths, arrsize(kPaths), {true}, {},
    KernelPatcher::KextInfo::Unloaded
};

// Reject unknown prefixes or a function somebody already routed.
static bool verifiedPrologue(const uint8_t *p) {
    return p != nullptr && p[0] == 0x55 && p[1] == 0x48 &&
           p[2] == 0x89 && p[3] == 0xE5;
}

static bool supportedKernel() {
    const auto kv = getKernelVersion();
    return kv == KernelVersion::Sequoia || kv == KernelVersion::Tahoe;
}

static IOReturn passThrough(IOUSBHostDevice *device, IOService *client,
                            StandardUSB::DeviceRequest &request, void *raw,
                            IOMemoryDescriptor *descriptor, uint32_t &transferred,
                            IOUSBHostCompletion *completion, uint32_t timeout) {
    return originalRequest(device, client, request, raw, descriptor,
                           transferred, completion, timeout);
}

static IOReturn requestHook(IOUSBHostDevice *device, IOService *client,
                            StandardUSB::DeviceRequest &request,
                            void *raw, IOMemoryDescriptor *descriptor,
                            uint32_t &transferred, IOUSBHostCompletion *completion,
                            uint32_t timeout) {
    if (originalRequest == nullptr) return kIOReturnUnsupported;
    // Cheap rejection first: never touch descriptors or allocate for other USB IO.
    if (device == nullptr || request.bmRequestType != 0xA1 ||
        request.bRequest != 0x81 ||
        (request.wValue != 0x0100 && request.wValue != 0x0200) ||
        request.wLength == 0 || request.wLength >= kMaxProbe ||
        raw == nullptr || descriptor != nullptr || completion != nullptr) {
        return passThrough(device, client, request, raw, descriptor,
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
        return passThrough(device, client, request, raw, descriptor,
                           transferred, completion, timeout);
    }

    // Never expose a device-long transfer to Apple's short receive buffer.
    void *expanded = IOMalloc(kMaxProbe);
    if (expanded == nullptr) {
        return passThrough(device, client, request, raw, descriptor,
                           transferred, completion, timeout);
    }
    bzero(expanded, kMaxProbe);
    StandardUSB::DeviceRequest enlarged = request;
    enlarged.wLength = kMaxProbe;
    uint32_t actual = 0;
    const IOReturn result = originalRequest(device, client, enlarged,
        expanded, nullptr, actual, nullptr, timeout);

    uint32_t copied = 0;
    if (result == kIOReturnSuccess && safeTransfer(actual, kMaxProbe)) {
        copied = actual < request.wLength ? actual : request.wLength;
        memcpy(raw, expanded, copied);
        transferred = copied;
        if (logBudget > 0) {
            logBudget--;
            SYSLOG(MODULE_SHORT,
                   "UVC1.5 GET_CUR(0x%04X wIndex %u) host=%u device=%u copied=%u",
                   request.wValue, request.wIndex, request.wLength, actual, copied);
        }
    } else {
        transferred = 0;
        if (logBudget > 0) {
            logBudget--;
            SYSLOG(MODULE_SHORT,
                   "UVC1.5 GET_CUR(0x%04X wIndex %u) host=%u failed result=0x%08X actual=%u",
                   request.wValue, request.wIndex, request.wLength,
                   static_cast<unsigned>(result), actual);
        }
    }
    bzero(expanded, kMaxProbe);
    IOFree(expanded, kMaxProbe);
    if (copied > 0) return kIOReturnSuccess;
    return result != kIOReturnSuccess ? result : kIOReturnUnderrun;
}

static void loaded(void *, KernelPatcher &patcher,
                   size_t index, mach_vm_address_t slide, size_t size) {
    if (routeInstalled || size == 0 || !supportedKernel()) return;
    const auto target = patcher.solveSymbol(index, kSymbol, slide, size, false);
    if (target == 0) {
        patcher.clearError();
        SYSLOG(MODULE_SHORT, "UVC1.5 symbol unavailable; not installing hook");
        return;
    }
    if (!verifiedPrologue(reinterpret_cast<const uint8_t *>(target))) {
        SYSLOG(MODULE_SHORT, "UVC1.5 unverified/already-routed prologue; skipped");
        return;
    }
    KernelPatcher::RouteRequest route {kSymbol, requestHook, originalRequest};
    if (!patcher.routeMultiple(index, &route, 1, slide, size, true, false) ||
        originalRequest == nullptr) {
        patcher.clearError();
        SYSLOG(MODULE_SHORT, "UVC1.5 route failed; recovery disabled");
        return;
    }
    routeInstalled = true;
    SYSLOG(MODULE_SHORT,
           "UVC1.5 GET_CUR lengthen route active (13D3:56F8 / 04F2:B6BF, Darwin %d)",
           static_cast<int>(getKernelVersion()));
}
}

bool start() {
    if (checkKernelArgument("-uvc15off")) {
        SYSLOG(MODULE_SHORT, "UVC1.5 disabled by -uvc15off");
        return true;
    }
    if (!supportedKernel()) return false;
    const auto error = lilu.onKextLoad(&hostInfo, 1, loaded, nullptr);
    if (error != LiluAPI::Error::NoError) {
        SYSLOG(MODULE_SHORT, "UVC1.5 failed to register IOUSBHostFamily (%d)",
               static_cast<int>(error));
        return false;
    }
    return true;
}
}
#else
// Host tests compile only the pure policy above.
#endif
