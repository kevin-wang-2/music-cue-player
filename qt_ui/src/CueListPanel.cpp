#include "CueListPanel.h"
#include "AppModel.h"
#include "ShowHelpers.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

static const char* kPanelStyle =
    "QWidget#CueListPanel{background:#161616;}"
    "QListWidget{background:#161616;color:#ccc;border:none;outline:none;"
    "  font-size:12px;}"
    "QListWidget::item{padding:5px 6px;border-bottom:1px solid #222;}"
    "QListWidget::item:selected{background:#1e3a5c;color:#fff;}"
    "QPushButton{background:#222;color:#aaa;border:1px solid #333;"
    "  border-radius:3px;padding:3px 8px;font-size:11px;}"
    "QPushButton:hover{background:#2a2a2a;}"
    "QLabel#header{color:#777;font-size:10px;padding:4px 6px;"
    "  border-bottom:1px solid #222;letter-spacing:1px;}";

// status: 0=idle 1=playing 2=armed 3=broken
static QPixmap makeStatusDot(int status) {
    QPixmap px(10, 10);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    QColor col;
    switch (status) {
        case 1: col = QColor(0x44, 0xcc, 0x44); break;  // green  — playing
        case 2: col = QColor(0xdd, 0xcc, 0x22); break;  // yellow — armed
        case 3: col = QColor(0xff, 0x44, 0x44); break;  // red    — broken
        default: col = QColor(0x44, 0x44, 0x44); break; // grey   — idle
    }
    p.setBrush(col);
    p.setPen(Qt::NoPen);
    p.drawEllipse(1, 1, 8, 8);
    return px;
}

CueListPanel::CueListPanel(AppModel* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    setObjectName("CueListPanel");
    setStyleSheet(kPanelStyle);
    setFixedWidth(160);

    auto* vlay = new QVBoxLayout(this);
    vlay->setContentsMargins(0, 0, 0, 0);
    vlay->setSpacing(0);

    auto* header = new QLabel("CUE LISTS");
    header->setObjectName("header");
    vlay->addWidget(header);

    m_list = new QListWidget;
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setDragDropMode(QAbstractItemView::NoDragDrop);
    vlay->addWidget(m_list, 1);

    auto* btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(4, 4, 4, 4);
    btnRow->setSpacing(4);
    m_btnAdd = new QPushButton("+ New");
    m_btnDel = new QPushButton("− Del");
    btnRow->addWidget(m_btnAdd);
    btnRow->addWidget(m_btnDel);
    vlay->addLayout(btnRow);

    rebuild();

    connect(m_list, &QListWidget::currentRowChanged, this, &CueListPanel::onListClicked);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        onListDoubleClicked(m_list->currentRow());
    });
    connect(m_btnAdd, &QPushButton::clicked, this, &CueListPanel::onAddList);
    connect(m_btnDel, &QPushButton::clicked, this, &CueListPanel::onDeleteList);

    connect(model, &AppModel::cueListsChanged,  this, &CueListPanel::onModelListsChanged);
    connect(model, &AppModel::activeListChanged, this, [this](int idx) {
        m_list->blockSignals(true);
        m_list->setCurrentRow(idx);
        m_list->blockSignals(false);
    });
}

void CueListPanel::rebuild() {
    m_list->blockSignals(true);
    m_list->clear();
    for (int li = 0; li < static_cast<int>(m_model->sf.cueLists.size()); ++li) {
        const auto& cld = m_model->sf.cueLists[static_cast<size_t>(li)];
        auto* item = new QListWidgetItem(QString::fromStdString(cld.name));
        item->setIcon(QIcon(makeStatusDot(0)));
        m_list->addItem(item);
    }
    const int active = m_model->activeListIdx();
    if (active >= 0 && active < m_list->count())
        m_list->setCurrentRow(active);
    m_list->blockSignals(false);
}

void CueListPanel::refresh() {
    const int n = m_list->count();
    if (n != static_cast<int>(m_model->sf.cueLists.size())) {
        rebuild();
        return;
    }
    for (int li = 0; li < n && li < m_model->listCount(); ++li) {
        auto& engList = m_model->cueListAt(li);
        const int cnt = engList.cueCount();

        bool hasPlaying = false, hasArmed = false, hasBroken = false;
        for (int i = 0; i < cnt; ++i) {
            if (engList.isCuePlaying(i) || engList.isCuePending(i)) { hasPlaying = true; break; }
        }
        if (!hasPlaying) {
            for (int i = 0; i < cnt; ++i) {
                if (engList.isArmed(i)) { hasArmed = true; break; }
            }
        }
        if (!hasPlaying && !hasArmed) {
            for (int i = 0; i < cnt; ++i) {
                if (engList.isSyncGroupBroken(i)) { hasBroken = true; break; }
            }
        }

        int status = 0;
        if (hasPlaying)      status = 1;
        else if (hasArmed)   status = 2;
        else if (hasBroken)  status = 3;

        if (auto* item = m_list->item(li))
            item->setIcon(QIcon(makeStatusDot(status)));
    }
}

void CueListPanel::onModelListsChanged() {
    rebuild();
}

void CueListPanel::onListClicked(int row) {
    if (row < 0 || row >= m_model->listCount()) return;
    m_model->setActiveList(row);
}

void CueListPanel::onListDoubleClicked(int row) {
    if (row < 0 || row >= static_cast<int>(m_model->sf.cueLists.size())) return;
    auto& cld = m_model->sf.cueLists[static_cast<size_t>(row)];
    bool ok = false;
    const QString cur = QString::fromStdString(cld.name);
    const QString name = QInputDialog::getText(this, "Rename List", "Name:", QLineEdit::Normal, cur, &ok);
    if (!ok || name.trimmed().isEmpty() || name == cur) return;
    m_model->pushUndo();
    cld.name = name.trimmed().toStdString();
    if (auto* item = m_list->item(row))
        item->setText(name.trimmed());
    m_model->dirty = true;
    emit m_model->dirtyChanged(true);
    emit m_model->cueListsChanged();
}

void CueListPanel::onAddList() {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, "New Cue List", "List name:", QLineEdit::Normal,
        QString("List %1").arg(m_model->sf.cueLists.size() + 1), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    m_model->pushUndo();
    mcp::ShowFile::CueListData cld;
    cld.name      = name.trimmed().toStdString();
    cld.numericId = m_model->sf.nextListId();
    m_model->sf.cueLists.push_back(std::move(cld));

    std::string err;
    ShowHelpers::rebuildAllCueLists(*m_model, err);

    m_model->dirty = true;
    emit m_model->dirtyChanged(true);
    emit m_model->cueListsChanged();

    // Switch to the new list
    m_model->setActiveList(static_cast<int>(m_model->sf.cueLists.size()) - 1);
}

void CueListPanel::onDeleteList() {
    const int row = m_list->currentRow();
    if (row < 0 || row >= static_cast<int>(m_model->sf.cueLists.size())) return;
    if (m_model->sf.cueLists.size() <= 1) {
        QMessageBox::warning(this, "Cannot Delete", "At least one cue list must remain.");
        return;
    }
    const QString name = QString::fromStdString(m_model->sf.cueLists[static_cast<size_t>(row)].name);
    if (QMessageBox::question(this, "Delete List",
            QString("Delete \"%1\" and all its cues?").arg(name))
        != QMessageBox::Yes) return;

    m_model->pushUndo();
    // Panic any playing cues in the deleted list
    m_model->cueListAt(row).panic();
    m_model->sf.cueLists.erase(m_model->sf.cueLists.begin() + row);

    std::string err;
    ShowHelpers::rebuildAllCueLists(*m_model, err);

    m_model->dirty = true;
    emit m_model->dirtyChanged(true);
    emit m_model->cueListsChanged();

    // Ensure activeListIdx stays valid
    const int newActive = std::min(row, static_cast<int>(m_model->sf.cueLists.size()) - 1);
    m_model->setActiveList(newActive);
}
