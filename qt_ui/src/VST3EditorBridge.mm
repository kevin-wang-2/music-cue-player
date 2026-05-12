#if defined(MCP_HAVE_VST3) && defined(__APPLE__)
#import <AppKit/AppKit.h>
#include <QPointer>
#include <QWidget>
#include "VST3EditorBridge.h"
#include "engine/plugin/VST3PluginAdapter.h"

// Owns the host NSView lifetime: clears the resize callback, then calls
// destroyEditorView() before Qt tears down the backing QNSView.
class VST3ContainerWidget : public QWidget {
public:
    VST3ContainerWidget(mcp::plugin::VST3PluginAdapter* adapter, QWidget* parent)
        : QWidget(parent), m_adapter(adapter) {}

    ~VST3ContainerWidget() override {
        if (m_adapter) {
            m_adapter->setResizeCallback(nullptr);
            m_adapter->destroyEditorView();
        }
    }

private:
    mcp::plugin::VST3PluginAdapter* m_adapter{nullptr};
};

// ── Phase 1 ──────────────────────────────────────────────────────────────────

QWidget* vst3CreateContainer(mcp::plugin::VST3PluginAdapter* adapter,
                              QWidget* parent)
{
    if (!adapter) return nullptr;

    auto* container = new VST3ContainerWidget(adapter, parent);

    // Install resize callback before attached() so the plugin can call
    // resizeView() during initialisation (e.g. FabFilter Pro-Q 3).
    // Also propagates the resize up to the host dialog.
    QPointer<VST3ContainerWidget> guard(container);
    adapter->setResizeCallback([guard](int w, int h) {
        if (!guard) return;
        guard->setFixedSize(w, h);
        // Resize the host dialog so it fits the new plugin view dimensions.
        if (QWidget* par = guard->parentWidget())
            par->adjustSize();
    });

    // Give the container a non-zero placeholder size before attached() so
    // plugins that query the parent NSView bounds during initialisation
    // (e.g. for HiDPI/backing-scale lookups) don't see a zero-size rect.
    container->setFixedSize(640, 400);

    // Create the native NSView now so winId() is stable across the two phases.
    container->setAttribute(Qt::WA_NativeWindow, true);
    container->winId();

    return container;
}

// ── Phase 2 ──────────────────────────────────────────────────────────────────

bool vst3AttachEditor(QWidget* container,
                      mcp::plugin::VST3PluginAdapter* adapter,
                      int& outW, int& outH)
{
    outW = 0; outH = 0;
    const WId wid = container->winId();
    void* hostNSView = reinterpret_cast<void*>(wid);

    // Verify the container's NSView is in a live NSWindow before calling
    // attached() — strict plugins query [nsview window] during initialisation.
    NSView* nsv = (__bridge NSView*)hostNSView;
    if (![nsv window]) {
        // Qt hasn't committed the addSubview: yet — run one more Cocoa pass.
        fprintf(stderr, "[MCP VST3] WARNING: container NSView not in NSWindow before attach "
                        "— flushing Cocoa layout\n");
        [nsv.superview layoutSubtreeIfNeeded];
        // If still not in a window, the attach will likely fail — log and proceed anyway.
        if (![nsv window])
            fprintf(stderr, "[MCP VST3] WARNING: still no NSWindow — attached() may fail\n");
    }

    if (!adapter->createEditorView(hostNSView, outW, outH))
        return false;

    // If the plugin didn't report a preferred size, inspect the host NSView
    // bounds — some plugins resize it directly during attached().
    if (outW <= 0 || outH <= 0) {
        const NSRect b = [nsv bounds];
        if (outW <= 0) outW = static_cast<int>(NSWidth(b));
        if (outH <= 0) outH = static_cast<int>(NSHeight(b));
    }
    if (outW <= 0) outW = 640;
    if (outH <= 0) outH = 400;

    return true;
}

#endif // MCP_HAVE_VST3 && __APPLE__
