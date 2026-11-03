#pragma once

#include "UVCRecoveryContext.hpp"

namespace UVC {

// Reserved interface for future, narrowly-proven device exceptions.
// UVCProbeFix 0.3.1 intentionally ships with no recovery quirks.
bool hasRecoveryQuirk(const DeviceIdentity &identity);

} // namespace UVC
