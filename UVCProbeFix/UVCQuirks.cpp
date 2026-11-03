#include "UVCQuirks.hpp"

namespace UVC {

bool hasRecoveryQuirk(const DeviceIdentity &identity) {
    (void)identity;
    return false;
}

} // namespace UVC
