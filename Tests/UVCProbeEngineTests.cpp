#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../UVCProbeFix/UVCDescriptorParser.hpp"
#include "../UVCProbeFix/UVCProbeControl.hpp"
#include "../UVCProbeFix/UVCProbeEngine.hpp"

namespace {
std::vector<uint8_t> load(const char *path) {
    FILE *f=std::fopen(path,"rb"); assert(f); std::fseek(f,0,SEEK_END); long n=std::ftell(f); std::rewind(f); assert(n>=0);
    std::vector<uint8_t> v(static_cast<size_t>(n)); if(!v.empty()) assert(std::fread(v.data(),1,v.size(),f)==v.size()); std::fclose(f); return v;
}
UVC::ProbePacket packet(const std::vector<uint8_t>& b){ UVC::ProbePacket p{}; assert(UVC::parseProbePacket(b.data(),b.size(),p)==UVC::ProbeParseStatus::Ok); return p; }
void write32(std::vector<uint8_t>& b,size_t o,uint32_t v){ b[o]=v&255; b[o+1]=(v>>8)&255; b[o+2]=(v>>16)&255; b[o+3]=(v>>24)&255; }

UVC::DescriptorView view(const std::vector<uint8_t>& b,UVC::UsbSpeed speed=UVC::UsbSpeed::High){ UVC::DescriptorView v; assert(v.init(b.data(),b.size(),1,speed)==UVC::DescriptorStatus::Ok); return v; }

void testAsusValidUntouched(){
 auto db=load("Tests/Fixtures/ASUS-known-good-descriptors.bin"); auto v=view(db);
 auto pb=load("Tests/Fixtures/ASUS-known-good-probe.bin"); auto last=packet(pb); auto resp=packet(pb);
 auto r=UVC::validateProbe(v,last,resp); assert(r.overall==UVC::FieldStatus::Valid); assert(r.invalidMask==0U);
 UVC::ProbePacket repaired{}; assert(!UVC::buildFieldRepair(v,resp,r,repaired));
 assert(std::memcmp(resp.bytes,pb.data(),pb.size())==0);
}

void testLenovoMalformedIsDescriptorDetected(){
 auto db=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin"); auto v=view(db);
 auto sb=load("Tests/Fixtures/Lenovo-recovered-probe.bin"); sb[3]=4; write32(sb,22,0); auto last=packet(sb);
 auto rb=load("Tests/Fixtures/Lenovo-malformed-probe.bin"); auto resp=packet(rb);
 auto r=UVC::validateProbe(v,last,resp);
 assert(r.overall==UVC::FieldStatus::Repairable);
 assert((r.invalidMask & UVC::InvalidFrameSize)!=0U); assert((r.invalidMask & UVC::InvalidPayload)!=0U);
 assert(r.resolvedFrame.frameIndex==4); assert(r.provenFrameSize==614400U); assert(r.maxPayloadKnown && r.provenMaxPayload==3072U);
 UVC::ProbePacket repaired{}; assert(UVC::buildFieldRepair(v,resp,r,repaired));
 assert(repaired.frameIndex()==4); assert(repaired.maxVideoFrameSize()==614400U); assert(repaired.maxPayloadTransferSize()==3072U);
}

void testUnknownFormatFrameIntervalAreNeverGuessed(){
 auto db=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin"); auto v=view(db);
 auto base=load("Tests/Fixtures/Lenovo-recovered-probe.bin");
 for(int mode=0;mode<3;++mode){ auto b=base; if(mode==0)b[2]=9; if(mode==1)b[3]=9; if(mode==2)write32(b,4,500000U); auto p=packet(b); auto r=UVC::validateProbe(v,p,p); assert(r.overall==UVC::FieldStatus::Unknown); UVC::ProbePacket out{}; assert(!UVC::buildFieldRepair(v,p,r,out)); }
}

void testUncompressedZeroDescriptorSizeComputesSafely(){
 auto db=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin");
 UVC::DescriptorView before=view(db); UVC::FrameInfo f{}; assert(before.findFrame(1,1,f)==UVC::DescriptorStatus::Ok);
 write32(db,f.descriptorOffset+17,0); auto v=view(db);
 auto b=load("Tests/Fixtures/Lenovo-recovered-probe.bin"); write32(b,18,123U); auto p=packet(b);
 auto r=UVC::validateProbe(v,p,p); assert(r.overall==UVC::FieldStatus::Repairable); assert(r.provenFrameSize==614400U);
}

void testMjpegNonzeroFrameSizePreserved(){
 auto db=load("Tests/Fixtures/ASUS-known-good-descriptors.bin"); auto v=view(db);
 auto b=load("Tests/Fixtures/ASUS-known-good-probe.bin"); write32(b,18,1234567U); auto p=packet(b);
 auto r=UVC::validateProbe(v,p,p); assert(r.overall==UVC::FieldStatus::Valid); assert((r.invalidMask & UVC::InvalidFrameSize)==0U);
}

void testPayloadRules(){
 auto db=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin"); auto v=view(db);
 auto b=load("Tests/Fixtures/Lenovo-recovered-probe.bin");
 { auto x=b; write32(x,22,0xffff0c00U); auto p=packet(x); auto r=UVC::validateProbe(v,p,p); assert(r.overall==UVC::FieldStatus::Repairable); UVC::ProbePacket out{}; assert(UVC::buildFieldRepair(v,p,r,out)); assert(out.maxPayloadTransferSize()==3072U); }
 { auto x=b; write32(x,22,9000U); auto p=packet(x); auto r=UVC::validateProbe(v,p,p); assert(r.overall==UVC::FieldStatus::Repairable); UVC::ProbePacket out{}; assert(UVC::buildFieldRepair(v,p,r,out)); assert(out.maxPayloadTransferSize()==3072U); }
 { auto x=b; write32(x,22,0U); auto p=packet(x); auto r=UVC::validateProbe(v,p,p); assert(r.overall==UVC::FieldStatus::Unknown); }
 { auto x=b; write32(x,22,0xffff0000U); auto p=packet(x); auto r=UVC::validateProbe(v,p,p); assert(r.overall==UVC::FieldStatus::Unknown); }
}

void testUnknownCapacityFailsClosed(){
 auto db=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin"); auto v=view(db,UVC::UsbSpeed::Unknown);
 auto b=load("Tests/Fixtures/Lenovo-recovered-probe.bin"); auto p=packet(b); auto r=UVC::validateProbe(v,p,p);
 assert(r.overall==UVC::FieldStatus::Unknown); assert(!r.maxPayloadKnown); assert((r.invalidMask & UVC::InvalidPayload)!=0U);
}

void testEquivalentFrameSelection(){
 auto db=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin"); auto v=view(db); UVC::FrameInfo src{},cand{};
 assert(v.findFrame(1,4,src)==UVC::DescriptorStatus::Ok);
 assert(v.findEquivalentFrame(src,333333U,cand)==UVC::DescriptorStatus::Ok); assert(cand.frameIndex==1U);

 auto cross=load("Tests/Fixtures/synthetic-descriptors-cross-format.bin"); auto cv=view(cross); assert(cv.findFrame(1,1,src)==UVC::DescriptorStatus::Ok);
 assert(cv.findEquivalentFrame(src,333333U,cand)==UVC::DescriptorStatus::NotFound);

 auto interval=load("Tests/Fixtures/synthetic-equivalent-interval.bin"); auto iv=view(interval); assert(iv.findFrame(1,4,src)==UVC::DescriptorStatus::Ok);
 assert(iv.findEquivalentFrame(src,500000U,cand)==UVC::DescriptorStatus::NotFound);

 auto def=load("Tests/Fixtures/synthetic-equivalent-default2.bin"); auto dv=view(def); assert(dv.findFrame(1,4,src)==UVC::DescriptorStatus::Ok);
 assert(dv.findEquivalentFrame(src,333333U,cand)==UVC::DescriptorStatus::Ok); assert(cand.frameIndex==2U);
 auto low=load("Tests/Fixtures/synthetic-equivalent-lowest.bin"); auto lv=view(low); assert(lv.findFrame(1,4,src)==UVC::DescriptorStatus::Ok);
 assert(lv.findEquivalentFrame(src,333333U,cand)==UVC::DescriptorStatus::Ok); assert(cand.frameIndex==1U);
}

void testRecoveryActionPrecedence(){
 auto db=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin"); auto v=view(db);
 auto goodb=load("Tests/Fixtures/Lenovo-recovered-probe.bin"); auto good=packet(goodb); auto valid=UVC::validateProbe(v,good,good); UVC::FrameInfo c{};
 assert(UVC::chooseRecoveryAction(v,good,good,valid,&c)==UVC::RecoveryAction::PassThrough);

 auto setb=goodb; setb[3]=4; write32(setb,22,0); auto last=packet(setb); auto badb=load("Tests/Fixtures/Lenovo-malformed-probe.bin"); auto bad=packet(badb); auto r=UVC::validateProbe(v,last,bad);
 assert(UVC::chooseRecoveryAction(v,last,bad,r,&c)==UVC::RecoveryAction::RetryEquivalent); assert(c.frameIndex==1U);

 auto noeq=load("Tests/Fixtures/synthetic-descriptors-mixed.bin"); auto nv=view(noeq); auto ub=goodb; ub[3]=1; write32(ub,18,1U); write32(ub,22,9000U); auto up=packet(ub); auto ur=UVC::validateProbe(nv,up,up);
 assert(ur.overall==UVC::FieldStatus::Repairable); assert(UVC::chooseRecoveryAction(nv,up,up,ur,&c)==UVC::RecoveryAction::FieldRepair);
}

void testEquivalentRetrySetPreservesEverythingElse(){
 auto db=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin"); auto v=view(db); UVC::FrameInfo src{},cand{};
 assert(v.findFrame(1,4,src)==UVC::DescriptorStatus::Ok); assert(v.findEquivalentFrame(src,333333U,cand)==UVC::DescriptorStatus::Ok);
 auto base=load("Tests/Fixtures/Lenovo-recovered-probe.bin"); base.resize(48); for(size_t i=26;i<48;++i) base[i]=static_cast<uint8_t>(0x80+i); base[3]=4; write32(base,22,777U);
 auto last=packet(base); UVC::ProbePacket retry{}; assert(UVC::buildEquivalentRetrySet(last,cand,614400U,retry));
 assert(retry.frameIndex()==1U); assert(retry.maxVideoFrameSize()==614400U); assert(retry.maxPayloadTransferSize()==0U);
 for(size_t i=0;i<48;++i){ bool changed=(i==3)||(i>=18&&i<26); if(!changed) assert(retry.bytes[i]==last.bytes[i]); }
 assert(std::memcmp(retry.bytes+26,last.bytes+26,22)==0);
}

struct FakeTransport {
    int setCalls;
    int getCalls;
    UVC::TransportStatus setStatus;
    UVC::TransportStatus getStatus;
    std::vector<uint8_t> getBytes;
    size_t forcedLength;
    uint8_t lastSet[48];
    size_t lastSetLength;
};
UVC::TransportStatus fakeSet(void *ctx,const uint8_t *data,size_t length){
    auto *f=static_cast<FakeTransport*>(ctx); ++f->setCalls; f->lastSetLength=length; std::memset(f->lastSet,0,sizeof(f->lastSet)); if(data&&length<=sizeof(f->lastSet))std::memcpy(f->lastSet,data,length); return f->setStatus;
}
UVC::TransportStatus fakeGet(void *ctx,uint8_t *out,size_t capacity,size_t *actual){
    auto *f=static_cast<FakeTransport*>(ctx); ++f->getCalls; if(f->getStatus!=UVC::TransportStatus::Success)return f->getStatus; size_t n=f->forcedLength?f->forcedLength:f->getBytes.size(); if(actual)*actual=n; if(n<=capacity && n<=f->getBytes.size())std::memcpy(out,f->getBytes.data(),n); return UVC::TransportStatus::Success;
}
UVC::ProbeTransportOps ops(FakeTransport &f){ return UVC::ProbeTransportOps{&f,fakeSet,fakeGet}; }

void testExecuteRecoveryTransactions(){
 auto db=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin"); auto v=view(db);
 auto goodb=load("Tests/Fixtures/Lenovo-recovered-probe.bin"); auto good=packet(goodb);
 FakeTransport f{0,0,UVC::TransportStatus::Success,UVC::TransportStatus::Success,goodb,0,{0},0};
 auto rr=UVC::executeRecovery(v,good,good,ops(f)); assert(rr.action==UVC::RecoveryAction::PassThrough); assert(!rr.mutated && f.setCalls==0 && f.getCalls==0); assert(std::memcmp(rr.callerVisible.bytes,good.bytes,good.length)==0);

 auto setb=goodb; setb[3]=4; write32(setb,22,0); auto last=packet(setb); auto badb=load("Tests/Fixtures/Lenovo-malformed-probe.bin"); auto bad=packet(badb);
 f={0,0,UVC::TransportStatus::Success,UVC::TransportStatus::Success,goodb,0,{0},0}; rr=UVC::executeRecovery(v,last,bad,ops(f));
 assert(rr.action==UVC::RecoveryAction::RetryEquivalent); assert(rr.mutated && rr.internalRetryAttempted && rr.internalRetrySucceeded); assert(f.setCalls==1 && f.getCalls==1); assert(f.lastSetLength==last.length); assert(f.lastSet[3]==1); assert(rr.callerVisible.frameIndex()==1 && rr.callerVisible.maxPayloadTransferSize()==3072U);
}

void testFieldRepairNeedsNoTransport(){
 auto db=load("Tests/Fixtures/synthetic-descriptors-mixed.bin"); auto v=view(db); auto b=load("Tests/Fixtures/Lenovo-recovered-probe.bin"); b[3]=1; write32(b,18,1); write32(b,22,9000); auto p=packet(b);
 FakeTransport f{0,0,UVC::TransportStatus::Success,UVC::TransportStatus::Success,{},0,{0},0}; auto rr=UVC::executeRecovery(v,p,p,ops(f));
 assert(rr.action==UVC::RecoveryAction::FieldRepair); assert(rr.mutated); assert(!rr.internalRetryAttempted); assert(f.setCalls==0&&f.getCalls==0); assert(rr.callerVisible.maxVideoFrameSize()==153600U); assert(rr.callerVisible.maxPayloadTransferSize()==3072U);
}

void testRetryFailuresReturnOriginalAndNeverLoop(){
 auto db=load("Tests/Fixtures/Lenovo-174F1176-descriptors.bin"); auto v=view(db); auto goodb=load("Tests/Fixtures/Lenovo-recovered-probe.bin"); auto setb=goodb; setb[3]=4; write32(setb,22,0); auto last=packet(setb); auto badb=load("Tests/Fixtures/Lenovo-malformed-probe.bin"); auto bad=packet(badb);
 {
   FakeTransport f{0,0,UVC::TransportStatus::Error,UVC::TransportStatus::Success,goodb,0,{0},0}; auto rr=UVC::executeRecovery(v,last,bad,ops(f)); assert(!rr.mutated&&!rr.internalRetrySucceeded&&rr.internalRetryAttempted); assert(f.setCalls==1&&f.getCalls==0); assert(std::memcmp(rr.callerVisible.bytes,bad.bytes,bad.length)==0);
 }
 {
   FakeTransport f{0,0,UVC::TransportStatus::Success,UVC::TransportStatus::Success,goodb,25,{0},0}; auto rr=UVC::executeRecovery(v,last,bad,ops(f)); assert(!rr.mutated&&!rr.internalRetrySucceeded); assert(f.setCalls==1&&f.getCalls==1); assert(std::memcmp(rr.callerVisible.bytes,bad.bytes,bad.length)==0);
 }
 {
   auto still=goodb; write32(still,22,9000U); FakeTransport f{0,0,UVC::TransportStatus::Success,UVC::TransportStatus::Success,still,0,{0},0}; auto rr=UVC::executeRecovery(v,last,bad,ops(f)); assert(!rr.mutated&&!rr.internalRetrySucceeded); assert(f.setCalls==1&&f.getCalls==1); assert(std::memcmp(rr.callerVisible.bytes,bad.bytes,bad.length)==0);
 }
}
}
int main(){ testAsusValidUntouched(); testLenovoMalformedIsDescriptorDetected(); testUnknownFormatFrameIntervalAreNeverGuessed(); testUncompressedZeroDescriptorSizeComputesSafely(); testMjpegNonzeroFrameSizePreserved(); testPayloadRules(); testUnknownCapacityFailsClosed(); testEquivalentFrameSelection(); testRecoveryActionPrecedence(); testEquivalentRetrySetPreservesEverythingElse(); testExecuteRecoveryTransactions(); testFieldRepairNeedsNoTransport(); testRetryFailuresReturnOriginalAndNeverLoop(); std::puts("UVCProbeEngineTests: PASS"); }
