#pragma once

#include <stddef.h>
#include <stdint.h>

#include "UVCProbeControl.hpp"

namespace UVC {

struct DeviceIdentity {
    uint16_t vendorId;
    uint16_t productId;
    uint16_t bcdDevice;
    uint8_t configurationValue;
    uint8_t interfaceNumber;
};

enum class CycleState : uint8_t { Idle, HaveSet, Validating, Recovered, Failed };

struct ContextTicket {
    uint8_t slot;
    uint64_t sequence;
    ProbePacket lastSet;
};

class RecoveryContextTable {
public:
    static constexpr size_t kCapacity = 16U;

    RecoveryContextTable();

    bool recordExternalSet(const void *interfaceKey,
                           const DeviceIdentity &identity,
                           const ProbePacket &setProbe,
                           ContextTicket &ticket);
    bool snapshotForGet(const void *interfaceKey,
                        const DeviceIdentity &identity,
                        ContextTicket &ticket);
    bool setInternal(const ContextTicket &ticket, bool internal);
    bool isInternal(const void *interfaceKey, const DeviceIdentity &identity) const;
    bool isCurrent(const ContextTicket &ticket) const;
    bool finish(const ContextTicket &ticket, CycleState finalState);
    void reset();

private:
    struct Slot {
        const void *interfaceKey;
        DeviceIdentity identity;
        ProbePacket lastSet;
        CycleState state;
        bool internalRequest;
        bool occupied;
        uint64_t sequence;
    };

    Slot slots_[kCapacity];

    static bool identityEqual(const DeviceIdentity &a, const DeviceIdentity &b);
    int findExact(const void *interfaceKey, const DeviceIdentity &identity) const;
    int findByKey(const void *interfaceKey) const;
    int chooseReusableSlot() const;
    void resetSlot(size_t index, bool preserveSequence);
    bool fillTicket(size_t index, ContextTicket &ticket) const;
};

} // namespace UVC
