#include "NetworkSender.h"

#include <cstring>
#include <sstream>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
   using ssize_t  = int;
   static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#  define net_close closesocket
   // Auto-init Winsock on first use.
   namespace { const bool kWsaReady = []{ WSADATA w; return WSAStartup(MAKEWORD(2,2),&w)==0; }(); }
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
   static constexpr socket_t kInvalidSocket = -1;
#  define net_close close
#endif

namespace mcp {

// ── OSC packet builder ─────────────────────────────────────────────────────

static void appendPaddedStr(std::vector<uint8_t>& buf, const std::string& s) {
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
    const int sz      = static_cast<int>(s.size()) + 1;
    const int padding = ((sz + 3) & ~3) - sz;
    for (int i = 0; i < padding; ++i) buf.push_back(0);
}

static void appendInt32(std::vector<uint8_t>& buf, int32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >>  8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v      ) & 0xFF));
}

static void appendFloat32(std::vector<uint8_t>& buf, float v) {
    uint32_t raw;
    std::memcpy(&raw, &v, 4);
    appendInt32(buf, static_cast<int32_t>(raw));
}

// Build an OSC packet from a command string.
// First whitespace-delimited token = OSC address; remaining = arguments.
// Explicit prefix: "i:N" → int32, "f:N" → float32, "s:text" → string.
// Auto-detect: whole number → int32, decimal → float32, else → string.
static std::vector<uint8_t> buildOscPacket(const std::string& command) {
    // Parse only the first line (multi-line commands use first line for OSC)
    std::string line;
    {
        std::istringstream iss(command);
        std::getline(iss, line);
    }

    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string tok;
    while (iss >> tok) tokens.push_back(tok);

    if (tokens.empty() || tokens[0].empty() || tokens[0][0] != '/')
        return {};

    std::string typeTag = ",";
    std::vector<uint8_t> argBuf;

    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string& arg = tokens[i];

        // Explicit type prefix "X:..."
        if (arg.size() > 2 && arg[1] == ':') {
            const char pfx = arg[0];
            const std::string val = arg.substr(2);
            if (pfx == 'i') {
                typeTag += 'i';
                int32_t iv = 0;
                try { iv = static_cast<int32_t>(std::stol(val)); } catch (...) {}
                appendInt32(argBuf, iv);
                continue;
            }
            if (pfx == 'f') {
                typeTag += 'f';
                float fv = 0.0f;
                try { fv = std::stof(val); } catch (...) {}
                appendFloat32(argBuf, fv);
                continue;
            }
            if (pfx == 's') {
                typeTag += 's';
                appendPaddedStr(argBuf, val);
                continue;
            }
        }

        // Auto-detect: try integer first, then float, else string
        {
            size_t pos = 0;
            bool   ok  = false;
            long   iv  = 0;
            try { iv = std::stol(arg, &pos); ok = (pos == arg.size()); } catch (...) {}
            if (ok) {
                typeTag += 'i';
                appendInt32(argBuf, static_cast<int32_t>(iv));
                continue;
            }
        }
        {
            size_t pos  = 0;
            bool   ok   = false;
            double dv   = 0.0;
            try { dv = std::stod(arg, &pos); ok = (pos == arg.size()); } catch (...) {}
            if (ok) {
                typeTag += 'f';
                appendFloat32(argBuf, static_cast<float>(dv));
                continue;
            }
        }
        typeTag += 's';
        appendPaddedStr(argBuf, arg);
    }

    std::vector<uint8_t> packet;
    appendPaddedStr(packet, tokens[0]);   // address
    appendPaddedStr(packet, typeTag);     // type tag
    packet.insert(packet.end(), argBuf.begin(), argBuf.end());
    return packet;
}

// ── socket send ───────────────────────────────────────────────────────────

static bool parseDestination(const std::string& dest, std::string& host, int& port) {
    const auto colon = dest.rfind(':');
    if (colon == std::string::npos || colon + 1 >= dest.size()) return false;
    host = dest.substr(0, colon);
    try {
        size_t pos = 0;
        const int p = std::stoi(dest.substr(colon + 1), &pos);
        if (p > 0 && p < 65536) { port = p; return true; }
    } catch (...) {}
    return false;
}

bool sendNetworkMessage(const ShowFile::NetworkSetup::Patch& patch,
                        const std::string& command,
                        std::string& error)
{
    std::string host;
    int         port = 0;
    if (!parseDestination(patch.destination, host, port)) {
        error = "invalid destination: " + patch.destination;
        return false;
    }

    // Build payload
    std::vector<uint8_t> payload;
    if (patch.type == "osc") {
        payload = buildOscPacket(command);
        if (payload.empty()) { error = "invalid or empty OSC command"; return false; }
    } else {
        payload.insert(payload.end(), command.begin(), command.end());
        payload.push_back('\n');
    }

    const bool useTcp = (patch.protocol == "tcp");

    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = useTcp ? SOCK_STREAM : SOCK_DGRAM;

    struct addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        error = "cannot resolve host: " + host;
        return false;
    }

    const socket_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == kInvalidSocket) {
        freeaddrinfo(res);
        error = "socket creation failed";
        return false;
    }

    bool ok = false;
    if (connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0) {
        if (useTcp) {
            // 4-byte big-endian length prefix (QLab-compatible)
            const uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
            send(fd, reinterpret_cast<const char*>(&len), 4, 0);
        }
        const ssize_t sent = send(fd,
            reinterpret_cast<const char*>(payload.data()),
            static_cast<int>(payload.size()), 0);
        ok = (sent == static_cast<ssize_t>(payload.size()));
        if (!ok) error = "send failed";
    } else {
        error = "connect failed to " + patch.destination;
    }

    net_close(fd);
    freeaddrinfo(res);
    return ok;
}

} // namespace mcp
