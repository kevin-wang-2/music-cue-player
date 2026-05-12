#pragma once
#ifdef __APPLE__

class QWidget;

// Embeds an AU Cocoa NSView* (passed as void*) into a new QWidget child.
// preferredW / preferredH are hints; if zero the view's own bounds are used.
// Returns nullptr if nsView is null or embedding fails.
QWidget* auCreateEditorWidget(void* nsView, int preferredW, int preferredH,
                              QWidget* parent = nullptr);

#endif // __APPLE__
