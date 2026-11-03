#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../UVCProbeFix/UVCProbeControl.hpp"

namespace {

[[noreturn]] void invariantFailure() {
    __builtin_trap();
}

bool isMutatedOffset(size_t i) {
    return i == 3U || (i >= 18U && i <= 25U);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    UVC::ProbePacket a {};
    UVC::ProbePacket b {};
    const UVC::ProbeParseStatus sa = UVC::parseProbePacket(data, size, a);
    const UVC::ProbeParseStatus sb = UVC::parseProbePacket(data, size, b);

    if (sa != sb) invariantFailure();
    if (sa != UVC::ProbeParseStatus::Ok) return 0;
    if (!UVC::isCanonicalProbeLength(size)) invariantFailure();
    if (a.length != size || b.length != size) invariantFailure();
    if (std::memcmp(a.bytes, b.bytes, size) != 0) invariantFailure();
    if (std::memcmp(a.bytes, data, size) != 0) invariantFailure();

    UVC::ProbePacket mutated = a;
    mutated.setFrameIndex(static_cast<uint8_t>(a.frameIndex() ^ 0x5AU));
    mutated.setMaxVideoFrameSize(a.maxVideoFrameSize() ^ 0xA5A55A5AU);
    mutated.setMaxPayloadTransferSize(a.maxPayloadTransferSize() ^ 0x5A5AA5A5U);

    for (size_t i = 0; i < size; ++i) {
        if (!isMutatedOffset(i) && mutated.bytes[i] != a.bytes[i]) invariantFailure();
    }

    uint8_t copied[48] = {};
    if (!UVC::copyProbePacket(mutated, copied, sizeof(copied))) invariantFailure();
    if (std::memcmp(copied, mutated.bytes, size) != 0) invariantFailure();
    return 0;
}
