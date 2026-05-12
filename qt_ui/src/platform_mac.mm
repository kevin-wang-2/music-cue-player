#import <AppKit/AppKit.h>

// Force the application (and all its windows) to use the dark Aqua appearance,
// regardless of the system-level light/dark mode setting.
// This makes the macOS title bar dark to match our Fusion dark theme.
void applyMacOSDarkAppearance() {
    if (@available(macOS 10.14, *)) {
        [NSApp setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
    }
}

// Call this at the very start of subprocess (--au-test-plugin) mode.
// NSApplicationActivationPolicyProhibited prevents any window the AU plugin
// tries to open (auth dialogs, splash screens) from being brought to the
// foreground or appearing in the Dock.
void mcpSetSubprocessMode() {
    [[NSApplication sharedApplication]
        setActivationPolicy:NSApplicationActivationPolicyProhibited];
}
