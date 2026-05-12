#pragma once
#if defined(MCP_HAVE_VST3) && defined(__APPLE__)

namespace mcp::plugin { class VST3PluginAdapter; }
class QWidget;

// Two-phase VST3 native editor embedding.
//
// Phase 1 — call BEFORE showing the host dialog:
//   Creates a native-NSView-backed QWidget child and wires up the resize
//   callback.  Does NOT call IPlugView::attached() yet.
QWidget* vst3CreateContainer(mcp::plugin::VST3PluginAdapter* adapter,
                              QWidget* parent = nullptr);

// Phase 2 — call AFTER the host dialog has been shown and
// QApplication::processEvents() has run so the container's NSView is in a
// live NSWindow hierarchy (required by strict plugins like FabFilter):
//   Calls IPlugView::attached() and fills outW/outH with the preferred size.
//   Returns false if the plugin provides no editor or attachment fails;
//   the caller must then delete the container.
bool vst3AttachEditor(QWidget* container,
                      mcp::plugin::VST3PluginAdapter* adapter,
                      int& outW, int& outH);

#endif // MCP_HAVE_VST3 && __APPLE__
