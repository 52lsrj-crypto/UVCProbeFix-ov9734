#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "../UVCProbeFix/UVCRuntimeMode.hpp"
#include "../UVCProbeFix/UVCTransportTahoe.hpp"
#include "../UVCProbeFix/UVCQuirks.hpp"

namespace {
using UVC::BootArgState; using UVC::RuntimeMode;
void testReleasePreservesKnownWorkingLegacyBeforeGenericHardwareGate(){
 assert(UVC::selectRuntimeMode(BootArgState{},false,false)==RuntimeMode::Legacy0215);
 assert(UVC::selectRuntimeMode(BootArgState{},false,true)==RuntimeMode::GenericFull);
}
void testDebugDefaultsAndPhaseGates(){
 assert(UVC::selectRuntimeMode(BootArgState{},true,false)==RuntimeMode::GenericObserve);
 assert(UVC::selectRuntimeMode(BootArgState{false,false,true,false,false},true,false)==RuntimeMode::GenericObserve);
 assert(UVC::selectRuntimeMode(BootArgState{false,false,false,true,false},true,false)==RuntimeMode::GenericFieldRepairDev);
 assert(UVC::selectRuntimeMode(BootArgState{false,false,false,false,true},true,false)==RuntimeMode::GenericFull);
}
void testOffAndLegacyArbitration(){
 assert(UVC::selectRuntimeMode(BootArgState{true,true,false,false,false},true,false)==RuntimeMode::Off);
 assert(UVC::selectRuntimeMode(BootArgState{false,true,false,false,false},true,false)==RuntimeMode::Legacy0215);
 assert(UVC::selectRuntimeMode(BootArgState{false,true,false,false,false},false,true)==RuntimeMode::Legacy0215);
}
void testDebugConflictsFailClosed(){
 assert(UVC::selectRuntimeMode(BootArgState{false,false,true,true,false},true,false)==RuntimeMode::Off);
 assert(UVC::selectRuntimeMode(BootArgState{false,false,false,true,true},true,false)==RuntimeMode::Off);
 assert(UVC::selectRuntimeMode(BootArgState{false,true,true,false,false},true,false)==RuntimeMode::Off);
}
void testReleaseIgnoresDebugOnlySwitches(){
 assert(UVC::selectRuntimeMode(BootArgState{false,false,true,true,true},false,false)==RuntimeMode::Legacy0215);
 assert(UVC::selectRuntimeMode(BootArgState{false,false,true,true,true},false,true)==RuntimeMode::GenericFull);
 assert(UVC::selectRuntimeMode(BootArgState{false,true,true,true,true},false,true)==RuntimeMode::Legacy0215);
}

void testGenericQuirkLayerIsEmpty(){
 const UVC::DeviceIdentity a{0x174F,0x1176,0x0100,1,1};
 const UVC::DeviceIdentity b{0x322E,0x2103,0x0100,1,1};
 const UVC::DeviceIdentity c{0x1234,0x5678,0x9999,2,7};
 assert(!UVC::hasRecoveryQuirk(a));
 assert(!UVC::hasRecoveryQuirk(b));
 assert(!UVC::hasRecoveryQuirk(c));
}

void testExternalSetSnapshotIsTakenBeforeTransportCanTouchCallerBytes(){
 uint8_t bytes[26] {};
 bytes[2]=1; bytes[3]=4;
 UVC::ProbePacket snapshot {};
 assert(UVC::snapshotExternalSetProbe(UVC::RequestKind::SetProbe,true,bytes,sizeof(bytes),snapshot));
 bytes[3]=9;
 assert(snapshot.formatIndex()==1);
 assert(snapshot.frameIndex()==4);
 assert(!UVC::snapshotExternalSetProbe(UVC::RequestKind::GetProbe,true,bytes,sizeof(bytes),snapshot));
 assert(!UVC::snapshotExternalSetProbe(UVC::RequestKind::SetProbe,false,bytes,sizeof(bytes),snapshot));
}

void testInternalGuardNeverHidesExternalSet(){
 assert(!UVC::shouldBypassDuringInternal(UVC::RequestKind::SetProbe));
 assert(UVC::shouldBypassDuringInternal(UVC::RequestKind::GetProbe));
 assert(UVC::shouldBypassDuringInternal(UVC::RequestKind::CommitProbe));
 assert(UVC::shouldBypassDuringInternal(UVC::RequestKind::Other));
}

void testTahoeDarwinVersionGate(){
 assert(UVC::isSupportedTahoeDarwinVersion(25U,6U));
 assert(!UVC::isSupportedTahoeDarwinVersion(25U,5U));
 assert(!UVC::isSupportedTahoeDarwinVersion(25U,7U));
 assert(!UVC::isSupportedTahoeDarwinVersion(24U,6U));
 assert(!UVC::isSupportedTahoeDarwinVersion(26U,6U));
}

void testTahoeUsbSpeedMappingFailsClosedForSuperSpeedPlus(){
 assert(UVC::mapTahoeUsbSpeed(0U)==UVC::UsbSpeed::Unknown);
 assert(UVC::mapTahoeUsbSpeed(1U)==UVC::UsbSpeed::Full);
 assert(UVC::mapTahoeUsbSpeed(2U)==UVC::UsbSpeed::High);
 assert(UVC::mapTahoeUsbSpeed(3U)==UVC::UsbSpeed::Super);
 assert(UVC::mapTahoeUsbSpeed(4U)==UVC::UsbSpeed::Unknown);
 assert(UVC::mapTahoeUsbSpeed(5U)==UVC::UsbSpeed::Unknown);
}

void testCompleteProbeTransferRequiresAllRequestedBytes(){
 assert(UVC::isCompleteProbeTransfer(26U,26U));
 assert(UVC::isCompleteProbeTransfer(34U,34U));
 assert(UVC::isCompleteProbeTransfer(48U,48U));
 assert(!UVC::isCompleteProbeTransfer(26U,0U));
 assert(!UVC::isCompleteProbeTransfer(26U,25U));
 assert(!UVC::isCompleteProbeTransfer(26U,27U));
}

void testStrictRequestClassifier(){
 const UVC::InterfaceView vs{14,2,1}; bool mutableLength=false;
 assert(UVC::classifyRequest(vs,UVC::ControlRequestView{0x21,0x01,0x0100,0x0001,26},26,mutableLength)==UVC::RequestKind::SetProbe && mutableLength);
 assert(UVC::classifyRequest(vs,UVC::ControlRequestView{0xA1,0x81,0x0100,0x0001,34},34,mutableLength)==UVC::RequestKind::GetProbe && mutableLength);
 assert(UVC::classifyRequest(vs,UVC::ControlRequestView{0x21,0x01,0x0200,0x0001,48},48,mutableLength)==UVC::RequestKind::CommitProbe && mutableLength);

 assert(UVC::classifyRequest(UVC::InterfaceView{14,1,1},UVC::ControlRequestView{0x21,0x01,0x0100,1,26},26,mutableLength)==UVC::RequestKind::Other);
 assert(UVC::classifyRequest(vs,UVC::ControlRequestView{0x20,0x01,0x0100,1,26},26,mutableLength)==UVC::RequestKind::Other);
 assert(UVC::classifyRequest(vs,UVC::ControlRequestView{0x21,0x81,0x0100,1,26},26,mutableLength)==UVC::RequestKind::Other);
 assert(UVC::classifyRequest(vs,UVC::ControlRequestView{0x21,0x01,0x0300,1,26},26,mutableLength)==UVC::RequestKind::Other);
 assert(UVC::classifyRequest(vs,UVC::ControlRequestView{0x21,0x01,0x0101,1,26},26,mutableLength)==UVC::RequestKind::Other);
 assert(UVC::classifyRequest(vs,UVC::ControlRequestView{0x21,0x01,0x0100,2,26},26,mutableLength)==UVC::RequestKind::Other);
 assert(UVC::classifyRequest(vs,UVC::ControlRequestView{0x21,0x01,0x0100,0x0101,26},26,mutableLength)==UVC::RequestKind::Other);
 assert(UVC::classifyRequest(vs,UVC::ControlRequestView{0x21,0x01,0x0100,1,25},25,mutableLength)==UVC::RequestKind::Other);
}
void testNoncanonicalProbeLengthsAreObservationOnly(){
 const UVC::InterfaceView vs{14,2,1};
 for(size_t n : {size_t(27),size_t(33),size_t(35),size_t(47),size_t(49),size_t(64)}){
   bool mut=true; auto k=UVC::classifyRequest(vs,UVC::ControlRequestView{0xA1,0x81,0x0100,1,static_cast<uint16_t>(n)},n,mut);
   assert(k==UVC::RequestKind::GetProbe); assert(!mut);
 }
}
}
int main(){ testReleasePreservesKnownWorkingLegacyBeforeGenericHardwareGate(); testGenericQuirkLayerIsEmpty(); testDebugDefaultsAndPhaseGates(); testOffAndLegacyArbitration(); testDebugConflictsFailClosed(); testReleaseIgnoresDebugOnlySwitches(); testExternalSetSnapshotIsTakenBeforeTransportCanTouchCallerBytes(); testInternalGuardNeverHidesExternalSet(); testTahoeDarwinVersionGate(); testTahoeUsbSpeedMappingFailsClosedForSuperSpeedPlus(); testCompleteProbeTransferRequiresAllRequestedBytes(); testStrictRequestClassifier(); testNoncanonicalProbeLengthsAreObservationOnly(); std::puts("UVCTransportPolicyTests: PASS"); }
