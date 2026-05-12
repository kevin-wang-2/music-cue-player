#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

// Force the application (and all its windows) to use the dark Aqua appearance,
// regardless of the system-level light/dark mode setting.
// This makes the macOS title bar dark to match our Fusion dark theme.
void applyMacOSDarkAppearance() {
    if (@available(macOS 10.14, *)) {
        [NSApp setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
    }
}

// Call this at the very start of any subprocess mode (--au-test-plugin,
// --scan-vst3, …).  Uses TransformProcessType instead of creating NSApp,
// so the Dock never sees a new-app-launched notification — no bounce.
void mcpSetSubprocessMode() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    ProcessSerialNumber psn = { 0, kCurrentProcess };
    TransformProcessType(&psn, kProcessTransformToBackgroundApplication);
#pragma clang diagnostic pop
}
