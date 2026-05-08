#pragma once
#include "engine/ShowFile.h"
#include <string>

namespace mcp {

// Send a network message to the given patch.
// For OSC patches: command is parsed as "/address arg1 arg2 ..."
//   - integer tokens → int32, decimal tokens → float32, others → string
//   - Explicit type prefixes: "i:123", "f:3.14", "s:hello"
// For plain-text patches: command bytes are sent verbatim (+ newline).
// TCP sends a 4-byte big-endian size prefix before the payload.
// Returns true on success; false on failure (error is set).
bool sendNetworkMessage(const ShowFile::NetworkSetup::Patch& patch,
                        const std::string& command,
                        std::string& error);

} // namespace mcp
