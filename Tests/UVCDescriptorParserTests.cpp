#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../UVCProbeFix/UVCDescriptorParser.hpp"

namespace {
std::vector<uint8_t> load(const char *path) {
    FILE *f = std::fopen(path, "rb"); assert(f);
    std::fseek(f,0,SEEK_END); long n=std::ftell(f); std::rewind(f); assert(n>=0);
    std::vector<uint8_t> v(static_cast<size_t>(n));
    if (!v.empty()) assert(std::fread(v.data(),1,v.size(),f)==v.size());
    std::fclose(f); return v;
}

void testFindFramesAndFormats() {
    auto bytes=load("Tests/Fixtures/synthetic-descriptors-mixed.bin");
    UVC::DescriptorView v; assert(v.init(bytes.data(),bytes.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::Ok);
    UVC::FrameInfo f{};
    assert(v.findFrame(1,1,f)==UVC::DescriptorStatus::Ok);
    assert(f.kind==UVC::FormatKind::Uncompressed && f.width==320 && f.height==240 && f.maxFrameBufferSize==153600);
    assert(v.findFrame(2,1,f)==UVC::DescriptorStatus::Ok);
    assert(f.kind==UVC::FormatKind::Mjpeg && f.width==640 && f.height==480);
    assert(v.findFrame(3,1,f)==UVC::DescriptorStatus::Ok);
    assert(f.kind==UVC::FormatKind::FrameBased && f.width==800 && f.height==600);
    assert(v.findFrame(9,1,f)==UVC::DescriptorStatus::NotFound);
}

void testDiscreteAndContinuousIntervals() {
    auto d=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin");
    UVC::DescriptorView dv; assert(dv.init(d.data(),d.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::Ok);
    UVC::FrameInfo f{}; assert(dv.findFrame(1,4,f)==UVC::DescriptorStatus::Ok);
    assert(dv.supportsInterval(f,333333)==UVC::DescriptorStatus::Ok);
    assert(dv.supportsInterval(f,500000)==UVC::DescriptorStatus::Unsupported);

    auto c=load("Tests/Fixtures/synthetic-descriptors-continuous.bin");
    UVC::DescriptorView cv; assert(cv.init(c.data(),c.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::Ok);
    assert(cv.findFrame(1,1,f)==UVC::DescriptorStatus::Ok);
    assert(cv.supportsInterval(f,333333)==UVC::DescriptorStatus::Ok);
    assert(cv.supportsInterval(f,666666)==UVC::DescriptorStatus::Ok);
    assert(cv.supportsInterval(f,500000)==UVC::DescriptorStatus::Unsupported);
    assert(cv.supportsInterval(f,1000000)==UVC::DescriptorStatus::Unsupported); // not aligned to 333333 step
}

void testDuplicateFrameAndCrossFormatSeparation() {
    auto b=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin");
    UVC::DescriptorView v; assert(v.init(b.data(),b.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::Ok);
    UVC::FrameInfo a{},d{};
    assert(v.findFrame(1,1,a)==UVC::DescriptorStatus::Ok);
    assert(v.findFrame(1,4,d)==UVC::DescriptorStatus::Ok);
    assert(a.width==d.width && a.height==d.height && a.formatIndex==d.formatIndex && a.frameIndex!=d.frameIndex);
    assert(a.defaultFrameIndex==1 && d.defaultFrameIndex==1);

    auto x=load("Tests/Fixtures/synthetic-descriptors-cross-format.bin");
    UVC::DescriptorView xv; assert(xv.init(x.data(),x.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::Ok);
    UVC::FrameInfo x1{},x2{};
    assert(xv.findFrame(1,1,x1)==UVC::DescriptorStatus::Ok);
    assert(xv.findFrame(2,1,x2)==UVC::DescriptorStatus::Ok);
    assert(x1.width==x2.width && x1.height==x2.height && x1.formatIndex!=x2.formatIndex);
}

void testMalformedConfigurationFailsClosed() {
    auto good=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin");
    {
        auto b=good; b[9]=0;
        UVC::DescriptorView v; assert(v.init(b.data(),b.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::Malformed);
    }
    {
        auto b=good; b[2]=0xff; b[3]=0x7f;
        UVC::DescriptorView v; assert(v.init(b.data(),b.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::BoundsExceeded);
    }
    {
        auto b=good; b.resize(12);
        UVC::DescriptorView v; assert(v.init(b.data(),b.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::Malformed);
    }
    {
        std::vector<uint8_t> b(4097,0); b[0]=9; b[1]=2; b[2]=1; b[3]=16;
        UVC::DescriptorView v; assert(v.init(b.data(),b.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::BoundsExceeded);
    }
    {
        std::vector<uint8_t> parts; parts.insert(parts.end(),{9,2,0,0,1,1,0,0x80,50});
        for (int i=0;i<513;i++) { parts.push_back(2); parts.push_back(0xff); }
        const uint16_t total=static_cast<uint16_t>(parts.size()); parts[2]=static_cast<uint8_t>(total); parts[3]=static_cast<uint8_t>(total>>8);
        UVC::DescriptorView v; assert(v.init(parts.data(),parts.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::BoundsExceeded);
    }
}

void testTruncatedFrameAndInvalidContinuousRange() {
    auto b=load("Tests/Fixtures/synthetic-descriptors-continuous.bin");
    UVC::DescriptorView v; assert(v.init(b.data(),b.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::Ok);
    UVC::FrameInfo f{}; assert(v.findFrame(1,1,f)==UVC::DescriptorStatus::Ok);
    auto bad=b;
    const size_t o=f.descriptorOffset;
    bad[o]=25; // too short for frame descriptor
    UVC::DescriptorView tv; assert(tv.init(bad.data(),bad.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::Malformed);

    bad=b;
    // continuous min > max; intervals start at frame offset + 26
    bad[o+26]=0x40; bad[o+27]=0x42; bad[o+28]=0x0f; bad[o+29]=0; // 1,000,000 min
    bad[o+30]=0x15; bad[o+31]=0x16; bad[o+32]=0x05; bad[o+33]=0; // 333,333 max
    UVC::DescriptorView iv; assert(iv.init(bad.data(),bad.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::Malformed);
}

void testEndpointCapacity() {
    UVC::FrameInfo unused{}; (void)unused;
    struct Case { const char *path; UVC::UsbSpeed speed; UVC::DescriptorStatus status; uint32_t value; };
    const Case cases[] = {
        {"Tests/Fixtures/synthetic-endpoint-full.bin", UVC::UsbSpeed::Full, UVC::DescriptorStatus::Ok, 1023U},
        {"Tests/Fixtures/synthetic-endpoint-high-multi.bin", UVC::UsbSpeed::High, UVC::DescriptorStatus::Ok, 3072U},
        {"Tests/Fixtures/synthetic-endpoint-high-reserved.bin", UVC::UsbSpeed::High, UVC::DescriptorStatus::CapacityUnknown, 0U},
        {"Tests/Fixtures/synthetic-endpoint-ss.bin", UVC::UsbSpeed::Super, UVC::DescriptorStatus::Ok, 4096U},
        {"Tests/Fixtures/synthetic-endpoint-wrong.bin", UVC::UsbSpeed::High, UVC::DescriptorStatus::CapacityUnknown, 0U},
        {"Tests/Fixtures/synthetic-endpoint-noniso.bin", UVC::UsbSpeed::High, UVC::DescriptorStatus::CapacityUnknown, 0U},
        {"Tests/Fixtures/synthetic-endpoint-ss-badcomp.bin", UVC::UsbSpeed::Super, UVC::DescriptorStatus::CapacityUnknown, 0U},
        {"Tests/Fixtures/synthetic-endpoint-high-multi.bin", UVC::UsbSpeed::Unknown, UVC::DescriptorStatus::CapacityUnknown, 0U},
    };
    for (const auto &c : cases) {
        auto b=load(c.path); UVC::DescriptorView v;
        assert(v.init(b.data(),b.size(),1,c.speed)==UVC::DescriptorStatus::Ok);
        uint32_t value=0xdeadbeefU;
        assert(v.maxPeriodicPayload(value)==c.status);
        if (c.status==UVC::DescriptorStatus::Ok) assert(value==c.value);
        else assert(value==0U);
    }

    auto fullBad=load("Tests/Fixtures/synthetic-endpoint-high-multi.bin");
    UVC::DescriptorView fv; assert(fv.init(fullBad.data(),fullBad.size(),1,UVC::UsbSpeed::Full)==UVC::DescriptorStatus::Ok);
    uint32_t value=123; assert(fv.maxPeriodicPayload(value)==UVC::DescriptorStatus::CapacityUnknown); assert(value==0);
}
void testEndpointCapacityRejectsReservedPacketBits() {
    auto hs=load("Tests/Fixtures/synthetic-endpoint-high-multi.bin");
    // First alternate-setting endpoint starts at byte 40. Bits 15:13 are
    // reserved in wMaxPacketSize and must make physical capacity unknown.
    hs[44]=0x00; hs[45]=0x84; // 0x8400: valid 1024-byte base + reserved bit 15
    UVC::DescriptorView hv;
    assert(hv.init(hs.data(),hs.size(),1,UVC::UsbSpeed::High)==UVC::DescriptorStatus::Ok);
    uint32_t value=123U;
    assert(hv.maxPeriodicPayload(value)==UVC::DescriptorStatus::CapacityUnknown);
    assert(value==0U);

    auto fs=load("Tests/Fixtures/synthetic-endpoint-full.bin");
    fs[44]=0xff; fs[45]=0x83; // 0x83ff: 1023-byte base + reserved bit 15
    UVC::DescriptorView fv;
    assert(fv.init(fs.data(),fs.size(),1,UVC::UsbSpeed::Full)==UVC::DescriptorStatus::Ok);
    value=123U;
    assert(fv.maxPeriodicPayload(value)==UVC::DescriptorStatus::CapacityUnknown);
    assert(value==0U);
}

void testSuperSpeedCompanionMustProvePhysicalCapacity() {
    auto ss=load("Tests/Fixtures/synthetic-endpoint-ss.bin");
    UVC::DescriptorView valid;
    assert(valid.init(ss.data(),ss.size(),1,UVC::UsbSpeed::Super)==UVC::DescriptorStatus::Ok);
    uint32_t value=0U;
    assert(valid.maxPeriodicPayload(value)==UVC::DescriptorStatus::Ok);
    assert(value==4096U);

    // Same 4096-byte claim with only one 1024-byte packet allowed per
    // service interval is physically impossible and must fail closed.
    ss[49]=0U;
    UVC::DescriptorView impossible;
    assert(impossible.init(ss.data(),ss.size(),1,UVC::UsbSpeed::Super)==UVC::DescriptorStatus::Ok);
    value=123U;
    assert(impossible.maxPeriodicPayload(value)==UVC::DescriptorStatus::CapacityUnknown);
    assert(value==0U);

    // Mult encoding 3 is reserved for ordinary SuperSpeed isochronous EPs.
    ss=load("Tests/Fixtures/synthetic-endpoint-ss.bin");
    ss[50]=3U;
    UVC::DescriptorView badMult;
    assert(badMult.init(ss.data(),ss.size(),1,UVC::UsbSpeed::Super)==UVC::DescriptorStatus::Ok);
    value=123U;
    assert(badMult.maxPeriodicPayload(value)==UVC::DescriptorStatus::CapacityUnknown);
    assert(value==0U);
}

}

int main(){
 testFindFramesAndFormats(); testDiscreteAndContinuousIntervals(); testDuplicateFrameAndCrossFormatSeparation();
 testMalformedConfigurationFailsClosed(); testTruncatedFrameAndInvalidContinuousRange(); testEndpointCapacity(); testEndpointCapacityRejectsReservedPacketBits(); testSuperSpeedCompanionMustProvePhysicalCapacity();
 std::puts("UVCDescriptorParserTests: PASS"); return 0;
}
