#include "engine/FadeData.h"

namespace mcp {

void FadeData::computeRamp(int steps) {
    rampReady.store(false, std::memory_order_relaxed);
    ramp.resize(std::max(1, steps));

    // Normalized progress ramp: ramp[i] = i/(steps-1) ∈ [0..1].
    // Curve shaping (linear vs equal-power) is applied at step-execution time
    // in CueList::fire() using lut::cos_hp / lut::sin_hp (O(1) LUT lookups).
    if (steps == 1) {
        ramp[0] = 1.0f;
    } else {
        for (int i = 0; i < steps; ++i)
            ramp[i] = static_cast<float>(i) / static_cast<float>(steps - 1);
    }

    rampReady.store(true, std::memory_order_release);
}

FadeData::~FadeData() {
    if (computeThread.joinable()) computeThread.join();
}

} // namespace mcp
