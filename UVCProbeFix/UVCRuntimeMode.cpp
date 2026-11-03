#include "UVCRuntimeMode.hpp"

namespace UVC {

RuntimeMode selectRuntimeMode(const BootArgState &args,
                              bool debugBuild,
                              bool genericDefaultEnabled) {
    if (args.off) return RuntimeMode::Off;

    if (!debugBuild) {
        if (args.legacy) return RuntimeMode::Legacy0215;
        // Until GenericFull has passed the physical hardware gates, preserve
        // the exact 0.2.15 behavior that is already known to recover the
        // affected Tahoe camera instead of silently disabling the plugin.
        return genericDefaultEnabled ? RuntimeMode::GenericFull : RuntimeMode::Legacy0215;
    }

    const unsigned genericSelectors = static_cast<unsigned>(args.observe) +
                                      static_cast<unsigned>(args.field) +
                                      static_cast<unsigned>(args.recover);
    if ((args.legacy && genericSelectors != 0U) || genericSelectors > 1U) {
        return RuntimeMode::Off;
    }
    if (args.legacy) return RuntimeMode::Legacy0215;
    if (args.observe) return RuntimeMode::GenericObserve;
    if (args.field) return RuntimeMode::GenericFieldRepairDev;
    if (args.recover) return RuntimeMode::GenericFull;
    return RuntimeMode::GenericObserve;
}

} // namespace UVC
