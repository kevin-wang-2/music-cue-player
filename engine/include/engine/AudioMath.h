#pragma once

// Lookup-table accelerated audio math.
//
// All tables are 8 192 entries + one guard entry (so linear interpolation
// never reads past the end).  Entries are initialised once on first call;
// the C++11 static-local guarantee makes that thread-safe.
//
// Accuracy:
//   lut_dBToLinear  — ≤ 0.003 % relative error over [-144, +40] dB
//   lut_cos/sin     — ≤ 3e-8 absolute error over [0, 1]
//   lut_linearToDB  — direct log10 (range is too large for a cheap table)

#include <array>
#include <cmath>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

namespace mcp {
namespace lut {

namespace detail {

constexpr int kN = 8192;

// cos(t * π/2) for t ∈ [0, 1], kN+1 entries so index kN is valid.
inline const std::array<float, kN + 1>& cosTable() {
    static const auto t = []() {
        std::array<float, kN + 1> a;
        for (int i = 0; i <= kN; ++i)
            a[i] = static_cast<float>(
                std::cos(static_cast<double>(i) / kN * (M_PI / 2.0)));
        return a;
    }();
    return t;
}

// dBToLinear over [-144, +40] dB
inline const std::array<float, kN + 1>& dBLinTable() {
    static const auto t = []() {
        constexpr double kLo = -144.0, kHi = 40.0;
        std::array<float, kN + 1> a;
        for (int i = 0; i <= kN; ++i) {
            const double dB = kLo + static_cast<double>(i) / kN * (kHi - kLo);
            a[i] = static_cast<float>(std::pow(10.0, dB / 20.0));
        }
        return a;
    }();
    return t;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API

// cos(t * π/2) for t ∈ [0, 1].  Clamps outside that range.
inline float cos_hp(float t) {
    const auto& tbl = detail::cosTable();
    const float idx  = t * detail::kN;
    const int   i0   = static_cast<int>(idx);
    if (i0 <= 0)             return tbl[0];
    if (i0 >= detail::kN)   return tbl[detail::kN];
    const float frac = idx - static_cast<float>(i0);
    return tbl[i0] + frac * (tbl[i0 + 1] - tbl[i0]);
}

// sin(t * π/2) for t ∈ [0, 1] — derived from the cosine table.
inline float sin_hp(float t) { return cos_hp(1.0f - t); }

// dB → linear amplitude.  Returns 0 for dB ≤ −144; direct pow for dB > 40.
inline float dBToLinear(double dB) {
    constexpr double kLo = -144.0, kHi = 40.0;
    if (dB <= kLo) return 0.0f;
    if (dB >  kHi) return static_cast<float>(std::pow(10.0, dB / 20.0));
    const auto&  tbl = detail::dBLinTable();
    const double idx  = (dB - kLo) / (kHi - kLo) * detail::kN;
    const int    i0   = static_cast<int>(idx);
    const float  frac = static_cast<float>(idx - i0);
    return tbl[i0] + frac * (tbl[i0 + 1] - tbl[i0]);
}

// Linear amplitude → dB.  Returns −144 for amp ≤ 0.
// Not LUT'd: the input range (0 … +∞) is too wide for a fixed table,
// and this is only called in the ramp pre-computation thread, not in any
// real-time path.
inline double linearToDB(double amp) {
    return (amp <= 0.0) ? -144.0 : 20.0 * std::log10(amp);
}

} // namespace lut
} // namespace mcp
