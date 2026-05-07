#pragma once
#include "engine/MusicContext.h"
#include <string>

namespace MCImport {
    // Returns empty string on success, error message on failure.
    std::string fromMidi(const std::string& path, mcp::MusicContext& mc);
    std::string fromSmt(const std::string& path, mcp::MusicContext& mc);
}
