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
#include <sys/sysctl.h>
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

// ---- observable counters (read via: sysctl kern.uvcp*) ----
extern "C" {
static int statStart = 0;      // start() entered
static int statLoaded = 0;     // loaded() callback fired
static int statSymbol = 0;     // symbol resolved
static int statProlog = 0;     // prologue verified
static int statRouted = 0;     // route installed
static int statHits = 0;       // hook saw a probe/commit GET_CUR
static int statExpanded = 0;   // enlarged request returned data
static int statFailed = 0;     // enlarged request failed
}
SYSCTL_DECL(_kern);
SYSCTL_INT(_kern, OID_AUTO, uvcpstart, CTLFLAG_RD, &statStart, 0, "UVCProbeFix start() entered");
SYSCTL_INT(_kern, OID_AUTO, uvcploaded, CTLFLAG_RD, &statLoaded, 0, "UVCProbeFix kext-load callback fired");
SYSCTL_INT(_kern, OID_AUTO, uvcpsymbol, CTLFLAG_RD, &statSymbol, 0, "UVCProbeFix symbol resolved");
SYSCTL_INT(_kern, OID_AUTO, uvcpprolog, CTLFLAG_RD, &statProlog, 0, "UVCProbeFix prologue verified");
SYSCTL_INT(_kern, OID_AUTO, uvcproute, CTLFLAG_RD, &statRouted, 0, "UVCProbeFix route installed");
SYSCTL_INT(_kern, OID_AUTO, uvcphits, CTLFLAG_RD, &statHits, 0, "UVCProbeFix probe requests seen");
SYSCTL_INT(_kern, OID_AUTO, uvcpexpand, CTLFLAG_RD, &statExpanded, 0, "UVCProbeFix probe requests enlarged+filled");
SYSCTL_INT(_kern, OID_AUTO, uvcpcpfail, CTLFLAG_RD, &statFailed, 0, "UVCProbeFix enlarged requests failed");

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

    __atomic_add_fetch(&statHits, 1, __ATOMIC_RELAXED);

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
        __atomic_add_fetch(&statExpanded, 1, __ATOMIC_RELAXED);
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
        __atomic_add_fetch(&statFailed, 1, __ATOMIC_RELAXED);
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
    __atomic_add_fetch(&statLoaded, 1, __ATOMIC_RELAXED);
    if (routeInstalled || size == 0 || !supportedKernel()) return;
    const auto target = patcher.solveSymbol(index, kSymbol, slide, size, false);
    if (target == 0) {
        patcher.clearError();
        SYSLOG(MODULE_SHORT, "UVC1.5 symbol unavailable; not installing hook");
        return;
    }
    __atomic_add_fetch(&statSymbol, 1, __ATOMIC_RELAXED);
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
    __atomic_add_fetch(&statProlog, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&statRouted, 1, __ATOMIC_RELAXED);
    routeInstalled = true;
    SYSLOG(MODULE_SHORT,
           "UVC1.5 GET_CUR lengthen route active (13D3:56F8 / 04F2:B6BF, Darwin %d)",
           static_cast<int>(getKernelVersion()));
}
}

bool start() {
    __atomic_add_fetch(&statStart, 1, __ATOMIC_RELAXED);
    SYSLOG(MODULE_SHORT, "UVC1.5 start() entered (Darwin %d)", static_cast<int>(getKernelVersion()));
    if (checkKernelArgument("-uvc15off")) {
        SYSLOG(MODULE_SHORT, "UVC1.5 disabled by -uvc15off");
        return true;
    }
    if (!supportedKernel()) {
        SYSLOG(MODULE_SHORT, "UVC1.5 kernel %d unsupported", static_cast<int>(getKernelVersion()));
        return false;
    }
    // IOUSBHostFamily is a prelinked built-in kext: it is already resident
    // before this plugin runs. Plain onKextLoad() only fires for kexts loaded
    // AFTER registration, so the callback never ran on real hardware and no
    // hook was ever installed. onKextLoadForce() replays it for live kexts.
    // onKextLoadForce() returns void: it replays the handler for kexts that
    // are already resident. Register first, then confirm our handler ran.
    lilu.onKextLoadForce(&hostInfo, 1, loaded, nullptr);
    SYSLOG(MODULE_SHORT, "UVC1.5 force-load watcher registered for IOUSBHostFamily");
    return true;
}
}
#else
// Host tests compile only the pure policy above.
#endif
