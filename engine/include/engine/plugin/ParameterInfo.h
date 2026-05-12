#pragma once
#include "engine/AutoParam.h"
#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcp::plugin {

// Metadata and conversion functions for one parameter.
// id must be stable across sessions — do NOT use the display name as id.
// toNormalized / fromNormalized store simple lambdas; for typical linear or
// dB parameters the captured state fits in SSO and causes no heap allocation.
struct ParameterInfo {
    std::string id;
    std::string name;
    std::string unit;
    float  defaultValue{0.0f};
    float  minValue    {0.0f};
    float  maxValue    {1.0f};
    bool   automatable {true};
    bool   discrete    {false};
    int    steps       {0};     // meaningful when discrete == true
    mcp::AutoParam::Domain domain{mcp::AutoParam::Domain::Linear};

    std::function<float(float value)>       toNormalized;   // value  → [0,1]
    std::function<float(float normalized)>  fromNormalized; // [0,1]  → value
    std::function<std::string(float value)> display;        // optional
};

// Factory for a linearly-mapped parameter.
// normalized = (value - min) / (max - min), clamped to [0,1].
inline ParameterInfo makeLinearParam(
    std::string id,
    std::string name,
    std::string unit,
    float minValue,
    float maxValue,
    float defaultValue,
    bool  automatable = true)
{
    ParameterInfo p;
    p.id           = std::move(id);
    p.name         = std::move(name);
    p.unit         = std::move(unit);
    p.minValue     = minValue;
    p.maxValue     = maxValue;
    p.defaultValue = defaultValue;
    p.automatable  = automatable;

    const float range = maxValue - minValue;
    p.toNormalized   = [minValue, range](float v) -> float {
        if (range <= 0.0f) return 0.0f;
        return std::clamp((v - minValue) / range, 0.0f, 1.0f);
    };
    p.fromNormalized = [minValue, range](float n) -> float {
        return minValue + std::clamp(n, 0.0f, 1.0f) * range;
    };
    return p;
}

// ── ParameterSet ─────────────────────────────────────────────────────────────
// Manages a fixed set of parameters.  Fast O(1) index-based access is
// available for the realtime process() path (getValueAt / setValueAt /
// applyEvent).  ID-based access (getValue / setValue) involves an
// unordered_map lookup and is intended for non-realtime use (UI, state load).
class ParameterSet {
public:
    explicit ParameterSet(std::vector<ParameterInfo> infos);

    // Metadata
    const std::vector<ParameterInfo>& infos()    const { return m_infos; }
    const ParameterInfo* findInfo(const std::string& id) const;

    // Returns the index for id, or -1 if not found.
    // Cache this in prepare() to avoid map lookup in process().
    int indexFor(const std::string& id) const;

    // ID-based access (non-RT-safe due to hash lookup; use for UI / state).
    float getValue     (const std::string& id) const;
    void  setValue     (const std::string& id, float value);      // clamps
    float getNormalized(const std::string& id) const;
    void  setNormalized(const std::string& id, float normalized);

    // Index-based access — RT-safe (no allocation, no map lookup).
    float getValueAt(int index) const;
    void  setValueAt(int index, float value);  // clamps to [min, max]

    // Apply one event by ID — suitable for event dispatch in process().
    // Internally uses the hash map; for maximum RT predictability, processors
    // can instead cache indices and call setValueAt directly.
    void applyEvent(const std::string& parameterId, float value);

    int  size() const { return static_cast<int>(m_values.size()); }

private:
    std::vector<ParameterInfo>        m_infos;
    std::vector<float>                m_values;
    std::unordered_map<std::string, int> m_idToIndex;
};

} // namespace mcp::plugin
