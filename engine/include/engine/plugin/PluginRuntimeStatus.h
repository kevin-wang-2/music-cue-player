#pragma once

namespace mcp::plugin {

enum class PluginRuntimeStatus {
    Ok,                // Plugin loaded and running normally
    Missing,           // Component / file not found on this system
    Failed,            // Found but instantiation or initialization failed
    UnsupportedLayout, // Channel layout not supported by this plugin
    StateRestoreFailed,// Plugin loaded OK but saved state could not be applied
    Disabled,          // Explicitly disabled; slot present but bypassed
    Deactivated        // Instance destroyed by Deactivate cue; will resume on Reactivate
};

inline const char* toString(PluginRuntimeStatus s) {
    switch (s) {
    case PluginRuntimeStatus::Ok:                 return "ok";
    case PluginRuntimeStatus::Missing:            return "missing";
    case PluginRuntimeStatus::Failed:             return "failed";
    case PluginRuntimeStatus::UnsupportedLayout:  return "unsupportedLayout";
    case PluginRuntimeStatus::StateRestoreFailed: return "stateRestoreFailed";
    case PluginRuntimeStatus::Disabled:           return "disabled";
    case PluginRuntimeStatus::Deactivated:        return "deactivated";
    }
    return "unknown";
}

} // namespace mcp::plugin
