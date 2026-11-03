#include "UVCRecoveryContext.hpp"

#include <string.h>

namespace UVC {

RecoveryContextTable::RecoveryContextTable() { reset(); }

bool RecoveryContextTable::identityEqual(const DeviceIdentity &a, const DeviceIdentity &b) {
    return a.vendorId == b.vendorId && a.productId == b.productId &&
           a.bcdDevice == b.bcdDevice && a.configurationValue == b.configurationValue &&
           a.interfaceNumber == b.interfaceNumber;
}

void RecoveryContextTable::resetSlot(size_t index, bool preserveSequence) {
    const uint64_t sequence = preserveSequence ? slots_[index].sequence : 0U;
    memset(&slots_[index], 0, sizeof(slots_[index]));
    slots_[index].state = CycleState::Idle;
    slots_[index].sequence = sequence;
}

void RecoveryContextTable::reset() {
    for (size_t i = 0; i < kCapacity; ++i) resetSlot(i, false);
}

int RecoveryContextTable::findExact(const void *interfaceKey, const DeviceIdentity &identity) const {
    if (interfaceKey == nullptr) return -1;
    for (size_t i = 0; i < kCapacity; ++i) {
        if (slots_[i].occupied && slots_[i].interfaceKey == interfaceKey && identityEqual(slots_[i].identity, identity)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int RecoveryContextTable::findByKey(const void *interfaceKey) const {
    if (interfaceKey == nullptr) return -1;
    for (size_t i = 0; i < kCapacity; ++i) {
        if (slots_[i].occupied && slots_[i].interfaceKey == interfaceKey) return static_cast<int>(i);
    }
    return -1;
}

int RecoveryContextTable::chooseReusableSlot() const {
    for (size_t i = 0; i < kCapacity; ++i) {
        if (!slots_[i].occupied) return static_cast<int>(i);
    }
    for (size_t i = 0; i < kCapacity; ++i) {
        if (slots_[i].state == CycleState::Idle || slots_[i].state == CycleState::Recovered || slots_[i].state == CycleState::Failed) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool RecoveryContextTable::fillTicket(size_t index, ContextTicket &ticket) const {
    if (index >= kCapacity || !slots_[index].occupied) return false;
    ticket.slot = static_cast<uint8_t>(index);
    ticket.sequence = slots_[index].sequence;
    ticket.lastSet = slots_[index].lastSet;
    return true;
}

bool RecoveryContextTable::recordExternalSet(const void *interfaceKey,
                                             const DeviceIdentity &identity,
                                             const ProbePacket &setProbe,
                                             ContextTicket &ticket) {
    if (interfaceKey == nullptr || !isCanonicalProbeLength(setProbe.length)) return false;
    int index = findExact(interfaceKey, identity);
    if (index < 0) {
        const int sameKey = findByKey(interfaceKey);
        if (sameKey >= 0) {
            index = sameKey;
            resetSlot(static_cast<size_t>(index), true);
        } else {
            index = chooseReusableSlot();
            if (index < 0) return false;
            resetSlot(static_cast<size_t>(index), true);
        }
        slots_[index].occupied = true;
        slots_[index].interfaceKey = interfaceKey;
        slots_[index].identity = identity;
    }

    Slot &slot = slots_[index];
    if (slot.sequence == UINT64_MAX) slot.sequence = 1U;
    else ++slot.sequence;
    if (slot.sequence == 0U) slot.sequence = 1U;
    slot.lastSet = setProbe;
    slot.state = CycleState::HaveSet;
    slot.internalRequest = false;
    return fillTicket(static_cast<size_t>(index), ticket);
}

bool RecoveryContextTable::snapshotForGet(const void *interfaceKey,
                                          const DeviceIdentity &identity,
                                          ContextTicket &ticket) {
    const int index = findExact(interfaceKey, identity);
    if (index < 0) return false;
    Slot &slot = slots_[index];
    if (slot.state != CycleState::HaveSet) return false;
    slot.state = CycleState::Validating;
    return fillTicket(static_cast<size_t>(index), ticket);
}

bool RecoveryContextTable::setInternal(const ContextTicket &ticket, bool internal) {
    if (!isCurrent(ticket)) return false;
    slots_[ticket.slot].internalRequest = internal;
    return true;
}

bool RecoveryContextTable::isInternal(const void *interfaceKey, const DeviceIdentity &identity) const {
    const int index = findExact(interfaceKey, identity);
    return index >= 0 && slots_[index].internalRequest;
}

bool RecoveryContextTable::isCurrent(const ContextTicket &ticket) const {
    if (ticket.slot >= kCapacity) return false;
    const Slot &slot = slots_[ticket.slot];
    return slot.occupied && slot.sequence != 0U && slot.sequence == ticket.sequence;
}

bool RecoveryContextTable::finish(const ContextTicket &ticket, CycleState finalState) {
    if (!isCurrent(ticket)) return false;
    if (finalState != CycleState::Idle && finalState != CycleState::Recovered && finalState != CycleState::Failed) return false;
    Slot &slot = slots_[ticket.slot];
    slot.state = finalState;
    slot.internalRequest = false;
    return true;
}

} // namespace UVC
