#pragma once

#include "engine/TriggerData.h"

#include <QObject>
#include <QUdpSocket>
#include <QString>
#include <functional>
#include <string>
#include <vector>

// Lightweight OSC-over-UDP server.
// Listens on a configurable port, parses incoming messages, and dispatches
// them to registered handlers.
//
// Parsed message:  path (string) + args (list of string/int/float)
//
// Access control:
//   - If OscServerSettings::accessList is empty → reject everything.
//   - If an entry has empty password → accept all (open access).
//   - Otherwise the first string arg must match a password entry.
//   - If password required, the first string arg is consumed and NOT forwarded.
class OscServer : public QObject {
    Q_OBJECT
public:
    explicit OscServer(QObject* parent = nullptr);
    ~OscServer() override;

    // Apply settings and (re)start/stop the server.
    void applySettings(const mcp::OscServerSettings& s);
    void stop();

    bool isRunning() const { return m_socket && m_socket->state() == QAbstractSocket::BoundState; }
    int  listenPort() const { return m_port; }

signals:
    // Emitted on the main thread for each valid, accepted OSC message.
    // args contains string, int, or float values as QVariant.
    void messageReceived(const QString& path, const QVariantList& args);

private slots:
    void onReadyRead();

private:
    struct Arg { enum Tag { Int, Float, Str } tag; int i{0}; float f{0}; std::string s; };
    bool parseOsc(const QByteArray& data, std::string& pathOut, std::vector<Arg>& argsOut);

    QUdpSocket* m_socket{nullptr};
    mcp::OscServerSettings m_settings;
    int m_port{0};
};
