#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../UVCProbeFix/UVCDescriptorParser.hpp"
#include "../UVCProbeFix/UVCProbeControl.hpp"
#include "../UVCProbeFix/UVCProbeEngine.hpp"
#include "../UVCProbeFix/UVCRecoveryContext.hpp"
#include "../UVCProbeFix/UVCRuntimeMode.hpp"
#include "../UVCProbeFix/UVCTransportTahoe.hpp"

namespace {
std::vector<uint8_t> load(const char *path) {
    FILE *f = std::fopen(path, "rb");
    assert(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::rewind(f);
    assert(n >= 0);
    std::vector<uint8_t> out(static_cast<size_t>(n));
    if (!out.empty()) assert(std::fread(out.data(), 1, out.size(), f) == out.size());
    std::fclose(f);
    return out;
}
void write32(std::vector<uint8_t> &b, size_t o, uint32_t v) {
    b[o] = static_cast<uint8_t>(v);
    b[o + 1] = static_cast<uint8_t>(v >> 8U);
    b[o + 2] = static_cast<uint8_t>(v >> 16U);
    b[o + 3] = static_cast<uint8_t>(v >> 24U);
}

struct FakeLock {
    int depth {0};
    int maxDepth {0};
};
void lockFn(void *ctx) {
    auto *l = static_cast<FakeLock *>(ctx);
    ++l->depth;
    if (l->depth > l->maxDepth) l->maxDepth = l->depth;
    assert(l->depth == 1);
}
void unlockFn(void *ctx) {
    auto *l = static_cast<FakeLock *>(ctx);
    assert(l->depth == 1);
    --l->depth;
}
UVC::ContextLockOps locks(FakeLock &lock) {
    return UVC::ContextLockOps{&lock, lockFn, unlockFn};
}

struct FakeTransport {
    FakeLock *lock {nullptr};
    UVC::TransportCoordinator *coordinator {nullptr};
    UVC::TransportEnvironment environment {};
    UVC::ContextLockOps lockOps {};
    std::vector<uint8_t> response;
    int setCalls {0};
    int getCalls {0};
    bool makeStaleOnSet {false};
    bool internalSeenOnSet {false};
    bool internalSeenOnGet {false};
};

UVC::TransportStatus fakeSet(void *ctx, const uint8_t *, size_t) {
    auto *f = static_cast<FakeTransport *>(ctx);
    ++f->setCalls;
    assert(f->lock != nullptr && f->lock->depth == 0);
    if (f->coordinator != nullptr) {
        f->internalSeenOnSet = f->coordinator->isInternal(f->environment, f->lockOps);
        if (f->makeStaleOnSet) {
            auto newer = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
            newer[3] = 4;
            write32(newer, 22, 0);
            assert(f->coordinator->recordExternalSet(f->environment, newer.data(), newer.size(), f->lockOps));
        }
    }
    return UVC::TransportStatus::Success;
}

UVC::TransportStatus fakeGet(void *ctx, uint8_t *out, size_t capacity, size_t *actual) {
    auto *f = static_cast<FakeTransport *>(ctx);
    ++f->getCalls;
    assert(f->lock != nullptr && f->lock->depth == 0);
    if (f->coordinator != nullptr) {
        f->internalSeenOnGet = f->coordinator->isInternal(f->environment, f->lockOps);
    }
    if (f->response.size() > capacity) return UVC::TransportStatus::Error;
    if (!f->response.empty()) std::memcpy(out, f->response.data(), f->response.size());
    if (actual != nullptr) *actual = f->response.size();
    return UVC::TransportStatus::Success;
}

UVC::ProbeTransportOps transportOps(FakeTransport &transport) {
    return UVC::ProbeTransportOps{&transport, fakeSet, fakeGet};
}

UVC::TransportEnvironment envFor(const std::vector<uint8_t> &descriptors) {
    UVC::TransportEnvironment env {};
    env.interfaceKey = reinterpret_cast<const void *>(0x1234U);
    env.identity = UVC::DeviceIdentity{0x1111, 0x2222, 0x0100, 1, 1};
    env.interfaceView = UVC::InterfaceView{14, 2, 1};
    env.configuration = descriptors.data();
    env.configurationLength = descriptors.size();
    env.speed = UVC::UsbSpeed::High;
    return env;
}

void testReentrancyBypassCounterIsBoundedMetadataOnly() {
    UVC::TransportCoordinator coordinator;
    FakeLock lock;
    coordinator.noteReentrancyBypass(locks(lock));
    coordinator.noteReentrancyBypass(locks(lock));
    const auto counters = coordinator.snapshotCounters(locks(lock));
    assert(counters.reentrancyBypass == 2U);
    assert(lock.depth == 0);
}

void testKnownSynchronousSymbolIsExact() {
    assert(std::strcmp(UVC::kTahoeSyncDeviceRequestSymbol,
                       "__ZN18IOUSBHostInterface13deviceRequestERN11StandardUSB13DeviceRequestEPvRjj") == 0);
    assert(UVC::isAcceptedTahoeDeviceRequestSymbol(UVC::kTahoeSyncDeviceRequestSymbol));
    assert(!UVC::isAcceptedTahoeDeviceRequestSymbol("__ZN18IOUSBHostInterface13deviceRequestEv"));
    assert(!UVC::isAcceptedTahoeDeviceRequestSymbol(nullptr));
}

void testObserveModeNeverMutatesOrRetries() {
    const auto desc = load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin");
    auto set = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    set[3] = 4;
    write32(set, 22, 0);
    auto response = load("Tests/Fixtures/Lenovo-malformed-probe.bin");
    const auto original = response;

    UVC::TransportCoordinator coordinator;
    FakeLock lock;
    const auto env = envFor(desc);
    assert(coordinator.recordExternalSet(env, set.data(), set.size(), locks(lock)));

    FakeTransport transport;
    transport.lock = &lock;
    transport.coordinator = &coordinator;
    transport.environment = env;
    transport.lockOps = locks(lock);
    transport.response = load("Tests/Fixtures/Lenovo-recovered-probe.bin");

    const auto result = coordinator.processCompletedGet(UVC::RuntimeMode::GenericObserve,
                                                        env,
                                                        response.data(), response.size(),
                                                        transportOps(transport), locks(lock));
    assert(!result.mutated);
    assert(!result.internalRetryAttempted);
    assert(std::memcmp(response.data(), original.data(), original.size()) == 0);
    assert(transport.setCalls == 0 && transport.getCalls == 0);
    assert(lock.depth == 0);
}

void testFieldModeRepairsOnlyWithoutTransport() {
    const auto desc = load("Tests/Fixtures/synthetic-descriptors-mixed.bin");
    auto set = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    set[3] = 1;
    write32(set, 18, 1U);
    write32(set, 22, 9000U);
    auto response = set;

    UVC::TransportCoordinator coordinator;
    FakeLock lock;
    const auto env = envFor(desc);
    assert(coordinator.recordExternalSet(env, set.data(), set.size(), locks(lock)));

    FakeTransport transport;
    transport.lock = &lock;
    transport.coordinator = &coordinator;
    transport.environment = env;
    transport.lockOps = locks(lock);

    const auto result = coordinator.processCompletedGet(UVC::RuntimeMode::GenericFieldRepairDev,
                                                        env,
                                                        response.data(), response.size(),
                                                        transportOps(transport), locks(lock));
    UVC::ProbePacket parsed {};
    assert(UVC::parseProbePacket(response.data(), response.size(), parsed) == UVC::ProbeParseStatus::Ok);
    assert(result.mutated);
    assert(result.action == UVC::RecoveryAction::FieldRepair);
    assert(!result.internalRetryAttempted);
    assert(parsed.maxVideoFrameSize() == 153600U);
    assert(parsed.maxPayloadTransferSize() == 3072U);
    assert(transport.setCalls == 0 && transport.getCalls == 0);
    assert(lock.depth == 0);
}

void testFullModeRetriesOnceWithNoLockAcrossIo() {
    const auto desc = load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin");
    auto set = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    set[3] = 4;
    write32(set, 22, 0);
    auto response = load("Tests/Fixtures/Lenovo-malformed-probe.bin");
    const auto recovered = load("Tests/Fixtures/Lenovo-recovered-probe.bin");

    UVC::TransportCoordinator coordinator;
    FakeLock lock;
    const auto env = envFor(desc);
    assert(coordinator.recordExternalSet(env, set.data(), set.size(), locks(lock)));

    FakeTransport transport;
    transport.lock = &lock;
    transport.coordinator = &coordinator;
    transport.environment = env;
    transport.lockOps = locks(lock);
    transport.response = recovered;

    const auto result = coordinator.processCompletedGet(UVC::RuntimeMode::GenericFull,
                                                        env,
                                                        response.data(), response.size(),
                                                        transportOps(transport), locks(lock));
    assert(result.mutated);
    assert(result.action == UVC::RecoveryAction::RetryEquivalent);
    assert(result.internalRetryAttempted && result.internalRetrySucceeded);
    assert(transport.setCalls == 1 && transport.getCalls == 1);
    assert(transport.internalSeenOnSet && transport.internalSeenOnGet);
    assert(std::memcmp(response.data(), recovered.data(), recovered.size()) == 0);
    assert(lock.depth == 0);
}

void testStaleSequenceDiscardsRecoveredBytes() {
    const auto desc = load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin");
    auto set = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    set[3] = 4;
    write32(set, 22, 0);
    auto response = load("Tests/Fixtures/Lenovo-malformed-probe.bin");
    const auto original = response;

    UVC::TransportCoordinator coordinator;
    FakeLock lock;
    const auto env = envFor(desc);
    assert(coordinator.recordExternalSet(env, set.data(), set.size(), locks(lock)));

    FakeTransport transport;
    transport.lock = &lock;
    transport.coordinator = &coordinator;
    transport.environment = env;
    transport.lockOps = locks(lock);
    transport.response = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    transport.makeStaleOnSet = true;

    const auto result = coordinator.processCompletedGet(UVC::RuntimeMode::GenericFull,
                                                        env,
                                                        response.data(), response.size(),
                                                        transportOps(transport), locks(lock));
    assert(!result.mutated);
    assert(result.staleDiscarded);
    assert(transport.setCalls == 1 && transport.getCalls == 1);
    assert(std::memcmp(response.data(), original.data(), original.size()) == 0);
    assert(lock.depth == 0);
}

void testValidDuplicateCameraIsByteIdentical() {
    const auto desc = load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin");
    const auto good = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    auto response = good;
    UVC::TransportCoordinator coordinator;
    FakeLock lock;
    const auto env = envFor(desc);
    assert(coordinator.recordExternalSet(env, good.data(), good.size(), locks(lock)));
    FakeTransport transport;
    transport.lock = &lock;
    const auto result = coordinator.processCompletedGet(UVC::RuntimeMode::GenericFull,
                                                        env,
                                                        response.data(), response.size(),
                                                        transportOps(transport), locks(lock));
    assert(!result.mutated);
    assert(result.action == UVC::RecoveryAction::PassThrough);
    assert(transport.setCalls == 0 && transport.getCalls == 0);
    assert(std::memcmp(response.data(), good.data(), good.size()) == 0);
}
// Regression: a successful internal GET with a different PROBE length must
// never be reported as a successful caller-visible repair.
void testLengthMismatchDoesNotReportRecovered() {
    const auto desc = load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin");
    auto set = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    set[3] = 4;
    write32(set, 22, 0);
    auto response = load("Tests/Fixtures/Lenovo-malformed-probe.bin");
    response.resize(34, 0xA5);
    const auto unchanged = response;
    UVC::TransportCoordinator coordinator;
    FakeLock lock;
    const auto env = envFor(desc);
    assert(coordinator.recordExternalSet(env, set.data(), set.size(), locks(lock)));
    FakeTransport transport;
    transport.lock = &lock;
    transport.coordinator = &coordinator;
    transport.environment = env;
    transport.lockOps = locks(lock);
    transport.response = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    const auto result = coordinator.processCompletedGet(
        UVC::RuntimeMode::GenericFull, env, response.data(), response.size(),
        transportOps(transport), locks(lock));
    assert(!result.internalRetryAttempted);
    assert(transport.setCalls == 0 && transport.getCalls == 0);
    assert(result.action == UVC::RecoveryAction::PassThrough);
    assert(!result.mutated);
    assert(!result.internalRetrySucceeded);
    assert(std::memcmp(response.data(), unchanged.data(), response.size()) == 0);
}
} // namespace

int main() {
    testReentrancyBypassCounterIsBoundedMetadataOnly();
    testKnownSynchronousSymbolIsExact();
    testObserveModeNeverMutatesOrRetries();
    testFieldModeRepairsOnlyWithoutTransport();
    testFullModeRetriesOnceWithNoLockAcrossIo();
    testStaleSequenceDiscardsRecoveredBytes();
    testValidDuplicateCameraIsByteIdentical();
    testLengthMismatchDoesNotReportRecovered();
    std::puts("UVCTransportCoordinatorTests: PASS");
    return 0;
}
