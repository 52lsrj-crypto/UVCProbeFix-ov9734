#include "../UVCProbeFix/UVC15ProbeOverrun.hpp"
#include <cassert>
#include <cstdio>
int main() {
    UVC15::Request r {0x04F2,0xB6BF,0xA1,0x81,0x0100,1,34,true,true,false};
    assert(UVC15::eligible(r));
    r.wIndex=3; assert(UVC15::eligible(r));
    r.wValue=0x0200; assert(UVC15::eligible(r));
    r.wValue=0x0300; assert(!UVC15::eligible(r)); r.wValue=0x0100;
    r.wIndex=0; assert(!UVC15::eligible(r)); r.wIndex=2; assert(!UVC15::eligible(r)); r.wIndex=1;
    r.vendor=0x174F; assert(!UVC15::eligible(r)); r.vendor=0x04F2;
    r.product=0xB6BE; assert(!UVC15::eligible(r)); r.product=0xB6BF;
    r.wLength=48; assert(!UVC15::eligible(r)); r.wLength=26; assert(!UVC15::eligible(r)); r.wLength=34;
    r.bRequest=0x01; assert(!UVC15::eligible(r)); r.bRequest=0x81;
    r.bmRequestType=0x21; assert(!UVC15::eligible(r)); r.bmRequestType=0xA1;
    r.synchronous=false; assert(!UVC15::eligible(r)); r.synchronous=true;
    r.rawBufferPresent=false; assert(!UVC15::eligible(r)); r.rawBufferPresent=true;
    r.descriptorBufferPresent=true; assert(!UVC15::eligible(r)); r.descriptorBufferPresent=false;
    assert(UVC15::eligible(r));
    assert(UVC15::safeTransfer(48,48));
    assert(!UVC15::safeTransfer(34,48));
    assert(!UVC15::safeTransfer(49,48));
    assert(!UVC15::safeTransfer(48,34));
    puts("UVC15 integrated policy tests: PASS");
}
