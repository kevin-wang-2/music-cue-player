#include "AppModel.h"
#include "MainWindow.h"
#include "ShowHelpers.h"

#include "engine/ShowFile.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef Q_OS_MAC
#  include "engine/plugin/AUCompatibilityTester.h"
#  include "engine/plugin/AUComponentEnumerator.h"
#endif

#ifdef Q_OS_MAC
void applyMacOSDarkAppearance();  // defined in platform_mac.mm
void mcpSetSubprocessMode();      // defined in platform_mac.mm
#endif

static void applyDarkPalette(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window,          QColor(0x18, 0x18, 0x18));
    p.setColor(QPalette::WindowText,      QColor(0xdd, 0xdd, 0xdd));
    p.setColor(QPalette::Base,            QColor(0x0e, 0x0e, 0x0e));
    p.setColor(QPalette::AlternateBase,   QColor(0x1c, 0x1c, 0x1c));
    p.setColor(QPalette::ToolTipBase,     QColor(0x28, 0x28, 0x28));
    p.setColor(QPalette::ToolTipText,     QColor(0xdd, 0xdd, 0xdd));
    p.setColor(QPalette::Text,            QColor(0xdd, 0xdd, 0xdd));
    p.setColor(QPalette::Button,          QColor(0x2c, 0x2c, 0x2c));
    p.setColor(QPalette::ButtonText,      QColor(0xdd, 0xdd, 0xdd));
    p.setColor(QPalette::BrightText,      Qt::red);
    p.setColor(QPalette::Highlight,       QColor(0x1e, 0x6a, 0xc8));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Light,           QColor(0x40, 0x40, 0x40));
    p.setColor(QPalette::Midlight,        QColor(0x30, 0x30, 0x30));
    p.setColor(QPalette::Mid,             QColor(0x28, 0x28, 0x28));
    p.setColor(QPalette::Dark,            QColor(0x18, 0x18, 0x18));
    p.setColor(QPalette::Shadow,          QColor(0x00, 0x00, 0x00));
    p.setColor(QPalette::Disabled, QPalette::Text,       QColor(0x55, 0x55, 0x55));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x55, 0x55, 0x55));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x55, 0x55, 0x55));
    app.setPalette(p);
}

int main(int argc, char* argv[]) {
#ifdef Q_OS_MAC
    // ── Subprocess mode: isolated AU plugin tester ────────────────────────────
    // Invoked as:  binary --au-test-plugin TTTTTTTT,SSSSSSSS,MMMMMMMM
    // where each token is 8 uppercase hex digits encoding a FCC code.
    // Runs the test in-process, prints one-entry JSON to stdout, exits.
    // Never creates a QApplication or touches any audio engine.
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--au-test-plugin") == 0) {
            // Suppress Dock icon and prevent plugin windows from stealing focus.
            mcpSetSubprocessMode();
            const char* arg = argv[i + 1];
            // Parse "TTTTTTTT,SSSSSSSS,MMMMMMMM"
            if (strlen(arg) == 26 && arg[8] == ',' && arg[17] == ',') {
                mcp::plugin::AUComponentEntry entry;
                entry.type         = static_cast<uint32_t>(strtoul(arg,      nullptr, 16));
                entry.subtype      = static_cast<uint32_t>(strtoul(arg +  9, nullptr, 16));
                entry.manufacturer = static_cast<uint32_t>(strtoul(arg + 18, nullptr, 16));
                // Look up display name/vendor/version from the system registry.
                for (const auto& e : mcp::plugin::AUComponentEnumerator::enumerate()) {
                    if (e.type == entry.type && e.subtype == entry.subtype &&
                            e.manufacturer == entry.manufacturer) {
                        entry.name             = e.name;
                        entry.manufacturerName = e.manufacturerName;
                        entry.version          = e.version;
                        break;
                    }
                }
                const auto result = mcp::plugin::AUCompatibilityTester::test(entry);
                const std::string json =
                    mcp::plugin::AUCompatibilityTester::toJson({result});
                fwrite(json.c_str(), 1, json.size(), stdout);
                fflush(stdout);
                return 0;
            }
            fprintf(stderr, "usage: --au-test-plugin TTTTTTTT,SSSSSSSS,MMMMMMMM\n");
            return 2;
        }
    }
#endif

    QApplication app(argc, argv);
    app.setApplicationName("Music Cue Player");
    app.setOrganizationName("click-in");

    applyDarkPalette(app);
#ifdef Q_OS_MAC
    applyMacOSDarkAppearance();
#endif

    AppModel model;
    model.engineOk = model.engine.initialize();
    if (!model.engineOk)
        model.engineError = "Could not open default audio output.";

    if (argc >= 2) {
        const std::string path = argv[1];
        std::string err;
        if (model.sf.load(path, err)) {
            model.showPath = path;
            model.baseDir  = std::filesystem::path(path).parent_path().string();
            ShowHelpers::rebuildAllCueLists(model, err);
        }
    } else {
        model.sf = mcp::ShowFile::empty();
        const int n = model.engineOk ? model.engine.channels() : 2;
        for (int i = 0; i < n; ++i) {
            mcp::ShowFile::AudioSetup::Channel ch;
            ch.name = "Ch " + std::to_string(i + 1);
            model.sf.audioSetup.channels.push_back(ch);
        }
        std::string err;
        ShowHelpers::rebuildAllCueLists(model, err);
    }

    // Initialize mixing state so channelOrder, fold matrix, and PDC are valid
    // before the UI is shown — required for DSP (polarity, plugins) to work.
    model.buildChannelPluginChains();
    model.applyMixing();
    model.rebuildSendTopology();

    MainWindow win(&model);
    win.showMaximized();

    const int ret = app.exec();
    model.engine.shutdown();
    return ret;
}
