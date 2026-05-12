#ifdef __APPLE__
#import <AppKit/AppKit.h>
#include <QWidget>
#include "AUEditorBridge.h"

// Container that removes the embedded AU NSView before Qt destroys the
// backing QNSView. Without this, CAAppleAUCustomViewBase::cleanup fires
// during -[NSView dealloc] after the AudioUnit may already be gone,
// causing a crash via objc_msgSend on a freed object (0x10).
class AUContainerWidget : public QWidget {
public:
    AUContainerWidget(NSView* auView, QWidget* parent)
        : QWidget(parent), m_auView(auView)
    {
        [m_auView retain];
    }

    ~AUContainerWidget() override {
        // Detach the AU view before Qt's QNSView dealloc chain runs.
        [m_auView removeFromSuperview];
        [m_auView release];
        m_auView = nil;
    }

private:
    NSView* m_auView{nil};
};

QWidget* auCreateEditorWidget(void* nsViewPtr, int preferredW, int preferredH,
                              QWidget* parent)
{
    NSView* auView = (__bridge NSView*)nsViewPtr;
    if (!auView) return nullptr;

    NSRect bounds = [auView bounds];
    int w = preferredW > 0 ? preferredW : static_cast<int>(NSWidth(bounds));
    int h = preferredH > 0 ? preferredH : static_cast<int>(NSHeight(bounds));
    if (w <= 0) w = 400;
    if (h <= 0) h = 300;

    auto* container = new AUContainerWidget(auView, parent);
    container->setFixedSize(w, h);
    // Force native (NSView-backed) window creation before getting winId().
    container->setAttribute(Qt::WA_NativeWindow, true);
    WId wid = container->winId();

    NSView* hostView = (__bridge NSView*)reinterpret_cast<void*>(wid);
    [auView setFrame:NSMakeRect(0, 0, w, h)];
    [hostView addSubview:auView];

    return container;
}
#endif // __APPLE__
