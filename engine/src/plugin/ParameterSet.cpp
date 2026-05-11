#include "engine/plugin/ParameterInfo.h"
#include <algorithm>
#include <stdexcept>

namespace mcp::plugin {

ParameterSet::ParameterSet(std::vector<ParameterInfo> infos)
    : m_infos(std::move(infos))
{
    m_values.reserve(m_infos.size());
    for (int i = 0; i < static_cast<int>(m_infos.size()); ++i) {
        m_values.push_back(m_infos[static_cast<size_t>(i)].defaultValue);
        m_idToIndex[m_infos[static_cast<size_t>(i)].id] = i;
    }
}

const ParameterInfo* ParameterSet::findInfo(const std::string& id) const {
    auto it = m_idToIndex.find(id);
    if (it == m_idToIndex.end()) return nullptr;
    return &m_infos[static_cast<size_t>(it->second)];
}

int ParameterSet::indexFor(const std::string& id) const {
    auto it = m_idToIndex.find(id);
    return (it == m_idToIndex.end()) ? -1 : it->second;
}

float ParameterSet::getValueAt(int idx) const {
    return m_values[static_cast<size_t>(idx)];
}

void ParameterSet::setValueAt(int idx, float value) {
    const auto& info = m_infos[static_cast<size_t>(idx)];
    m_values[static_cast<size_t>(idx)] = std::clamp(value, info.minValue, info.maxValue);
}

void ParameterSet::applyEvent(const std::string& parameterId, float value) {
    auto it = m_idToIndex.find(parameterId);
    if (it != m_idToIndex.end())
        setValueAt(it->second, value);
}

float ParameterSet::getValue(const std::string& id) const {
    const int idx = indexFor(id);
    return (idx >= 0) ? getValueAt(idx) : 0.0f;
}

void ParameterSet::setValue(const std::string& id, float value) {
    const int idx = indexFor(id);
    if (idx >= 0) setValueAt(idx, value);
}

float ParameterSet::getNormalized(const std::string& id) const {
    const ParameterInfo* info = findInfo(id);
    if (!info || !info->toNormalized) return 0.0f;
    return info->toNormalized(getValue(id));
}

void ParameterSet::setNormalized(const std::string& id, float normalized) {
    const ParameterInfo* info = findInfo(id);
    if (!info || !info->fromNormalized) return;
    setValue(id, info->fromNormalized(normalized));
}

} // namespace mcp::plugin
