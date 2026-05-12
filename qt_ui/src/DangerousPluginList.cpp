#include "DangerousPluginList.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

static const char* const kDefaultVendors[] = {
    "Waves Audio",
    "Waves",
};

DangerousPluginList::DangerousPluginList() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_path = (dir + "/dangerous_au_vendors.json").toStdString();

    for (const auto* v : kDefaultVendors)
        m_vendors.insert(v);

    load();  // overlay with any additional entries from disk
}

bool DangerousPluginList::contains(const std::string& vendorName) const {
    return m_vendors.count(vendorName) > 0;
}

void DangerousPluginList::add(const std::string& vendorName) {
    if (vendorName.empty()) return;
    if (m_vendors.count(vendorName)) return;  // already present — avoid spurious writes
    m_vendors.insert(vendorName);
    save();
}

void DangerousPluginList::save() const {
    const QString path = QString::fromStdString(m_path);
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonArray arr;
    for (const auto& v : m_vendors)
        arr.append(QString::fromStdString(v));

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(QJsonDocument(arr).toJson());
}

void DangerousPluginList::load() {
    const QString path = QString::fromStdString(m_path);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;

    for (const QJsonValue& v : doc.array()) {
        const QString s = v.toString();
        if (!s.isEmpty())
            m_vendors.insert(s.toStdString());
    }
}
