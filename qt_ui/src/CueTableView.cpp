#include "CueTableView.h"
#include "AppModel.h"
#include "ShowHelpers.h"

#include "engine/Cue.h"
#include "engine/ShowFile.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QUrl>
#include <filesystem>

// ── helpers ────────────────────────────────────────────────────────────────

static const QColor kColorPlaying {0x44, 0xcc, 0x55};
static const QColor kColorPending {0xff, 0xaa, 0x00};
static const QColor kColorArmed   {0xff, 0xee, 0x00};
static const QColor kColorIdle    {0x55, 0x55, 0x55};
static const QColor kColorBroken  {0xff, 0x44, 0x44};

// ── ctor ───────────────────────────────────────────────────────────────────

CueTableView::CueTableView(AppModel* model, QWidget* parent)
    : QTableWidget(parent), m_model(model)
{
    setColumnCount(ColCount);
    setHorizontalHeaderLabels({{"#"}, {"Type"}, {"Name"}, {"Target"}, {"Duration"}, {"Status"}});

    horizontalHeader()->setSectionResizeMode(ColNum,      QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(ColType,     QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(ColName,     QHeaderView::Stretch);
    horizontalHeader()->setSectionResizeMode(ColTarget,   QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(ColDuration, QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(ColStatus,   QHeaderView::Fixed);
    setColumnWidth(ColStatus, 90);

    verticalHeader()->setDefaultSectionSize(22);
    verticalHeader()->hide();

    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    setDragDropMode(QAbstractItemView::InternalMove);  // draws insertion line
    setAcceptDrops(true);   // also allow external file drops
    setDropIndicatorShown(true);

    setAlternatingRowColors(true);
    setShowGrid(false);
    setWordWrap(false);
    setStyleSheet(
        "QTableWidget { background:#0e0e0e; alternate-background-color:#181818;"
        "  color:#dddddd; border:none; outline:none; }"
        "QTableWidget::item:selected { background:#1e6ac8; color:#ffffff; }"
        "QHeaderView::section { background:#1a1a1a; color:#999; border:none;"
        "  border-bottom:1px solid #333; padding:3px 6px; }"
    );

    connect(this, &QTableWidget::cellChanged, this, &CueTableView::onCellChanged);
}

// ── public API ─────────────────────────────────────────────────────────────

void CueTableView::refresh() {
    // Remove stale collapsed entries (group cues that no longer exist or are empty).
    {
        std::set<int> validCollapsed;
        for (int gi : m_collapsed) {
            const auto* c = m_model->cues.cueAt(gi);
            if (c && c->type == mcp::CueType::Group && c->childCount > 0)
                validCollapsed.insert(gi);
        }
        m_collapsed = std::move(validCollapsed);
    }

    m_refreshing = true;
    const int n = m_model->cues.cueCount();
    setRowCount(n);
    for (int i = 0; i < n; ++i) populateRow(i);

    // Hide rows whose ancestor group is collapsed.
    for (int i = 0; i < n; ++i) {
        bool hidden = false;
        const mcp::Cue* c = m_model->cues.cueAt(i);
        int pi = c ? c->parentIndex : -1;
        while (pi >= 0) {
            if (m_collapsed.count(pi)) { hidden = true; break; }
            const mcp::Cue* par = m_model->cues.cueAt(pi);
            pi = par ? par->parentIndex : -1;
        }
        setRowHidden(i, hidden);
    }

    m_refreshing = false;

    // Restore selection
    if (m_selRow >= 0 && m_selRow < n) {
        selectRow(m_selRow);
    } else if (n > 0) {
        m_selRow = 0;
        selectRow(0);
        emit rowSelected(0);
    } else {
        m_selRow = -1;
        emit rowSelected(-1);
    }
}

void CueTableView::refreshStatus() {
    const int n = rowCount();
    for (int i = 0; i < n; ++i) setRowStatus(i);
}

void CueTableView::syncEngineSelection(int engineIdx) {
    if (engineIdx == m_selRow) return;
    m_selRow = engineIdx;
    if (engineIdx >= 0 && engineIdx < rowCount()) {
        selectRow(engineIdx);
    } else {
        clearSelection();
    }
    emit rowSelected(engineIdx);
}

// ── internal row population ────────────────────────────────────────────────

void CueTableView::populateRow(int row) {
    const mcp::Cue* c = m_model->cues.cueAt(row);
    if (!c) return;

    const bool isAudio = c->type == mcp::CueType::Audio;

    // No ItemIsDropEnabled on editable/RO items — rows reorder with an insertion
    // line indicator. Target cells of control cues get ItemIsDropEnabled separately.
    auto mkEdit = [](const QString& txt) {
        auto* it = new QTableWidgetItem(txt);
        it->setFlags((it->flags() | Qt::ItemIsEditable) & ~Qt::ItemIsDropEnabled);
        return it;
    };
    auto mkRO = [](const QString& txt) {
        auto* it = new QTableWidgetItem(txt);
        it->setFlags((it->flags() & ~Qt::ItemIsEditable) & ~Qt::ItemIsDropEnabled);
        return it;
    };

    // Depth-based indent prefix for the Name column; ▶/▼ toggle for groups.
    const int depth = cueDepth(row);
    const QString indent(depth * 4, QChar(' '));
    QString namePrefix = indent;
    if (c->type == mcp::CueType::Group && c->childCount > 0)
        namePrefix += m_collapsed.count(row) ? QStringLiteral("> ") : QStringLiteral("∨ ");
    else if (depth > 0)
        namePrefix += QStringLiteral("  ");   // align children to the same column as group text

    setItem(row, ColNum,      mkEdit(QString::fromStdString(c->cueNumber)));
    setItem(row, ColType,     mkRO  (typeLabel(c->type)));
    setItem(row, ColName,     mkEdit(namePrefix + QString::fromStdString(c->name)));

    // Target cell: control cues get ItemIsDropEnabled so dragging a cue onto it
    // sets the target. Show "—" when no target is assigned yet.
    const bool canTarget = (c->type == mcp::CueType::Fade  ||
                            c->type == mcp::CueType::Start ||
                            c->type == mcp::CueType::Stop  ||
                            c->type == mcp::CueType::Devamp||
                            c->type == mcp::CueType::Arm);
    {
        QString tl = targetLabel(row);
        if (canTarget && tl.isEmpty()) tl = QStringLiteral("—");
        auto* tgtItem = mkRO(tl);
        if (canTarget) {
            tgtItem->setFlags(tgtItem->flags() | Qt::ItemIsDropEnabled);
            if (c->targetIndex < 0)
                tgtItem->setForeground(QColor(0x44, 0x44, 0x44));
        }
        setItem(row, ColTarget, tgtItem);
    }

    setItem(row, ColDuration, isAudio ? mkEdit(durationLabel(row)) : mkRO(durationLabel(row)));
    setItem(row, ColStatus,   mkRO  ({}));

    setRowStatus(row);
}

void CueTableView::setRowStatus(int row) {
    QTableWidgetItem* it = item(row, ColStatus);
    if (!it) return;

    const mcp::Cue* c = m_model->cues.cueAt(row);
    if (!c) return;

    if (!c->isLoaded()) {
        it->setText("broken");
        it->setForeground(kColorBroken);
        return;
    }
    if (m_model->cues.isCuePlaying(row)) {
        it->setText("playing");
        it->setForeground(kColorPlaying);
    } else if (m_model->cues.isFadeActive(row)) {
        it->setText("fading");
        it->setForeground(kColorPlaying);
    } else if (m_model->cues.isCuePending(row)) {
        it->setText("pending");
        it->setForeground(kColorPending);
    } else if (m_model->cues.isArmed(row)) {
        it->setText("armed");
        it->setForeground(kColorArmed);
    } else {
        it->setText("idle");
        it->setForeground(kColorIdle);
    }
}

QString CueTableView::typeLabel(mcp::CueType t) const {
    switch (t) {
        case mcp::CueType::Audio:  return "Audio";
        case mcp::CueType::Start:  return "Start";
        case mcp::CueType::Stop:   return "Stop";
        case mcp::CueType::Fade:   return "Fade";
        case mcp::CueType::Arm:    return "Arm";
        case mcp::CueType::Devamp: return "Devamp";
        case mcp::CueType::Group:  return "Group";
    }
    return "?";
}

int CueTableView::cueDepth(int row) const {
    int depth = 0;
    const mcp::Cue* c = m_model->cues.cueAt(row);
    while (c && c->parentIndex >= 0) {
        ++depth;
        c = m_model->cues.cueAt(c->parentIndex);
    }
    return depth;
}

QString CueTableView::targetLabel(int cueIdx) const {
    const mcp::Cue* c = m_model->cues.cueAt(cueIdx);
    if (!c) return {};
    if (c->type == mcp::CueType::Audio) {
        if (c->path.empty()) return {};
        return QString::fromStdString(std::filesystem::path(c->path).filename().string());
    }
    if (c->type == mcp::CueType::Group) return {};  // groups have no target column
    if (c->targetIndex >= 0) {
        const mcp::Cue* tgt = m_model->cues.cueAt(c->targetIndex);
        if (tgt) {
            QString s = QString::fromStdString(tgt->cueNumber);
            if (!tgt->name.empty())
                s += " – " + QString::fromStdString(tgt->name);
            return s;
        }
    }
    return {};
}

QString CueTableView::durationLabel(int cueIdx) const {
    const mcp::Cue* c = m_model->cues.cueAt(cueIdx);
    if (!c) return {};
    if (c->type == mcp::CueType::Group && c->groupData &&
        c->groupData->mode == mcp::GroupData::Mode::Sync) {
        if (m_model->cues.isSyncGroupBroken(cueIdx)) return "BROKEN";
        const double d = m_model->cues.syncGroupTotalSeconds(cueIdx);
        if (std::isinf(d)) return QString::fromUtf8("\xe2\x88\x9e");  // ∞
        return QString::fromStdString(ShowHelpers::fmtDuration(d));
    }
    if (c->type != mcp::CueType::Audio) return {};
    const double d = (c->duration > 0.0) ? c->duration
                                          : m_model->cues.cueTotalSeconds(cueIdx);
    return QString::fromStdString(ShowHelpers::fmtDuration(d));
}

// ── mouse events ───────────────────────────────────────────────────────────

void CueTableView::mousePressEvent(QMouseEvent* ev) {
    const int row = rowAt(ev->pos().y());

    // Check for expand/collapse click on a Group cue's ▶/▼ indicator.
    if (row >= 0 && ev->button() == Qt::LeftButton) {
        const mcp::Cue* c = m_model->cues.cueAt(row);
        if (c && c->type == mcp::CueType::Group && c->childCount > 0) {
            const QRect nameRect = visualRect(model()->index(row, ColName));
            const int depth  = cueDepth(row);
            const int indentPx = depth * 4 * fontMetrics().averageCharWidth();
            const int toggleX  = nameRect.left() + indentPx;
            if (ev->pos().x() >= toggleX && ev->pos().x() <= toggleX + 24) {
                if (m_collapsed.count(row)) m_collapsed.erase(row);
                else                         m_collapsed.insert(row);
                refresh();
                return;
            }
        }
    }

    QTableWidget::mousePressEvent(ev);

    if (row < 0) {
        clearSelection();
        m_selRow = -1;
        emit rowSelected(-1);
        return;
    }

    // Sync multiSel from Qt selection model
    m_model->multiSel.clear();
    const auto selected = selectionModel()->selectedRows();
    for (const auto& idx : selected)
        m_model->multiSel.insert(idx.row());

    if (m_selRow != row) {
        m_selRow = row;
        emit rowSelected(row);
    }
}

void CueTableView::mouseDoubleClickEvent(QMouseEvent* ev) {
    // Native in-cell editing handles all editable columns — just pass through.
    QTableWidget::mouseDoubleClickEvent(ev);
}

void CueTableView::paintEvent(QPaintEvent* ev) {
    QTableWidget::paintEvent(ev);

    // ── Group bounding boxes ──────────────────────────────────────────────────
    {
        static const QColor kGroupColors[] = {
            QColor(0x28, 0x99, 0x28),   // depth 0: green
            QColor(0x22, 0x77, 0x99),   // depth 1: cyan
            QColor(0x88, 0x66, 0x22),   // depth 2: amber
        };
        QPainter gp(viewport());
        gp.setBrush(Qt::NoBrush);
        for (int gi = 0; gi < rowCount(); ++gi) {
            if (isRowHidden(gi)) continue;
            const mcp::Cue* g = m_model->cues.cueAt(gi);
            if (!g || g->type != mcp::CueType::Group || g->childCount == 0) continue;
            // Find last visible descendant row (stays at gi if collapsed).
            int lastRow = gi;
            if (!m_collapsed.count(gi)) {
                for (int j = gi + 1; j <= gi + g->childCount; ++j)
                    if (!isRowHidden(j)) lastRow = j;
            }

            const QRect topR  = visualRect(model()->index(gi,      0));
            const QRect botR  = visualRect(model()->index(lastRow, ColName));
            const QRect nameH = visualRect(model()->index(gi,      ColName));

            // Frame spans col-0 left edge to ColName right edge.
            const QRect frame(topR.left(), topR.top(),
                              nameH.right() - topR.left(),
                              botR.bottom() - topR.top());

            const int depth = cueDepth(gi);
            gp.setPen(QPen(kGroupColors[depth % 3], 1));
            gp.drawRoundedRect(frame.adjusted(1, 1, -1, -1), 3, 3);
        }
    }

    if (m_dropTargetRow >= 0 && m_dropTargetRow < rowCount()) {
        // Outline only the Target cell
        QPainter p(viewport());
        const QRect r = visualRect(model()->index(m_dropTargetRow, ColTarget));
        p.setPen(QPen(QColor(0x2a, 0x6a, 0xb8), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(1, 1, -2, -2));
    } else if (m_dropInsertRow >= 0) {
        // Draw a horizontal insertion line
        QPainter p(viewport());
        int y;
        if (m_dropInsertRow >= rowCount() && rowCount() > 0)
            y = visualRect(model()->index(rowCount() - 1, 0)).bottom();
        else if (m_dropInsertRow < rowCount())
            y = visualRect(model()->index(m_dropInsertRow, 0)).top();
        else
            y = 0;
        const int x1 = viewport()->width();
        p.setPen(QPen(QColor(0x2a, 0x6a, 0xb8), 2));
        p.drawLine(0, y, x1, y);
        // Small arrow tips
        p.drawLine(0, y - 4, 0, y + 4);
        p.drawLine(x1, y - 4, x1, y + 4);
    }
}

void CueTableView::keyPressEvent(QKeyEvent* ev) {
    if (ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) {
        std::vector<int> rows;
        for (const auto& idx : selectionModel()->selectedRows())
            rows.push_back(idx.row());
        if (!rows.empty()) deleteRows(rows);
        return;
    }

    if (ev->modifiers() == Qt::ControlModifier) {
        if (ev->key() == Qt::Key_C) {
            m_model->clipboard.clear();
            for (const auto& idx : selectionModel()->selectedRows()) {
                const auto* cd = ShowHelpers::sfCueAt(m_model->sf, idx.row());
                if (cd) m_model->clipboard.push_back(*cd);
            }
            ev->accept();
            return;
        }
        if (ev->key() == Qt::Key_V) {
            if (!m_model->clipboard.empty() && !m_model->sf.cueLists.empty()) {
                m_model->pushUndo();
                int ins = (m_selRow >= 0) ? m_selRow + 1 : m_model->cues.cueCount();
                for (auto cd : m_model->clipboard) {
                    cd.cueNumber = ShowHelpers::nextCueNumber(m_model->sf);
                    ShowHelpers::sfInsertBefore(m_model->sf, ins, cd);
                    ++ins;
                }
                std::string err;
                ShowHelpers::rebuildCueList(*m_model, err);
                m_selRow = ins - 1;
                emit rowSelected(m_selRow);
                emit cueListModified();
                refresh();
            }
            ev->accept();
            return;
        }
        if (ev->key() == Qt::Key_D) {
            if (m_selRow >= 0) {
                const auto* cd = ShowHelpers::sfCueAt(m_model->sf, m_selRow);
                if (cd) {
                    m_model->pushUndo();
                    auto copy = *cd;
                    copy.cueNumber = ShowHelpers::nextCueNumber(m_model->sf);
                    ShowHelpers::sfInsertBefore(m_model->sf, m_selRow + 1, std::move(copy));
                    std::string err;
                    ShowHelpers::rebuildCueList(*m_model, err);
                    m_selRow = m_selRow + 1;
                    emit rowSelected(m_selRow);
                    emit cueListModified();
                    refresh();
                }
            }
            ev->accept();
            return;
        }
    }

    QTableWidget::keyPressEvent(ev);
}

// ── drag / drop ────────────────────────────────────────────────────────────

void CueTableView::dragEnterEvent(QDragEnterEvent* ev) {
    if (ev->mimeData()->hasUrls()) {
        ev->acceptProposedAction();
    } else {
        setDropIndicatorShown(false);  // suppress Qt's built-in indicator once per drag
        QTableWidget::dragEnterEvent(ev);
    }
}

void CueTableView::dragMoveEvent(QDragMoveEvent* ev) {
    if (ev->mimeData()->hasUrls()) {
        ev->acceptProposedAction();
        if (m_dropTargetRow != -1 || m_dropInsertRow != -1) {
            m_dropTargetRow = -1;
            m_dropInsertRow = -1;
            viewport()->update();
        }
        return;
    }

    const QPoint pt = ev->position().toPoint();
    const QModelIndex idx = indexAt(pt);

    // Check if we're over a Target cell that can receive a target-setting drop.
    bool isTargetDrop = false;
    if (idx.isValid() && idx.column() == ColTarget) {
        const mcp::Cue* c = m_model->cues.cueAt(idx.row());
        isTargetDrop = c && (c->type == mcp::CueType::Fade  ||
                             c->type == mcp::CueType::Start ||
                             c->type == mcp::CueType::Stop  ||
                             c->type == mcp::CueType::Devamp||
                             c->type == mcp::CueType::Arm);
    }

    int newTargetRow = -1;
    int newInsertRow = -1;

    if (isTargetDrop) {
        newTargetRow = idx.row();
        ev->accept();
    } else {
        if (!idx.isValid()) {
            newInsertRow = rowCount();
        } else {
            const QRect r = visualRect(idx);
            newInsertRow = idx.row();
            if (pt.y() >= r.center().y()) newInsertRow = idx.row() + 1;
        }
        ev->accept();
    }

    if (newTargetRow != m_dropTargetRow || newInsertRow != m_dropInsertRow) {
        m_dropTargetRow = newTargetRow;
        m_dropInsertRow = newInsertRow;
        viewport()->update();
    }
}

void CueTableView::dragLeaveEvent(QDragLeaveEvent* ev) {
    m_dropTargetRow = -1;
    m_dropInsertRow = -1;
    viewport()->update();
    QTableWidget::dragLeaveEvent(ev);
}

void CueTableView::dropEvent(QDropEvent* ev) {
    // Always clear drag indicators on drop.
    m_dropTargetRow = -1;
    m_dropInsertRow = -1;
    setDropIndicatorShown(true);

    if (ev->mimeData()->hasUrls()) {
        // OS file drop — determine insertion row
        int insertBefore = rowAt(ev->position().toPoint().y());
        if (insertBefore < 0) insertBefore = rowCount();

        const auto urls = ev->mimeData()->urls();
        for (const QUrl& url : urls) {
            if (!url.isLocalFile()) continue;
            const QString path = url.toLocalFile();
            const QString lp = path.toLower();
            if (lp.endsWith(".wav") || lp.endsWith(".aiff") || lp.endsWith(".aif")
                || lp.endsWith(".mp3") || lp.endsWith(".flac") || lp.endsWith(".ogg")
                || lp.endsWith(".m4a") || lp.endsWith(".caf"))
            {
                insertAudioCueForPath(path, insertBefore);
                ++insertBefore;
            }
        }
        ev->acceptProposedAction();
        return;
    }

    // Internal drag: either set a cue's target or reorder rows.
    if (m_model->sf.cueLists.empty()) { ev->ignore(); return; }

    const auto selRows = selectionModel()->selectedRows();
    if (selRows.isEmpty()) { ev->ignore(); return; }
    const int srcRow = selRows.first().row();

    const QPoint pt    = ev->position().toPoint();
    const QModelIndex dropIdx = indexAt(pt);

    // ── Target-setting drop: dragged cue onto Target cell of a control cue ──
    if (dropIdx.isValid() && dropIdx.column() == ColTarget) {
        const mcp::Cue* dstCue = m_model->cues.cueAt(dropIdx.row());
        const bool canTarget = dstCue &&
            (dstCue->type == mcp::CueType::Fade  ||
             dstCue->type == mcp::CueType::Start ||
             dstCue->type == mcp::CueType::Stop  ||
             dstCue->type == mcp::CueType::Devamp||
             dstCue->type == mcp::CueType::Arm);
        if (canTarget && srcRow != dropIdx.row()) {
            m_model->pushUndo();
            m_model->cues.setCueTarget(dropIdx.row(), srcRow);
            ShowHelpers::syncSfFromCues(*m_model);
            m_selRow = dropIdx.row();
            ev->setDropAction(Qt::IgnoreAction);
            ev->accept();
            emit rowSelected(m_selRow);
            emit cueListModified();
            refresh();
            return;
        }
    }

    // ── Row reorder ──────────────────────────────────────────────────────────
    int dstRow;
    if (!dropIdx.isValid()) {
        dstRow = rowCount();
    } else {
        const QRect r = visualRect(dropIdx);
        dstRow = dropIdx.row();
        if (pt.y() >= r.center().y()) ++dstRow;
    }

    if (dstRow == srcRow || dstRow == srcRow + 1) { ev->ignore(); return; }
    if (m_model->sf.cueLists.empty()) { ev->ignore(); return; }

    m_model->pushUndo();
    auto cd = ShowHelpers::sfRemoveAt(m_model->sf, srcRow);
    if (cd.type.empty()) { ev->ignore(); return; }  // invalid index

    // Adjust dstRow for the removal: if dstRow > srcRow, it shifts down by the
    // number of flat elements removed (1 for non-group; 1+childCount for group).
    const int blocksRemoved = (cd.type == "group")
        ? (1 + static_cast<int>(cd.children.size()))   // approximate; exact via countAll
        : 1;
    int ins = dstRow;
    if (dstRow > srcRow) ins = std::max(srcRow, dstRow - blocksRemoved);
    ins = std::max(0, ins);

    ShowHelpers::sfInsertBefore(m_model->sf, ins, std::move(cd));

    std::string err;
    ShowHelpers::rebuildCueList(*m_model, err);
    m_selRow = ins;
    // Qt::IgnoreAction prevents startDrag's clearOrRemove() from deleting
    // the source row after we've already refreshed the widget.
    ev->setDropAction(Qt::IgnoreAction);
    ev->accept();
    emit rowSelected(m_selRow);
    emit cueListModified();
    refresh();
}

// ── context menu ───────────────────────────────────────────────────────────

void CueTableView::contextMenuEvent(QContextMenuEvent* ev) {
    const int row = rowAt(ev->pos().y());
    const mcp::Cue* c = (row >= 0) ? m_model->cues.cueAt(row) : nullptr;

    QMenu menu(this);

    if (c) {
        // Playback actions
        auto* actGo   = menu.addAction("Go");
        auto* actStop = menu.addAction("Stop");
        auto* actPanic= menu.addAction("Panic");
        menu.addSeparator();

        connect(actGo, &QAction::triggered, this, [this, row]() {
            m_model->cues.setSelectedIndex(row);
            m_model->cues.go();
        });
        connect(actStop, &QAction::triggered, this, [this, row]() {
            m_model->cues.stop(row);
        });
        connect(actPanic, &QAction::triggered, this, [this]() {
            m_model->cues.panic();
        });

        // Arm (audio/arm cues only)
        if (c->type == mcp::CueType::Audio || c->type == mcp::CueType::Arm) {
            auto* actArm = menu.addAction("Arm");
            connect(actArm, &QAction::triggered, this, [this, row]() {
                m_model->cues.arm(row);
            });
            menu.addSeparator();
        }

        // Edit actions
        auto* actCopy = menu.addAction("Copy");
        auto* actDup  = menu.addAction("Duplicate");
        connect(actCopy, &QAction::triggered, this, [this]() {
            m_model->clipboard.clear();
            for (const auto& idx : selectionModel()->selectedRows()) {
                const auto* cd = ShowHelpers::sfCueAt(m_model->sf, idx.row());
                if (cd) m_model->clipboard.push_back(*cd);
            }
        });
        connect(actDup, &QAction::triggered, this, [this, row]() {
            const auto* cd = ShowHelpers::sfCueAt(m_model->sf, row);
            if (!cd) return;
            m_model->pushUndo();
            auto copy = *cd;
            copy.cueNumber = ShowHelpers::nextCueNumber(m_model->sf);
            ShowHelpers::sfInsertBefore(m_model->sf, row + 1, std::move(copy));
            std::string err;
            ShowHelpers::rebuildCueList(*m_model, err);
            m_selRow = row + 1;
            emit rowSelected(m_selRow);
            emit cueListModified();
            refresh();
        });

        menu.addSeparator();
        auto* actDel = menu.addAction("Delete");
        connect(actDel, &QAction::triggered, this, [this]() {
            std::vector<int> rows;
            for (const auto& idx : selectionModel()->selectedRows())
                rows.push_back(idx.row());
            if (!rows.empty()) deleteRows(rows);
        });
        menu.addSeparator();
    }

    if (!m_model->clipboard.empty()) {
        auto* actPaste = menu.addAction("Paste after");
        connect(actPaste, &QAction::triggered, this, [this, row]() {
            if (m_model->sf.cueLists.empty()) return;
            m_model->pushUndo();
            int ins = (row >= 0) ? row + 1 : m_model->cues.cueCount();
            for (auto cd : m_model->clipboard) {
                cd.cueNumber = ShowHelpers::nextCueNumber(m_model->sf);
                ShowHelpers::sfInsertBefore(m_model->sf, ins, cd);
                ++ins;
            }
            std::string err;
            ShowHelpers::rebuildCueList(*m_model, err);
            m_selRow = ins - 1;
            emit rowSelected(m_selRow);
            emit cueListModified();
            refresh();
        });
        menu.addSeparator();
    }

    // "Create Group from Selection" — available when 2+ rows are selected.
    {
        const auto selRows = selectionModel()->selectedRows();
        if (selRows.size() >= 2) {
            auto* actGroup = menu.addAction("Create Group from Selection");
            connect(actGroup, &QAction::triggered, this, [this, selRows]() {
                std::vector<int> rows;
                for (const auto& idx : selRows) rows.push_back(idx.row());
                std::sort(rows.begin(), rows.end());
                createGroupFromSelection(rows);
            });
            menu.addSeparator();
        }
    }

    // Add cue submenu — insert after currently selected cue (not right-click row).
    const int insertAt  = (m_selRow >= 0) ? m_selRow + 1 : rowCount();
    // Auto-target: if exactly one cue is selected, pre-assign it as target for
    // cue types that reference a target.
    const bool singleSel = (m_selRow >= 0 && selectionModel()->selectedRows().size() == 1);
    const int  autoTarget = singleSel ? m_selRow : -1;
    auto* addMenu = menu.addMenu("Add Cue");
    for (const char* type : {"Audio", "Start", "Stop", "Fade", "Arm", "Devamp", "Group"}) {
        auto* act = addMenu->addAction(type);
        connect(act, &QAction::triggered, this, [this, type, insertAt, autoTarget]() {
            addCueOfType(type, insertAt, autoTarget);
        });
    }

    menu.exec(ev->globalPos());
}

// ── edit commit ────────────────────────────────────────────────────────────

static double parseDuration(const QString& txt) {
    const auto parts = txt.split(':');
    if (parts.size() == 2)
        return parts[0].toInt() * 60.0 + parts[1].toDouble();
    return txt.toDouble();
}

void CueTableView::onCellChanged(int row, int col) {
    if (m_refreshing) return;
    auto* it = item(row, col);
    if (!it) return;
    const QString txt = it->text().trimmed();

    if (col == ColNum) {
        m_model->pushUndo();
        ShowHelpers::setCueNumberChecked(*m_model, row, txt.toStdString());
        ShowHelpers::syncSfFromCues(*m_model);
    } else if (col == ColName) {
        m_model->pushUndo();
        // Strip leading whitespace then optional > / ∨ indicator before storing.
        QString name = txt.trimmed();
        if (name.startsWith(QLatin1Char('>')) || name.startsWith(QChar(0x2228)))
            name = name.mid(1).trimmed();
        m_model->cues.setCueName(row, name.toStdString());
        ShowHelpers::syncSfFromCues(*m_model);
    } else if (col == ColDuration) {
        const double secs = parseDuration(txt);
        if (secs >= 0.0) {
            m_model->pushUndo();
            m_model->cues.setCueDuration(row, secs);
            ShowHelpers::syncSfFromCues(*m_model);
            // Update display to formatted string
            m_refreshing = true;
            it->setText(durationLabel(row));
            m_refreshing = false;
        } else {
            // Restore previous value on bad input
            m_refreshing = true;
            it->setText(durationLabel(row));
            m_refreshing = false;
            return;
        }
    } else {
        return;
    }
    emit cueListModified();
}

// ── mutation helpers ───────────────────────────────────────────────────────

void CueTableView::insertAudioCueForPath(const QString& path, int beforeRow) {
    if (m_model->sf.cueLists.empty())
        m_model->sf.cueLists.push_back({});
    m_model->pushUndo();

    mcp::ShowFile::CueData cd;
    cd.type       = "audio";
    cd.cueNumber  = ShowHelpers::nextCueNumber(m_model->sf);
    cd.name       = QString::fromStdString(
                        std::filesystem::path(path.toStdString()).stem().string()
                    ).toStdString();

    // Store a path relative to baseDir if possible
    if (!m_model->baseDir.empty()) {
        try {
            auto rel = std::filesystem::relative(path.toStdString(), m_model->baseDir);
            cd.path = rel.string();
        } catch (...) {
            cd.path = path.toStdString();
        }
    } else {
        cd.path = path.toStdString();
    }

    ShowHelpers::sfInsertBefore(m_model->sf, beforeRow, std::move(cd));

    std::string err;
    ShowHelpers::rebuildCueList(*m_model, err);
    m_selRow = beforeRow;
    emit rowSelected(m_selRow);
    emit cueListModified();
    refresh();
}

void CueTableView::addCueOfType(const QString& type, int beforeRow, int autoTarget) {
    if (m_model->sf.cueLists.empty())
        m_model->sf.cueLists.push_back({});

    m_model->pushUndo();

    mcp::ShowFile::CueData cd;
    cd.type      = type.toLower().toStdString();
    cd.cueNumber = ShowHelpers::nextCueNumber(m_model->sf);

    // Auto-assign target for cue types that reference another cue.
    if (autoTarget >= 0) {
        const std::string& t = cd.type;
        if (t == "fade" || t == "start" || t == "stop" || t == "arm" || t == "devamp")
            cd.target = autoTarget;
    }

    // Default group mode for new group cues.
    if (cd.type == "group") cd.groupMode = "timeline";

    ShowHelpers::sfInsertBefore(m_model->sf, beforeRow, std::move(cd));

    std::string err;
    ShowHelpers::rebuildCueList(*m_model, err);
    m_selRow = beforeRow;
    emit rowSelected(m_selRow);
    emit cueListModified();
    refresh();
}

void CueTableView::deleteRows(const std::vector<int>& rows) {
    if (m_model->sf.cueLists.empty()) return;
    m_model->pushUndo();

    // Sort descending so earlier removals don't shift indices of later ones.
    auto sorted = rows;
    std::sort(sorted.rbegin(), sorted.rend());
    for (int r : sorted)
        ShowHelpers::sfRemoveAt(m_model->sf, r);

    std::string err;
    ShowHelpers::rebuildCueList(*m_model, err);

    const int newCount = m_model->cues.cueCount();
    if (m_selRow >= newCount) m_selRow = newCount - 1;
    emit rowSelected(m_selRow);
    emit cueListModified();
    refresh();
}

void CueTableView::createGroupFromSelection(const std::vector<int>& rows) {
    if (rows.empty() || m_model->sf.cueLists.empty()) return;
    m_model->pushUndo();

    // Remove selected cues from SF in descending order to keep earlier indices valid.
    std::vector<mcp::ShowFile::CueData> children;
    for (int i = (int)rows.size() - 1; i >= 0; --i)
        children.insert(children.begin(), ShowHelpers::sfRemoveAt(m_model->sf, rows[i]));

    // Build a group CueData containing the removed cues as children.
    mcp::ShowFile::CueData groupCd;
    groupCd.type      = "group";
    groupCd.groupMode = "timeline";
    groupCd.cueNumber = ShowHelpers::nextCueNumber(m_model->sf);
    groupCd.name      = "Group";
    groupCd.children  = std::move(children);

    // Insert the group at the position where the first selected cue was.
    ShowHelpers::sfInsertBefore(m_model->sf, rows.front(), std::move(groupCd));

    std::string err;
    ShowHelpers::rebuildCueList(*m_model, err);
    m_selRow = rows.front();
    emit rowSelected(m_selRow);
    emit cueListModified();
    refresh();
}
