#include "OscServer.h"

#include <QHostAddress>
#include <QVariant>
#include <cstring>

OscServer::OscServer(QObject* parent) : QObject(parent) {}

OscServer::~OscServer() { stop(); }

void OscServer::applySettings(const mcp::OscServerSettings& s) {
    m_settings = s;
    stop();
    if (!s.enabled) return;

    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &OscServer::onReadyRead);
    if (!m_socket->bind(QHostAddress::AnyIPv4, static_cast<quint16>(s.listenPort))) {
        delete m_socket; m_socket = nullptr;
        return;
    }
    m_port = s.listenPort;
}

void OscServer::stop() {
    if (m_socket) {
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_port = 0;
}

// ---------------------------------------------------------------------------
void OscServer::onReadyRead() {
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        m_socket->readDatagram(data.data(), data.size());

        std::string path;
        std::vector<Arg> args;
        if (!parseOsc(data, path, args)) continue;

        // Access control
        if (!m_settings.accessList.empty()) {
            bool accepted = false;
            if (!m_settings.requiresPassword()) {
                accepted = true;
            } else {
                // First string arg must be the password
                if (!args.empty() && args[0].tag == Arg::Str) {
                    if (m_settings.acceptsPassword(args[0].s)) {
                        args.erase(args.begin());  // consume password arg
                        accepted = true;
                    }
                }
            }
            if (!accepted) continue;
        }

        // Build QVariantList for the signal
        QVariantList qargs;
        for (const auto& a : args) {
            switch (a.tag) {
                case Arg::Int:   qargs.append(a.i); break;
                case Arg::Float: qargs.append(static_cast<double>(a.f)); break;
                case Arg::Str:   qargs.append(QString::fromStdString(a.s)); break;
            }
        }
        emit messageReceived(QString::fromStdString(path), qargs);
    }
}

// ---------------------------------------------------------------------------
// Minimal OSC message parser (no bundles).
// OSC packet layout:
//   address   : null-terminated string, padded to 4-byte boundary
//   type tags : ',' + type chars, null-terminated, padded to 4-byte boundary
//   arguments : packed per type tag
bool OscServer::parseOsc(const QByteArray& data, std::string& pathOut,
                          std::vector<Arg>& argsOut) {
    const char* buf = data.constData();
    int         len = data.size();
    int         pos = 0;

    auto pad4 = [](int n) { return (n + 3) & ~3; };

    // Address
    int addrLen = static_cast<int>(strnlen(buf + pos, static_cast<size_t>(len - pos)));
    if (pos + addrLen >= len) return false;
    pathOut = std::string(buf + pos, static_cast<size_t>(addrLen));
    pos = pad4(pos + addrLen + 1);
    if (pos > len) return false;

    // Type tag string (starts with ',')
    if (pos >= len || buf[pos] != ',') return true;  // no type tags — valid, empty args
    int ttLen = static_cast<int>(strnlen(buf + pos, static_cast<size_t>(len - pos)));
    std::string typeTags(buf + pos + 1, static_cast<size_t>(ttLen - 1));  // strip ','
    pos = pad4(pos + ttLen + 1);

    // Arguments
    for (char t : typeTags) {
        if (pos > len) return false;
        Arg a;
        switch (t) {
            case 'i': case 'r': {
                if (pos + 4 > len) return false;
                uint32_t v; std::memcpy(&v, buf + pos, 4);
                // Network byte order → host
                a.tag = Arg::Int;
                a.i   = static_cast<int>(__builtin_bswap32(v));
                pos  += 4; break;
            }
            case 'f': {
                if (pos + 4 > len) return false;
                uint32_t v; std::memcpy(&v, buf + pos, 4);
                v = __builtin_bswap32(v);
                std::memcpy(&a.f, &v, 4);
                a.tag = Arg::Float;
                pos  += 4; break;
            }
            case 's': case 'S': {
                int sLen = static_cast<int>(strnlen(buf + pos, static_cast<size_t>(len - pos)));
                a.tag = Arg::Str;
                a.s   = std::string(buf + pos, static_cast<size_t>(sLen));
                pos   = pad4(pos + sLen + 1); break;
            }
            case 'T': a.tag = Arg::Int; a.i = 1; break;
            case 'F': a.tag = Arg::Int; a.i = 0; break;
            default: break;  // skip unknown types
        }
        argsOut.push_back(a);
    }
    return true;
}
