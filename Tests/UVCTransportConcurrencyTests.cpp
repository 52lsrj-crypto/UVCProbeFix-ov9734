#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "../UVCProbeFix/UVCTransportTahoe.hpp"

namespace {
std::vector<uint8_t> load(const char *path) {
    FILE *f = std::fopen(path, "rb"); assert(f != nullptr);
    std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::rewind(f); assert(n >= 0);
    std::vector<uint8_t> out(static_cast<size_t>(n));
    if (!out.empty()) assert(std::fread(out.data(), 1, out.size(), f) == out.size());
    std::fclose(f); return out;
}
void write32(std::vector<uint8_t> &b, size_t o, uint32_t v) {
    b[o]=static_cast<uint8_t>(v); b[o+1]=static_cast<uint8_t>(v>>8U);
    b[o+2]=static_cast<uint8_t>(v>>16U); b[o+3]=static_cast<uint8_t>(v>>24U);
}
struct MutexLock { std::mutex mutex; };
void lockFn(void *p) { static_cast<MutexLock *>(p)->mutex.lock(); }
void unlockFn(void *p) { static_cast<MutexLock *>(p)->mutex.unlock(); }

struct BlockingTransport {
    std::mutex mutex;
    std::condition_variable cv;
    bool setEntered {false};
    bool continueSet {false};
    std::vector<uint8_t> getResponse;
};
UVC::TransportStatus blockingSet(void *p, const uint8_t *, size_t) {
    auto *t=static_cast<BlockingTransport *>(p);
    std::unique_lock<std::mutex> lock(t->mutex);
    t->setEntered=true; t->cv.notify_all();
    t->cv.wait(lock,[&]{return t->continueSet;});
    return UVC::TransportStatus::Success;
}
UVC::TransportStatus blockingGet(void *p, uint8_t *out, size_t cap, size_t *actual) {
    auto *t=static_cast<BlockingTransport *>(p);
    if (cap < t->getResponse.size()) return UVC::TransportStatus::Error;
    std::memcpy(out,t->getResponse.data(),t->getResponse.size());
    *actual=t->getResponse.size();
    return UVC::TransportStatus::Success;
}

void testConcurrentExternalSetInvalidatesInFlightRecovery() {
    auto descriptors=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin");
    auto bad=load("Tests/Fixtures/Lenovo-malformed-probe.bin");
    auto recovered=load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    auto initialSet=recovered; initialSet[3]=4U; write32(initialSet,22,0U);
    auto newerSet=initialSet; newerSet[3]=1U;

    UVC::TransportEnvironment env{};
    env.interfaceKey=reinterpret_cast<const void *>(0x1234U);
    env.identity=UVC::DeviceIdentity{0x1111,0x2222,0x0100,1,1};
    env.interfaceView=UVC::InterfaceView{14,2,1};
    env.configuration=descriptors.data(); env.configurationLength=descriptors.size(); env.speed=UVC::UsbSpeed::High;

    MutexLock mutexLock;
    const UVC::ContextLockOps lockOps{&mutexLock,lockFn,unlockFn};
    UVC::TransportCoordinator coordinator; coordinator.reset(lockOps);
    assert(coordinator.recordExternalSet(env,initialSet.data(),initialSet.size(),lockOps));

    BlockingTransport transport; transport.getResponse=recovered;
    const UVC::ProbeTransportOps ops{&transport,blockingSet,blockingGet};
    UVC::CompletedGetResult result{};
    std::vector<uint8_t> caller=bad;

    std::thread recoveryThread([&]{
        result=coordinator.processCompletedGet(UVC::RuntimeMode::GenericFull,env,caller.data(),caller.size(),ops,lockOps);
    });
    {
        std::unique_lock<std::mutex> lock(transport.mutex);
        transport.cv.wait(lock,[&]{return transport.setEntered;});
    }
    assert(coordinator.recordExternalSet(env,newerSet.data(),newerSet.size(),lockOps));
    {
        std::lock_guard<std::mutex> lock(transport.mutex);
        transport.continueSet=true;
    }
    transport.cv.notify_all();
    recoveryThread.join();

    assert(result.staleDiscarded);
    assert(!result.mutated);
    assert(std::memcmp(caller.data(),bad.data(),bad.size())==0);
}
}

int main() {
    testConcurrentExternalSetInvalidatesInFlightRecovery();
    std::puts("UVCTransportConcurrencyTests: PASS");
}
