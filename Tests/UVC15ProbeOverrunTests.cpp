#include "../UVCProbeFix/UVC15ProbeOverrun.hpp"
#include <cassert>
#include <cstdio>

// Policy tests for the ov9734 port.
// Upstream asserted a strictly HP-bound, index-bound, 34-byte-only policy.
// The port deliberately widens three things, and each widening is asserted here
// so a future regression cannot silently narrow it again:
//   1. AzureWave ov9734 (13D3:56F8) is a first-class target alongside the HP card
//   2. any plausible host length is accepted (macOS may ask 26 or 34 bytes)
//   3. the streaming interface index is not filtered (the MateBook differs)
int main() {
    UVC15::Request r {0x04F2, 0xB6BF, 0xA1, 0x81, 0x0100, 1, 34, true, true, false};
    assert(UVC15::eligible(r));

    // 1. ov9734 accepted, HP still accepted
    r.vendor = 0x13D3;
    r.product = 0x56F8;
    assert(UVC15::eligible(r));

    // 2. host-length tolerance
    r.wLength = 26;
    assert(UVC15::eligible(r));
    r.wLength = 34;
    assert(UVC15::eligible(r));
    r.wLength = 48;
    assert(UVC15::eligible(r));
    r.wLength = 0;
    assert(!UVC15::eligible(r));
    r.wLength = UVC15::kMaxProbe;
    assert(!UVC15::eligible(r));
    r.wLength = 34;

    // 3. interface index not filtered
    r.wIndex = 0;
    assert(UVC15::eligible(r));
    r.wIndex = 2;
    assert(UVC15::eligible(r));
    r.wIndex = 7;
    assert(UVC15::eligible(r));

    // Only VS_PROBE_CONTROL / VS_COMMIT_CONTROL
    r.wValue = 0x0200;
    assert(UVC15::eligible(r));
    r.wValue = 0x0300;
    assert(!UVC15::eligible(r));
    r.wValue = 0x0100;

    // Unknown devices stay rejected
    r.vendor = 0x174F;
    r.product = 0x1176;
    assert(!UVC15::eligible(r));
    r.vendor = 0x13D3;
    r.product = 0x56F7;
    assert(!UVC15::eligible(r));
    r.product = 0x56F8;
    assert(UVC15::eligible(r));

    // Request shape still enforced
    r.bRequest = 0x01;
    assert(!UVC15::eligible(r));
    r.bRequest = 0x81;
    r.bmRequestType = 0x21;
    assert(!UVC15::eligible(r));
    r.bmRequestType = 0xA1;
    r.synchronous = false;
    assert(!UVC15::eligible(r));
    r.synchronous = true;
    r.rawBufferPresent = false;
    assert(!UVC15::eligible(r));
    r.rawBufferPresent = true;
    r.descriptorBufferPresent = true;
    assert(!UVC15::eligible(r));
    r.descriptorBufferPresent = false;
    assert(UVC15::eligible(r));

    // Transfer acceptance: any non-zero length that fits the enlarged buffer.
    assert(UVC15::safeTransfer(34, UVC15::kMaxProbe));
    assert(UVC15::safeTransfer(48, UVC15::kMaxProbe));
    assert(UVC15::safeTransfer(1, UVC15::kMaxProbe));
    assert(!UVC15::safeTransfer(0, UVC15::kMaxProbe));
    assert(!UVC15::safeTransfer(UVC15::kMaxProbe + 1, UVC15::kMaxProbe));

    puts("UVC15 ov9734 policy tests: PASS");
}
