#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../UVCProbeFix/UVCProbeControl.hpp"

namespace {
std::vector<uint8_t> load(const char *path) {
    FILE *f = std::fopen(path, "rb");
    assert(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::rewind(f);
    assert(n >= 0);
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    if (!bytes.empty()) assert(std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size());
    std::fclose(f);
    return bytes;
}

void testCanonicalLengths() {
    const auto base = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    for (size_t length : {size_t(26), size_t(34), size_t(48)}) {
        std::vector<uint8_t> bytes(length, 0xA5);
        std::memcpy(bytes.data(), base.data(), base.size());
        UVC::ProbePacket packet {};
        assert(UVC::parseProbePacket(bytes.data(), bytes.size(), packet) == UVC::ProbeParseStatus::Ok);
        assert(packet.length == length);
        assert(std::memcmp(packet.bytes, bytes.data(), length) == 0);
    }
}

void testUnsupportedLengths() {
    const auto base = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    for (size_t length = 0; length <= 64; ++length) {
        if (length == 26 || length == 34 || length == 48) continue;
        std::vector<uint8_t> bytes(length, 0);
        if (length >= base.size()) std::memcpy(bytes.data(), base.data(), base.size());
        UVC::ProbePacket packet {};
        const auto status = UVC::parseProbePacket(bytes.empty() ? nullptr : bytes.data(), bytes.size(), packet);
        if (length == 0) assert(status == UVC::ProbeParseStatus::InvalidArgument);
        else assert(status == UVC::ProbeParseStatus::UnsupportedLength);
    }
    UVC::ProbePacket packet {};
    assert(UVC::parseProbePacket(nullptr, 26, packet) == UVC::ProbeParseStatus::InvalidArgument);
}

void testDecodedFixtures() {
    {
        const auto bytes = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
        UVC::ProbePacket p {};
        assert(UVC::parseProbePacket(bytes.data(), bytes.size(), p) == UVC::ProbeParseStatus::Ok);
        assert(p.formatIndex() == 1);
        assert(p.frameIndex() == 1);
        assert(p.frameInterval() == 333333U);
        assert(p.maxVideoFrameSize() == 614400U);
        assert(p.maxPayloadTransferSize() == 3072U);
    }
    {
        const auto bytes = load("Tests/Fixtures/ASUS-known-good-probe.bin");
        UVC::ProbePacket p {};
        assert(UVC::parseProbePacket(bytes.data(), bytes.size(), p) == UVC::ProbeParseStatus::Ok);
        assert(p.formatIndex() == 1);
        assert(p.frameIndex() == 1);
        assert(p.frameInterval() == 333333U);
        assert(p.maxVideoFrameSize() == 1843789U);
        assert(p.maxPayloadTransferSize() == 3072U);
    }
}

void testMutationPreservesUnrelatedBytes() {
    const auto base = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    for (size_t length : {size_t(26), size_t(34), size_t(48)}) {
        std::vector<uint8_t> bytes(length);
        for (size_t i = 0; i < length; ++i) bytes[i] = static_cast<uint8_t>(0x40U + i);
        std::memcpy(bytes.data(), base.data(), base.size());
        const auto original = bytes;
        UVC::ProbePacket p {};
        assert(UVC::parseProbePacket(bytes.data(), bytes.size(), p) == UVC::ProbeParseStatus::Ok);
        p.setFrameIndex(7);
        p.setMaxVideoFrameSize(0x12345678U);
        p.setMaxPayloadTransferSize(0x11223344U);
        uint8_t out[48];
        std::memset(out, 0xCC, sizeof(out));
        assert(UVC::copyProbePacket(p, out, sizeof(out)));
        for (size_t i = 0; i < length; ++i) {
            const bool changed = i == 3 || (i >= 18 && i < 26);
            if (!changed) assert(out[i] == original[i]);
        }
        if (length > 26) assert(std::memcmp(out + 26, original.data() + 26, length - 26) == 0);
    }
}

void testCopyFailuresDoNotWrite() {
    const auto bytes = load("Tests/Fixtures/Lenovo-recovered-probe.bin");
    UVC::ProbePacket p {};
    assert(UVC::parseProbePacket(bytes.data(), bytes.size(), p) == UVC::ProbeParseStatus::Ok);
    uint8_t small[25];
    std::memset(small, 0x5A, sizeof(small));
    assert(!UVC::copyProbePacket(p, small, sizeof(small)));
    for (uint8_t b : small) assert(b == 0x5A);
    assert(!UVC::copyProbePacket(p, nullptr, 26));
}
}

int main() {
    testCanonicalLengths();
    testUnsupportedLengths();
    testDecodedFixtures();
    testMutationPreservesUnrelatedBytes();
    testCopyFailuresDoNotWrite();
    std::puts("UVCProbeControlTests: PASS");
    return 0;
}
