#include "engine/FadeData.h"
#include "engine/AudioMath.h"

namespace mcp {

void FadeData::computeRamp(double startDB, int steps) {
    rampReady.store(false, std::memory_order_relaxed);
    ramp.resize(std::max(1, steps));

    if (steps == 1) {
        ramp[0] = targetValue;
        rampReady.store(true, std::memory_order_release);
        return;
    }

    const float startLin = lut::dBToLinear(startDB);
    const float endLin   = lut::dBToLinear(targetValue);

    if (curve == Curve::EqualPower) {
        // Equal-power cosine fade:
        //   amp(t) = startLin * cos(t·π/2) + endLin * sin(t·π/2)
        //
        // At any t, startLin²·cos²+endLin²·sin² is constant only for a
        // symmetric crossfade (startLin==endLin==1), but the cosine shape
        // gives the perceptually smooth "S-curve" transition that listeners
        // associate with equal-power fades.  The cos/sin values come from
        // the LUT so no std::cos/std::sin is called inside the loop.
        for (int i = 0; i < steps; ++i) {
            const float t   = static_cast<float>(i) / static_cast<float>(steps - 1);
            const float amp = startLin * lut::cos_hp(t) + endLin * lut::sin_hp(t);
            ramp[i] = lut::linearToDB(static_cast<double>(amp));
        }
    } else {
        // Linear amplitude fade: straight line in amplitude space.
        for (int i = 0; i < steps; ++i) {
            const float t   = static_cast<float>(i) / static_cast<float>(steps - 1);
            const float amp = startLin + t * (endLin - startLin);
            ramp[i] = lut::linearToDB(static_cast<double>(amp));
        }
    }

    rampReady.store(true, std::memory_order_release);
}

FadeData::~FadeData() {
    if (computeThread.joinable()) computeThread.join();
}

} // namespace mcp
