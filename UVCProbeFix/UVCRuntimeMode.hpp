#pragma once

#include <stdint.h>

namespace UVC {

enum class RuntimeMode : uint8_t {
    Off,
    Legacy0215,
    GenericObserve,
    GenericFieldRepairDev,
    GenericFull
};

struct BootArgState {
    bool off;
    bool legacy;
    bool observe;
    bool field;
    bool recover;
};

RuntimeMode selectRuntimeMode(const BootArgState &args,
                              bool debugBuild,
                              bool genericDefaultEnabled);

} // namespace UVC
