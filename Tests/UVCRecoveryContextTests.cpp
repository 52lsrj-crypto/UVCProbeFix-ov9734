#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../UVCProbeFix/UVCProbeControl.hpp"
#include "../UVCProbeFix/UVCRecoveryContext.hpp"

namespace {
UVC::ProbePacket makeProbe(uint8_t frame){ uint8_t b[26]{}; b[2]=1;b[3]=frame;b[4]=0x15;b[5]=0x16;b[6]=0x05;b[18]=0;b[19]=0x60;b[20]=0x09;b[22]=0;b[23]=0x0c; UVC::ProbePacket p{}; assert(UVC::parseProbePacket(b,sizeof(b),p)==UVC::ProbeParseStatus::Ok); return p; }
UVC::DeviceIdentity id(uint16_t v,uint16_t p,uint8_t i=1){ return UVC::DeviceIdentity{v,p,0x0100,1,i}; }

void testSetSnapshotSequenceAndFinish(){
 UVC::RecoveryContextTable t; int key=0; UVC::ContextTicket a{},b{};
 assert(t.recordExternalSet(&key,id(1,2),makeProbe(4),a)); assert(a.sequence!=0); assert(a.lastSet.frameIndex()==4);
 assert(t.snapshotForGet(&key,id(1,2),b)); assert(b.slot==a.slot && b.sequence==a.sequence && b.lastSet.frameIndex()==4);
 assert(t.isCurrent(b)); assert(t.finish(b,UVC::CycleState::Failed));
 UVC::ContextTicket c{}; assert(t.recordExternalSet(&key,id(1,2),makeProbe(1),c)); assert(c.sequence>a.sequence); assert(!t.isCurrent(a)); assert(c.lastSet.frameIndex()==1);
}

void testStaleTicketRejectedAfterOverlappingSet(){
 UVC::RecoveryContextTable t; int key=0; UVC::ContextTicket a{},snap{},newer{};
 assert(t.recordExternalSet(&key,id(1,2),makeProbe(4),a)); assert(t.snapshotForGet(&key,id(1,2),snap));
 assert(t.recordExternalSet(&key,id(1,2),makeProbe(1),newer)); assert(!t.isCurrent(snap)); assert(!t.finish(snap,UVC::CycleState::Recovered));
}

void testInternalIsPerInterface(){
 UVC::RecoveryContextTable t; int akey=0,bkey=0; UVC::ContextTicket a{},b{};
 assert(t.recordExternalSet(&akey,id(1,2,1),makeProbe(4),a)); assert(t.recordExternalSet(&bkey,id(1,3,2),makeProbe(4),b));
 assert(t.setInternal(a,true)); assert(t.isInternal(&akey,id(1,2,1))); assert(!t.isInternal(&bkey,id(1,3,2))); assert(t.setInternal(a,false)); assert(!t.isInternal(&akey,id(1,2,1)));
}

void testPointerReuseWithDifferentIdentityInvalidatesOldTicket(){
 UVC::RecoveryContextTable t; int key=0; UVC::ContextTicket old{},fresh{};
 assert(t.recordExternalSet(&key,id(0x1111,0x2222),makeProbe(4),old));
 assert(t.recordExternalSet(&key,id(0x3333,0x4444),makeProbe(1),fresh));
 assert(fresh.slot==old.slot); assert(fresh.sequence>old.sequence); assert(!t.isCurrent(old)); assert(t.isCurrent(fresh));
}

void testTableFullDoesNotEvictActiveButReusesTerminal(){
 UVC::RecoveryContextTable t; int keys[17]{}; UVC::ContextTicket tickets[17]{};
 for(size_t i=0;i<UVC::RecoveryContextTable::kCapacity;++i) assert(t.recordExternalSet(&keys[i],id(1,static_cast<uint16_t>(100+i),static_cast<uint8_t>(i)),makeProbe(4),tickets[i]));
 assert(!t.recordExternalSet(&keys[16],id(9,9),makeProbe(4),tickets[16]));
 UVC::ContextTicket snap{}; assert(t.snapshotForGet(&keys[3],id(1,103,3),snap)); assert(t.finish(snap,UVC::CycleState::Failed));
 assert(t.recordExternalSet(&keys[16],id(9,9),makeProbe(1),tickets[16])); assert(tickets[16].slot==tickets[3].slot);
 assert(!t.isCurrent(tickets[3]));
}

void testResetClearsEverything(){
 UVC::RecoveryContextTable t; int key=0; UVC::ContextTicket a{}; assert(t.recordExternalSet(&key,id(1,2),makeProbe(4),a)); t.reset(); assert(!t.isCurrent(a)); assert(!t.isInternal(&key,id(1,2)));
}
}
int main(){ testSetSnapshotSequenceAndFinish(); testStaleTicketRejectedAfterOverlappingSet(); testInternalIsPerInterface(); testPointerReuseWithDifferentIdentityInvalidatesOldTicket(); testTableFullDoesNotEvictActiveButReusesTerminal(); testResetClearsEverything(); std::puts("UVCRecoveryContextTests: PASS"); }
