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
#include <numeric>

// ── helpers ────────────────────────────────────────────────────────────────

static const QColor kColorPlaying {0x44, 0xcc, 0x55};
static const QColor kColorPending {0xff, 0xaa, 0x00};
static const QColor kColorArmed   {0xff, 0xee, 0x00};
static const QColor kColorBroken  {0xff, 0x44, 0x44};

// status: 0=idle 1=playing 2=pending 3=armed 4=broken
static QPixmap makeStatusPixmap(int status) {
    const int S = 14;
    QPixmap px(S, S);
    px.fill(Qt::transparent);
    if (status == 0) return px;
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    if (status == 1) {          // green right-pointing triangle
        p.setBrush(kColorPlaying);
        p.setPen(Qt::NoPen);
        QPolygon tri;
        tri << QPoint(2, 1) << QPoint(S - 1, S / 2) << QPoint(2, S - 2);
        p.drawPolygon(tri);
    } else if (status == 2) {   // orange small circle (pending)
        p.setBrush(kColorPending);
        p.setPen(Qt::NoPen);
        p.drawEllipse(3, 3, S - 6, S - 6);
    } else if (status == 3) {   // yellow circle (armed)
        p.setBrush(kColorArmed);
        p.setPen(Qt::NoPen);
        p.drawEllipse(1, 1, S - 2, S - 2);
    } else if (status == 4) {   // red X (broken)
        p.setPen(QPen(kColorBroken, 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(2, 2, S - 2, S - 2);
        p.drawLine(S - 2, 2, 2, S - 2);
    }
    return px;
}

struct TypeInfo { const char* glyph; QColor color; };
static TypeInfo typeInfoFor(mcp::CueType t) {
    switch (t) {
        case mcp::CueType::Group:        return {"▤",  {0x99, 0x99, 0x99}};
        case mcp::CueType::Audio:        return {"♫",  {0x44, 0x99, 0xff}};
        case mcp::CueType::MusicContext: return {"♩",  {0xdd, 0x66, 0xbb}};
        case mcp::CueType::Fade:        return {"〰", {0xaa, 0x77, 0xff}};
        case mcp::CueType::Start:        return {"▷",  {0x44, 0xcc, 0x66}};
        case mcp::CueType::Stop:         return {"□",  {0xff, 0x66, 0x44}};
        case mcp::CueType::Arm:          return {"⊙",  {0xff, 0xbb, 0x44}};
        case mcp::CueType::Devamp:       return {"⤴",  {0x44, 0xcc, 0xee}};
        case mcp::CueType::Marker:       return {"◈",  {0x88, 0xcc, 0x88}};
        case mcp::CueType::Network:      return {"⊹",  {0x44, 0xdd, 0xaa}};
        case mcp::CueType::Midi:         return {"♪",  {0xff, 0x99, 0x44}};
        case mcp::CueType::Timecode:     return {"TC", {0x44, 0xcc, 0xdd}};
        case mcp::CueType::Goto:         return {"→",  {0x44, 0xdd, 0x88}};
        case mcp::CueType::Memo:         return {"✎",  {0x88, 0x88, 0x88}};
        case mcp::CueType::Scriptlet:    return {"λ",  {0xff, 0xaa, 0x44}};
        case mcp::CueType::Snapshot:     return {"📷", {0x44, 0xbb, 0xee}};
        case mcp::CueType::Automation:   return {"∿",  {0xff, 0x77, 0xbb}};
    }
    return {"?", {0x88, 0x88, 0x88}};
}

// ── ctor ───────────────────────────────────────────────────────────────────

CueTableView::CueTableView(AppModel* model, QWidget* parent)
    : QTableWidget(parent), m_model(model)
{
    setColumnCount(ColCount);
    setHorizontalHeaderLabels({{""},{""},{"#"},{"Name"},{"Target"},{"Pre-Wait"},{"Duration"},{"→"}});

    horizontalHeader()->setSectionResizeMode(ColStatus,   QHeaderView::Fixed);
    horizontalHeader()->setSectionResizeMode(ColType,     QHeaderView::Fixed);
    horizontalHeader()->setSectionResizeMode(ColNum,      QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(ColName,     QHeaderView::Stretch);
    horizontalHeader()->setSectionResizeMode(ColTarget,   QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(ColPreWait,  QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(ColDuration, QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(ColFollow,   QHeaderView::Fixed);
    setColumnWidth(ColStatus, 20);
    setColumnWidth(ColType,   26);
    setColumnWidth(ColFollow, 22);

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
            const auto* c = m_model->cues().cueAt(gi);
            if (c && c->type == mcp::CueType::Group && c->childCount > 0)
                validCollapsed.insert(gi);
        }
        m_collapsed = std::move(validCollapsed);
    }

    m_refreshing = true;
    const int n = m_model->cues().cueCount();
    setRowCount(n);
    for (int i = 0; i < n; ++i) populateRow(i);

    // Hide rows whose ancestor group is collapsed.
    for (int i = 0; i < n; ++i) {
        bool hidden = false;
        const mcp::Cue* c = m_model->cues().cueAt(i);
        int pi = c ? c->parentIndex : -1;
        while (pi >= 0) {
            if (m_collapsed.count(pi)) { hidden = true; break; }
            const mcp::Cue* par = m_model->cues().cueAt(pi);
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
    m_rowProgress.resize(n);
    for (int i = 0; i < n; ++i) {
        setRowStatus(i);
        auto& rp       = m_rowProgress[i];
        rp.preWaitFrac = m_model->cues().cuePendingFraction(i);
        rp.sliceFrac   = m_model->cues().cueSliceProgress(i, rp.sliceIsLoop);
    }
    viewport()->update();
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
    const mcp::Cue* c = m_model->cues().cueAt(row);
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

    // Status icon (filled in by setRowStatus below)
    {
        auto* it = mkRO({});
        it->setTextAlignment(Qt::AlignCenter);
        setItem(row, ColStatus, it);
    }

    // Type glyph — same icon as the toolbar "Add X cue" buttons
    {
        const auto ti = typeInfoFor(c->type);
        auto* it = mkRO(QString::fromUtf8(ti.glyph));
        it->setTextAlignment(Qt::AlignCenter);
        it->setForeground(ti.color);
        setItem(row, ColType, it);
    }

    setItem(row, ColNum,  mkEdit(QString::fromStdString(c->cueNumber)));
    setItem(row, ColName, mkEdit(namePrefix + QString::fromStdString(c->name)));

    // Target cell: control cues get ItemIsDropEnabled so dragging a cue onto it
    // sets the target. Show "—" when no target is assigned yet.
    const bool canTarget = (c->type == mcp::CueType::Fade   ||
                            c->type == mcp::CueType::Start  ||
                            c->type == mcp::CueType::Stop   ||
                            c->type == mcp::CueType::Devamp ||
                            c->type == mcp::CueType::Arm    ||
                            c->type == mcp::CueType::Marker ||
                            c->type == mcp::CueType::Goto);
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

    // Pre-wait (read-only; edit in Inspector)
    {
        auto* it = mkRO(c->preWaitSeconds > 0.0
            ? QString::fromStdString(ShowHelpers::fmtTime(c->preWaitSeconds))
            : QString{});
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        setItem(row, ColPreWait, it);
    }

    setItem(row, ColDuration, isAudio ? mkEdit(durationLabel(row)) : mkRO(durationLabel(row)));

    // Follow mode: C = auto-continue, F = auto-follow
    {
        QString fl;
        if (c->autoContinue && c->autoFollow) fl = "CF";
        else if (c->autoContinue)             fl = "C";
        else if (c->autoFollow)               fl = "F";
        auto* it = mkRO(fl);
        it->setTextAlignment(Qt::AlignCenter);
        if (!fl.isEmpty()) it->setForeground(QColor(0x88, 0xcc, 0xff));
        setItem(row, ColFollow, it);
    }

    setRowStatus(row);
}

void CueTableView::setRowStatus(int row) {
    QTableWidgetItem* it = item(row, ColStatus);
    if (!it) return;
    const mcp::Cue* c = m_model->cues().cueAt(row);
    if (!c) return;

    int status = 0;
    if (!c->isLoaded() || m_model->scriptletErrorCues.count(row)) {
        status = 4;
    } else if (m_model->cues().isCuePlaying(row) || m_model->cues().isFadeActive(row)
            || m_model->isScriptletCuePlaying(row)) {
        status = 1;
    } else if (m_model->cues().isCuePending(row)) {
        status = 2;
    } else if (m_model->cues().isArmed(row)) {
        status = 3;
    }
    it->setIcon(QIcon(makeStatusPixmap(status)));
}

QString CueTableView::typeLabel(mcp::CueType t) const {
    switch (t) {
        case mcp::CueType::Audio:        return "Audio";
        case mcp::CueType::Start:        return "Start";
        case mcp::CueType::Stop:         return "Stop";
        case mcp::CueType::Fade:         return "Fade";
        case mcp::CueType::Arm:          return "Arm";
        case mcp::CueType::Devamp:       return "Devamp";
        case mcp::CueType::Group:        return "Group";
        case mcp::CueType::MusicContext: return "MC";
        case mcp::CueType::Marker:       return "Marker";
        case mcp::CueType::Network:      return "Network";
        case mcp::CueType::Midi:         return "MIDI";
        case mcp::CueType::Timecode:     return "Timecode";
        case mcp::CueType::Goto:         return "Goto";
        case mcp::CueType::Memo:         return "Memo";
        case mcp::CueType::Scriptlet:    return "Script";
        case mcp::CueType::Snapshot:     return "Snapshot";
        case mcp::CueType::Automation:   return "Auto";
    }
    return "?";
}

int CueTableView::cueDepth(int row) const {
    int depth = 0;
    const mcp::Cue* c = m_model->cues().cueAt(row);
    while (c && c->parentIndex >= 0) {
        ++depth;
        c = m_model->cues().cueAt(c->parentIndex);
    }
    return depth;
}

QString CueTableView::targetLabel(int cueIdx) const {
    const mcp::Cue* c = m_model->cues().cueAt(cueIdx);
    if (!c) return {};
    if (c->type == mcp::CueType::Audio) {
        if (c->path.empty()) return {};
        return QString::fromStdString(std::filesystem::path(c->path).filename().string());
    }
    if (c->type == mcp::CueType::Group) return {};  // groups have no target column
    if (c->crossListNumericId != -1) {
        for (int li = 0; li < m_model->listCount(); ++li) {
            if (li < (int)m_model->sf.cueLists.size() &&
                m_model->sf.cueLists[static_cast<size_t>(li)].numericId == c->crossListNumericId) {
                const mcp::Cue* tgt = m_model->cueListAt(li).cueAt(c->crossListFlatIdx);
                if (tgt) {
                    QString listName = QString::fromStdString(
                        m_model->sf.cueLists[static_cast<size_t>(li)].name);
                    QString s = listName + "/" + QString::fromStdString(tgt->cueNumber);
                    if (!tgt->name.empty())
                        s += " – " + QString::fromStdString(tgt->name);
                    return s;
                }
                break;
            }
        }
        return {};
    }
    if (c->targetIndex >= 0) {
        const mcp::Cue* tgt = m_model->cues().cueAt(c->targetIndex);
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
    const mcp::Cue* c = m_model->cues().cueAt(cueIdx);
    if (!c) return {};
    if (c->type == mcp::CueType::Group && c->groupData &&
        c->groupData->mode == mcp::GroupData::Mode::Sync) {
        if (m_model->cues().isSyncGroupBroken(cueIdx)) return "BROKEN";
        const double d = m_model->cues().syncGroupTotalSeconds(cueIdx);
        if (std::isinf(d)) return QString::fromUtf8("\xe2\x88\x9e");  // ∞
        return QString::fromStdString(ShowHelpers::fmtDuration(d));
    }
    if (c->type == mcp::CueType::Timecode && c->tcStartTC < c->tcEndTC) {
        const int64_t frames = mcp::tcToFrames(c->tcEndTC,   c->tcFps)
                             - mcp::tcToFrames(c->tcStartTC, c->tcFps);
        const mcp::TcRate r  = mcp::tcRateFor(c->tcFps);
        const double secs    = static_cast<double>(frames) * r.rateDen
                               / (static_cast<double>(r.nomFPS) * r.rateNum);
        return QString::fromStdString(ShowHelpers::fmtDuration(secs));
    }
    if (c->type != mcp::CueType::Audio) return {};
    const double d = (c->duration > 0.0) ? c->duration
                                          : m_model->cues().cueTotalSeconds(cueIdx);
    return QString::fromStdString(ShowHelpers::fmtDuration(d));
}

// ── mouse events ───────────────────────────────────────────────────────────

void CueTableView::mousePressEvent(QMouseEvent* ev) {
    const int row = rowAt(ev->pos().y());

    // Check for expand/collapse click on a Group cue's ▶/▼ indicator.
    if (row >= 0 && ev->button() == Qt::LeftButton) {
        const mcp::Cue* c = m_model->cues().cueAt(row);
        if (c && c->type == mcp::CueType::Group && c->childCount > 0) {
            const QRect nameRect = visualRect(model()->index(row, ColName));
            const int depth  = cueDepth(row);
            const int indentPx = depth * 4 * fontMetrics().averageCharWidth();
            const int toggleX  = nameRect.left() + indentPx;
            if (ev->pos().x() >= toggleX && ev->pos().x() <= toggleX + 24) {
                if (m_collapsed.count(row)) m_collapsed.erase(row);
                else                         m_collapsed.insert(row);
                if (m_selRow < 0) m_selRow = row;
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
            const mcp::Cue* g = m_model->cues().cueAt(gi);
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

    if (m_dropReplaceRow >= 0 && m_dropReplaceRow < rowCount()) {
        // Full-row outline: indicates the dragged audio file will REPLACE this cue's media.
        QPainter p(viewport());
        const QRect topR = visualRect(model()->index(m_dropReplaceRow, 0));
        const QRect botR = visualRect(model()->index(m_dropReplaceRow, columnCount() - 1));
        const QRect rowR(topR.left(), topR.top(),
                         botR.right() - topR.left(),
                         botR.bottom() - topR.top());
        p.setPen(QPen(QColor(0xff, 0xff, 0xff), 2));
        p.setBrush(QColor(0xff, 0xff, 0xff, 30));
        p.drawRect(rowR.adjusted(1, 1, -2, -2));
    } else if (m_dropTargetRow >= 0 && m_dropTargetRow < rowCount()) {
        // Outline only the Target cell
        QPainter p(viewport());
        const QRect r = visualRect(model()->index(m_dropTargetRow, ColTarget));
        p.setPen(QPen(QColor(0x2a, 0x6a, 0xb8), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(1, 1, -2, -2));
    } else if (m_dropInsertRow >= 0) {
        // Draw a horizontal insertion line.
        // For inside-group drops: indented line at group's child depth.
        // For normal drops: full-width line.
        QPainter p(viewport());

        int y;
        if (m_dropInsideGroup && m_dropGroupRow >= 0) {
            // Line appears after the last visible row inside the group.
            int y_row = m_dropGroupRow;
            const mcp::Cue* grp = m_model->cues().cueAt(m_dropGroupRow);
            if (grp && grp->childCount > 0) {
                const int lastChild = m_dropGroupRow + grp->childCount;
                for (int r = lastChild; r > m_dropGroupRow; --r) {
                    if (!isRowHidden(r)) { y_row = r; break; }
                }
            }
            y = visualRect(model()->index(y_row, 0)).bottom();
        } else if (m_dropInsertRow >= rowCount() && rowCount() > 0) {
            y = visualRect(model()->index(rowCount() - 1, 0)).bottom();
        } else if (m_dropInsertRow < rowCount()) {
            y = visualRect(model()->index(m_dropInsertRow, 0)).top();
        } else {
            y = 0;
        }

        const int x1 = viewport()->width();
        int x0 = 0;
        if (m_dropInsideGroup && m_dropGroupRow >= 0) {
            const int gDepth = cueDepth(m_dropGroupRow);
            x0 = (gDepth + 1) * 4 * fontMetrics().averageCharWidth();
        }

        p.setPen(QPen(QColor(0x2a, 0x6a, 0xb8), 2));
        p.drawLine(x0, y, x1, y);
        p.drawLine(x0, y - 4, x0, y + 4);
        p.drawLine(x1, y - 4, x1, y + 4);
    }

    // ── Progress overlays (pre-wait bar + slice progress bar/pie) ─────────────
    if (!m_rowProgress.empty()) {
        QPainter pp(viewport());
        pp.setRenderHint(QPainter::Antialiasing);
        pp.setPen(Qt::NoPen);

        for (int row = 0; row < rowCount(); ++row) {
            if (isRowHidden(row) || row >= (int)m_rowProgress.size()) continue;
            const auto& rp = m_rowProgress[row];

            // Pre-wait: fill bar in the PreWait cell
            if (rp.preWaitFrac >= 0.0) {
                const QRect cell = visualRect(model()->index(row, ColPreWait));
                pp.setBrush(QColor(0xff, 0x88, 0x00, 40));
                pp.drawRect(cell);
                if (rp.preWaitFrac > 0.0) {
                    QRect fill = cell;
                    fill.setWidth(std::max(2, static_cast<int>(cell.width() * rp.preWaitFrac)));
                    pp.setBrush(QColor(0xff, 0x88, 0x00, 170));
                    pp.drawRect(fill);
                }
            }

            // Slice progress in the Duration cell
            if (rp.sliceFrac >= 0.0) {
                const QRect cell = visualRect(model()->index(row, ColDuration));
                if (rp.sliceIsLoop) {
                    // Pie chart: one full revolution = one loop iteration
                    const int S = std::min(cell.height() - 4, 14);
                    const QRect pie(cell.left() + 3, cell.center().y() - S / 2, S, S);
                    pp.setBrush(QColor(0x22, 0x22, 0x22));
                    pp.drawEllipse(pie);
                    pp.setBrush(kColorPlaying);
                    pp.drawPie(pie, 90 * 16,
                               -static_cast<int>(rp.sliceFrac * 360.0 * 16));
                    pp.setPen(QPen(QColor(0x55, 0x55, 0x55), 1));
                    pp.setBrush(Qt::NoBrush);
                    pp.drawEllipse(pie);
                    pp.setPen(Qt::NoPen);
                } else {
                    // Linear bar
                    pp.setBrush(QColor(0x44, 0xcc, 0x55, 40));
                    pp.drawRect(cell);
                    if (rp.sliceFrac > 0.0) {
                        QRect fill = cell;
                        fill.setWidth(std::max(2, static_cast<int>(cell.width() * rp.sliceFrac)));
                        pp.setBrush(QColor(0x44, 0xcc, 0x55, 170));
                        pp.drawRect(fill);
                    }
                }
            }
        }
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

    if (ev->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
        if (ev->key() == Qt::Key_C) {
            m_model->clipboard.clear();
            for (const auto& idx : selectionModel()->selectedRows()) {
                const auto* cd = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), idx.row());
                if (cd) m_model->clipboard.push_back(*cd);
            }
            ev->accept();
            return;
        }
        if (ev->key() == Qt::Key_X) {
            m_model->clipboard.clear();
            std::vector<int> rows;
            for (const auto& idx : selectionModel()->selectedRows()) {
                rows.push_back(idx.row());
                const auto* cd = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), idx.row());
                if (cd) m_model->clipboard.push_back(*cd);
            }
            if (!rows.empty()) {
                // Adjust same-list target indices in clipboard: removing cues shifts
                // any same-list target that comes after the cut position.
                if (!m_model->sf.cueLists.empty()) {
                    const int listId = m_model->sf.cueLists[
                        static_cast<size_t>(m_model->activeListIdx())].numericId;
                    auto sortedRows = rows;
                    std::sort(sortedRows.begin(), sortedRows.end());
                    for (auto& cb : m_model->clipboard) {
                        if (cb.targetListId != listId) continue;
                        bool removed = false;
                        int shift = 0;
                        for (int r : sortedRows) {
                            if (r == cb.target) { removed = true; break; }
                            if (r < cb.target) ++shift;
                        }
                        if (removed) cb.target = -1;
                        else         cb.target -= shift;
                    }
                }
                deleteRows(rows);
            }
            ev->accept();
            return;
        }
        if (ev->key() == Qt::Key_V) {
            if (!m_model->clipboard.empty() && !m_model->sf.cueLists.empty()) {
                m_model->pushUndo();
                int ins = (m_selRow >= 0) ? m_selRow + 1 : m_model->cues().cueCount();
                std::string err;
                for (auto cd : m_model->clipboard) {
                    cd.cueNumber = ShowHelpers::nextCueNumber(m_model->sf);
                    ShowHelpers::insertEngineCue(*m_model, m_model->activeListIdx(), ins, std::move(cd), err);
                    ++ins;
                }
                m_selRow = ins - 1;
                emit rowSelected(m_selRow);
                emit cueListModified();
                refresh();
            }
            ev->accept();
            return;
        }
        if (ev->key() == Qt::Key_D) {
            // Delete (clear) the cue number of all selected cues.
            const auto selected = selectionModel()->selectedRows();
            if (!selected.isEmpty()) {
                m_model->pushUndo();
                for (const auto& idx : selected)
                    ShowHelpers::setCueNumberChecked(*m_model, idx.row(), "");
                emit cueListModified();
                refresh();
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

        const QPoint pt      = ev->position().toPoint();
        const int hoverRow   = rowAt(pt.y());

        // Detect single audio file drag so we can show replace vs. insert feedback.
        bool isSingleAudio = false;
        const auto& urls = ev->mimeData()->urls();
        if (urls.size() == 1 && urls.first().isLocalFile()) {
            const QString lp = urls.first().toLocalFile().toLower();
            isSingleAudio = lp.endsWith(".wav") || lp.endsWith(".aiff") || lp.endsWith(".aif")
                || lp.endsWith(".mp3") || lp.endsWith(".flac") || lp.endsWith(".ogg")
                || lp.endsWith(".m4a") || lp.endsWith(".aac") || lp.endsWith(".caf")
                || lp.endsWith(".opus") || lp.endsWith(".wv")  || lp.endsWith(".ape");
        }

        int newReplaceRow = -1;
        int newInsertRow  = -1;

        if (isSingleAudio && hoverRow >= 0) {
            const mcp::Cue* hc = m_model->cues().cueAt(hoverRow);
            if (hc && hc->type == mcp::CueType::Audio) {
                newReplaceRow = hoverRow;   // replace mode — no insert line
            } else {
                const QRect r = visualRect(model()->index(hoverRow, 0));
                newInsertRow  = (pt.y() >= r.center().y()) ? hoverRow + 1 : hoverRow;
            }
        } else {
            if (hoverRow < 0) {
                newInsertRow = rowCount();
            } else {
                const QRect r = visualRect(model()->index(hoverRow, 0));
                newInsertRow  = (pt.y() >= r.center().y()) ? hoverRow + 1 : hoverRow;
            }
        }

        if (newReplaceRow != m_dropReplaceRow || newInsertRow != m_dropInsertRow
                || m_dropTargetRow != -1) {
            m_dropTargetRow  = -1;
            m_dropReplaceRow = newReplaceRow;
            m_dropInsertRow  = newInsertRow;
            viewport()->update();
        }
        return;
    }

    const QPoint pt = ev->position().toPoint();
    const QModelIndex idx = indexAt(pt);

    // Check if we're over a Target cell that can receive a target-setting drop.
    bool isTargetDrop = false;
    if (idx.isValid() && idx.column() == ColTarget) {
        const mcp::Cue* c = m_model->cues().cueAt(idx.row());
        isTargetDrop = c && (c->type == mcp::CueType::Fade   ||
                             c->type == mcp::CueType::Start  ||
                             c->type == mcp::CueType::Stop   ||
                             c->type == mcp::CueType::Devamp ||
                             c->type == mcp::CueType::Arm    ||
                             c->type == mcp::CueType::Marker ||
                             c->type == mcp::CueType::Goto);
    }

    int newTargetRow    = -1;
    int newInsertRow    = -1;
    bool newInsideGroup = false;
    int  newGroupRow    = -1;

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

        // Check if this insert position is at the "end of" a group, offering
        // inside-group (default) vs after-group (cursor left) semantics.
        if (newInsertRow > 0) {
            const mcp::Cue* prevCue = m_model->cues().cueAt(newInsertRow - 1);
            if (prevCue) {
                int candidateGroup = -1;
                if (prevCue->type == mcp::CueType::Group && prevCue->childCount == 0) {
                    // Empty group: bottom half of the group row itself
                    candidateGroup = newInsertRow - 1;
                } else if (prevCue->parentIndex >= 0) {
                    const mcp::Cue* par = m_model->cues().cueAt(prevCue->parentIndex);
                    // prevCue is the last descendant of par when its flat index equals
                    // par's flat index + par's total childCount.
                    if (par && prevCue->parentIndex + par->childCount == newInsertRow - 1)
                        candidateGroup = prevCue->parentIndex;
                }
                if (candidateGroup >= 0) {
                    // Default = inside; cursor to the left of the group's own left edge = outside.
                    const int gDepth   = cueDepth(candidateGroup);
                    const int threshold = gDepth * 4 * fontMetrics().averageCharWidth() + 24;
                    if (pt.x() > threshold) {
                        newInsideGroup = true;
                        newGroupRow    = candidateGroup;
                    }
                }
            }
        }
    }

    if (newTargetRow != m_dropTargetRow || newInsertRow != m_dropInsertRow
        || newInsideGroup != m_dropInsideGroup || newGroupRow != m_dropGroupRow) {
        m_dropTargetRow   = newTargetRow;
        m_dropInsertRow   = newInsertRow;
        m_dropInsideGroup = newInsideGroup;
        m_dropGroupRow    = newGroupRow;
        viewport()->update();
    }
}

void CueTableView::dragLeaveEvent(QDragLeaveEvent* ev) {
    m_dropTargetRow   = -1;
    m_dropInsertRow   = -1;
    m_dropInsideGroup = false;
    m_dropGroupRow    = -1;
    m_dropReplaceRow  = -1;
    viewport()->update();
    QTableWidget::dragLeaveEvent(ev);
}

void CueTableView::dropEvent(QDropEvent* ev) {
    // Always clear drag indicators on drop.
    m_dropTargetRow   = -1;
    m_dropInsertRow   = -1;
    m_dropInsideGroup = false;
    m_dropGroupRow    = -1;
    m_dropReplaceRow  = -1;
    setDropIndicatorShown(true);

    if (ev->mimeData()->hasUrls()) {
        const auto urls = ev->mimeData()->urls();

        // Collect audio-file URLs only
        QStringList audioPaths;
        for (const QUrl& url : urls) {
            if (!url.isLocalFile()) continue;
            const QString p  = url.toLocalFile();
            const QString lp = p.toLower();
            if (lp.endsWith(".wav") || lp.endsWith(".aiff") || lp.endsWith(".aif")
                || lp.endsWith(".mp3") || lp.endsWith(".flac") || lp.endsWith(".ogg")
                || lp.endsWith(".m4a") || lp.endsWith(".aac") || lp.endsWith(".caf")
                || lp.endsWith(".opus") || lp.endsWith(".wv")  || lp.endsWith(".ape"))
                audioPaths << p;
        }

        // Single file dropped ON an existing audio cue → replace its media
        const int hoverRow = rowAt(ev->position().toPoint().y());
        if (audioPaths.size() == 1 && hoverRow >= 0) {
            const mcp::Cue* hc = m_model->cues().cueAt(hoverRow);
            if (hc && hc->type == mcp::CueType::Audio) {
                replaceAudioForRow(hoverRow, audioPaths.first());
                ev->acceptProposedAction();
                return;
            }
        }

        // Otherwise insert new audio cues
        int insertBefore = hoverRow < 0 ? rowCount() : hoverRow;
        for (const QString& path : audioPaths) {
            insertAudioCueForPath(path, insertBefore);
            ++insertBefore;
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
        const mcp::Cue* dstCue = m_model->cues().cueAt(dropIdx.row());
        const bool canTarget = dstCue &&
            (dstCue->type == mcp::CueType::Fade   ||
             dstCue->type == mcp::CueType::Start  ||
             dstCue->type == mcp::CueType::Stop   ||
             dstCue->type == mcp::CueType::Devamp ||
             dstCue->type == mcp::CueType::Arm    ||
             dstCue->type == mcp::CueType::Marker ||
             dstCue->type == mcp::CueType::Goto);
        if (canTarget && srcRow != dropIdx.row()) {
            m_model->pushUndo();
            m_model->cues().setCueTarget(dropIdx.row(), srcRow);
            ShowHelpers::syncSfFromCues(*m_model);
            // Ensure targetListId is absolute (same list); syncSfFromCues restores
            // the old snapshot value which may be -1 for cues created before this fix.
            if (auto* sfCue = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), dropIdx.row())) {
                sfCue->targetListId = m_model->sf.cueLists[
                    static_cast<size_t>(m_model->activeListIdx())].numericId;
            }
            m_selRow = dropIdx.row();
            ev->setDropAction(Qt::IgnoreAction);
            ev->accept();
            emit rowSelected(m_selRow);
            emit cueListModified();
            refresh();
            return;
        }
    }

    // ── Inside-group drop (append as last child of a group) ─────────────────
    // Re-detect the inside-group state using cursor position (indicator state was cleared).
    {
        const int newInsertRow = [&]() -> int {
            if (!dropIdx.isValid()) return rowCount();
            const QRect r = visualRect(dropIdx);
            return dropIdx.row() + (pt.y() >= r.center().y() ? 1 : 0);
        }();

        if (newInsertRow > 0) {
            const mcp::Cue* prevCue = m_model->cues().cueAt(newInsertRow - 1);
            if (prevCue) {
                int candidateGroup = -1;
                if (prevCue->type == mcp::CueType::Group && prevCue->childCount == 0) {
                    candidateGroup = newInsertRow - 1;
                } else if (prevCue->parentIndex >= 0) {
                    const mcp::Cue* par = m_model->cues().cueAt(prevCue->parentIndex);
                    if (par && prevCue->parentIndex + par->childCount == newInsertRow - 1)
                        candidateGroup = prevCue->parentIndex;
                }
                if (candidateGroup >= 0 && candidateGroup != srcRow) {
                    const int gDepth   = cueDepth(candidateGroup);
                    const int threshold = gDepth * 4 * fontMetrics().averageCharWidth() + 24;
                    if (pt.x() > threshold) {
                        // Drop inside the group.
                        const mcp::Cue* srcCueEng = m_model->cues().cueAt(srcRow);
                        const int blocksRm = (srcCueEng && srcCueEng->type == mcp::CueType::Group)
                                             ? (1 + srcCueEng->childCount) : 1;
                        m_model->pushUndo();
                        auto cd = ShowHelpers::sfRemoveAt(m_model->sf, m_model->activeListIdx(), srcRow);
                        if (cd.type.empty()) { ev->ignore(); return; }
                        int adjGroup = candidateGroup;
                        if (srcRow < candidateGroup) adjGroup -= blocksRm;
                        ShowHelpers::sfAppendToGroup(m_model->sf, m_model->activeListIdx(), adjGroup, std::move(cd));
                        std::string err;
                        ShowHelpers::rebuildCueList(*m_model, m_model->activeListIdx(), err);
                        const mcp::Cue* grp = m_model->cues().cueAt(adjGroup);
                        m_selRow = grp ? (adjGroup + grp->childCount) : adjGroup;
                        ev->setDropAction(Qt::IgnoreAction);
                        ev->accept();
                        emit rowSelected(m_selRow);
                        emit cueListModified();
                        refresh();
                        return;
                    }
                }
            }
        }
    }

    // ── Row reorder (multi-select) ───────────────────────────────────────────
    // Collect all selected rows, sorted ascending.
    std::vector<int> selSorted;
    for (const auto& mi : selectionModel()->selectedRows())
        selSorted.push_back(mi.row());
    std::sort(selSorted.begin(), selSorted.end());

    // Filter to top-level selected rows (skip rows whose ancestor is also selected).
    std::set<int> selSet(selSorted.begin(), selSorted.end());
    std::vector<int> topLevel;
    for (int r : selSorted) {
        const mcp::Cue* c = m_model->cues().cueAt(r);
        bool skip = false;
        for (int p = c ? c->parentIndex : -1; p >= 0; ) {
            if (selSet.count(p)) { skip = true; break; }
            const mcp::Cue* pc = m_model->cues().cueAt(p);
            p = pc ? pc->parentIndex : -1;
        }
        if (!skip) topLevel.push_back(r);
    }
    if (topLevel.empty()) { ev->ignore(); return; }

    // Pre-compute flat block size for each top-level row (group = 1 + all descendants).
    std::vector<int> blockSizes;
    for (int r : topLevel) {
        const mcp::Cue* c = m_model->cues().cueAt(r);
        blockSizes.push_back(1 + (c && c->type == mcp::CueType::Group ? c->childCount : 0));
    }
    const int totalMoved = std::accumulate(blockSizes.begin(), blockSizes.end(), 0);

    // Destination row (in the original flat numbering).
    int dstRow;
    if (!dropIdx.isValid()) {
        dstRow = rowCount();
    } else {
        const QRect r = visualRect(dropIdx);
        dstRow = dropIdx.row() + (pt.y() >= r.center().y() ? 1 : 0);
    }

    // Noop: dstRow falls within or immediately after an existing block boundary
    // such that the result would be identical to the current order.
    {
        bool noop = (dstRow == topLevel.front());  // drop onto start of first block
        if (!noop) {
            // Also noop if dstRow == end of last block (no movement).
            noop = (dstRow == topLevel.back() + blockSizes.back());
        }
        if (!noop) {
            // Noop if dstRow falls strictly inside any block (can't insert there).
            for (size_t i = 0; i < topLevel.size(); ++i) {
                if (dstRow > topLevel[i] && dstRow < topLevel[i] + blockSizes[i]) {
                    noop = true; break;
                }
            }
        }
        if (noop) { ev->ignore(); return; }
    }

    // Compute adjusted insertion point after hypothetical removal of all blocks.
    int adjustedDst = dstRow;
    for (size_t i = 0; i < topLevel.size(); ++i) {
        if (topLevel[i] + blockSizes[i] <= dstRow)
            adjustedDst -= blockSizes[i];
    }
    adjustedDst = std::max(0, adjustedDst);

    if (m_model->sf.cueLists.empty()) { ev->ignore(); return; }
    m_model->pushUndo();

    // Remove top-level blocks back-to-front so earlier indices stay valid.
    std::vector<mcp::ShowFile::CueData> removed;
    for (int i = (int)topLevel.size() - 1; i >= 0; --i) {
        auto cd = ShowHelpers::sfRemoveAt(m_model->sf, m_model->activeListIdx(), topLevel[i]);
        if (!cd.type.empty()) removed.push_back(std::move(cd));
    }
    std::reverse(removed.begin(), removed.end());  // restore original forward order

    // Re-insert all blocks at adjustedDst in order.
    int ins = adjustedDst;
    for (size_t i = 0; i < removed.size(); ++i) {
        ShowHelpers::sfInsertBefore(m_model->sf, m_model->activeListIdx(), ins, removed[i]);
        ins += blockSizes[i];
    }

    // Fix stale target flat-indices in SF for the multi-block move.
    // Build a mapping: old flat index → new flat index.
    {
        const int srcListId = m_model->sf.cueLists[
            static_cast<size_t>(m_model->activeListIdx())].numericId;

        auto mapIdx = [&](int T) -> int {
            if (T < 0) return T;
            // Check if T is inside one of the moved blocks.
            int cumulative = 0;
            for (size_t i = 0; i < topLevel.size(); ++i) {
                if (T >= topLevel[i] && T < topLevel[i] + blockSizes[i])
                    return adjustedDst + cumulative + (T - topLevel[i]);
                cumulative += blockSizes[i];
            }
            // T is not in any moved block — compute compressed index.
            int movedBefore = 0;
            for (size_t i = 0; i < topLevel.size(); ++i) {
                if (topLevel[i] + blockSizes[i] <= T) movedBefore += blockSizes[i];
            }
            const int compressed = T - movedBefore;
            return (compressed >= adjustedDst) ? compressed + totalMoved : compressed;
        };

        for (auto& cl : m_model->sf.cueLists) {
            std::function<void(std::vector<mcp::ShowFile::CueData>&)> fix;
            fix = [&](std::vector<mcp::ShowFile::CueData>& cues) {
                for (auto& cdata : cues) {
                    const bool refersToSrc = (cdata.targetListId == srcListId ||
                        (cdata.targetListId == -1 && cl.numericId == srcListId));
                    if (refersToSrc && cdata.target >= 0)
                        cdata.target = mapIdx(cdata.target);
                    if (!cdata.children.empty()) fix(cdata.children);
                }
            };
            fix(cl.cues);
        }
    }

    ShowHelpers::moveEngineCues(*m_model, m_model->activeListIdx(),
                                topLevel, blockSizes, dstRow, adjustedDst);
    m_selRow = adjustedDst;
    ev->setDropAction(Qt::IgnoreAction);
    ev->accept();
    emit rowSelected(m_selRow);
    emit cueListModified();
    refresh();
}

// ── context menu ───────────────────────────────────────────────────────────

void CueTableView::contextMenuEvent(QContextMenuEvent* ev) {
    const int row = rowAt(ev->pos().y());
    const mcp::Cue* c = (row >= 0) ? m_model->cues().cueAt(row) : nullptr;

    QMenu menu(this);

    if (c) {
        // Playback actions
        auto* actGo   = menu.addAction("Go");
        auto* actStop = menu.addAction("Stop");
        auto* actPanic= menu.addAction("Panic");
        menu.addSeparator();

        connect(actGo, &QAction::triggered, this, [this, row]() {
            m_model->cues().setSelectedIndex(row);
            m_model->go();
        });
        connect(actStop, &QAction::triggered, this, [this, row]() {
            m_model->cues().stop(row);
        });
        connect(actPanic, &QAction::triggered, this, [this]() {
            m_model->cues().panic();
        });

        // Arm (audio/arm cues only)
        if (c->type == mcp::CueType::Audio || c->type == mcp::CueType::Arm) {
            auto* actArm = menu.addAction("Arm");
            connect(actArm, &QAction::triggered, this, [this, row]() {
                m_model->cues().arm(row);
            });
            menu.addSeparator();
        }

        // Edit actions
        auto* actCut  = menu.addAction("Cut");
        auto* actCopy = menu.addAction("Copy");
        auto* actDup  = menu.addAction("Duplicate");
        connect(actCut, &QAction::triggered, this, [this]() {
            m_model->clipboard.clear();
            std::vector<int> rows;
            for (const auto& idx : selectionModel()->selectedRows()) {
                rows.push_back(idx.row());
                const auto* cd = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), idx.row());
                if (cd) m_model->clipboard.push_back(*cd);
            }
            if (!rows.empty()) {
                if (!m_model->sf.cueLists.empty()) {
                    const int listId = m_model->sf.cueLists[
                        static_cast<size_t>(m_model->activeListIdx())].numericId;
                    auto sortedRows = rows;
                    std::sort(sortedRows.begin(), sortedRows.end());
                    for (auto& cb : m_model->clipboard) {
                        if (cb.targetListId != listId) continue;
                        bool removed = false;
                        int shift = 0;
                        for (int r : sortedRows) {
                            if (r == cb.target) { removed = true; break; }
                            if (r < cb.target) ++shift;
                        }
                        if (removed) cb.target = -1;
                        else         cb.target -= shift;
                    }
                }
                deleteRows(rows);
            }
        });
        connect(actCopy, &QAction::triggered, this, [this]() {
            m_model->clipboard.clear();
            for (const auto& idx : selectionModel()->selectedRows()) {
                const auto* cd = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), idx.row());
                if (cd) m_model->clipboard.push_back(*cd);
            }
        });
        connect(actDup, &QAction::triggered, this, [this, row]() {
            const auto* cd = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), row);
            if (!cd) return;
            m_model->pushUndo();
            auto copy = *cd;
            copy.cueNumber = ShowHelpers::nextCueNumber(m_model->sf);
            std::string err;
            ShowHelpers::insertEngineCue(*m_model, m_model->activeListIdx(), row + 1, std::move(copy), err);
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
            int ins = (row >= 0) ? row + 1 : m_model->cues().cueCount();
            std::string err;
            for (auto cd : m_model->clipboard) {
                cd.cueNumber = ShowHelpers::nextCueNumber(m_model->sf);
                ShowHelpers::insertEngineCue(*m_model, m_model->activeListIdx(), ins, std::move(cd), err);
                ++ins;
            }
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
    // Insert after the selected cue's entire subtree (skip Group children).
    int insertAt = (m_selRow >= 0) ? m_selRow + 1 : rowCount();
    if (m_selRow >= 0) {
        const auto* sc = m_model->cues().cueAt(m_selRow);
        if (sc && sc->type == mcp::CueType::Group)
            insertAt = m_selRow + 1 + sc->childCount;
    }
    // Auto-target: if exactly one cue is selected, pre-assign it as target for
    // cue types that reference a target.
    const bool singleSel = (m_selRow >= 0 && selectionModel()->selectedRows().size() == 1);
    const int  autoTarget = singleSel ? m_selRow : -1;
    auto* addMenu = menu.addMenu("Add Cue");
    // Group: Group
    for (const char* type : {"Group"}) {
        auto* act = addMenu->addAction(type);
        connect(act, &QAction::triggered, this, [this, type, insertAt, autoTarget]() {
            addCueOfType(type, insertAt, autoTarget);
        });
    }
    addMenu->addSeparator();
    // Media / context
    for (const char* type : {"Audio", "MC"}) {
        auto* act = addMenu->addAction(type);
        connect(act, &QAction::triggered, this, [this, type, insertAt, autoTarget]() {
            addCueOfType(type, insertAt, autoTarget);
        });
    }
    addMenu->addSeparator();
    // Mix control
    for (const char* type : {"Fade"}) {
        auto* act = addMenu->addAction(type);
        connect(act, &QAction::triggered, this, [this, type, insertAt, autoTarget]() {
            addCueOfType(type, insertAt, autoTarget);
        });
    }
    addMenu->addSeparator();
    // Transport control
    for (const char* type : {"Start", "Stop", "Goto", "Arm", "Devamp", "Marker", "Memo", "Scriptlet"}) {
        auto* act = addMenu->addAction(type);
        connect(act, &QAction::triggered, this, [this, type, insertAt, autoTarget]() {
            addCueOfType(type, insertAt, autoTarget);
        });
    }
    addMenu->addSeparator();
    // Snapshot & Automation
    for (const char* type : {"Snapshot", "Automation"}) {
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
        m_model->cues().setCueName(row, name.toStdString());
        ShowHelpers::syncSfFromCues(*m_model);
    } else if (col == ColDuration) {
        const double secs = parseDuration(txt);
        if (secs >= 0.0) {
            m_model->pushUndo();
            m_model->cues().setCueDuration(row, secs);
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

void CueTableView::replaceAudioForRow(int row, const QString& absPath) {
    m_model->pushUndo();

    namespace fs = std::filesystem;
    const fs::path base(m_model->baseDir);
    fs::path np(absPath.toStdString());
    std::string pathToStore;
    if (!m_model->baseDir.empty()) {
        try {
            const auto rel = fs::relative(np, base);
            const std::string relStr = rel.string();
            pathToStore = (relStr.find("..") == std::string::npos) ? relStr : np.string();
        } catch (...) {
            pathToStore = np.string();
        }
    } else {
        pathToStore = np.string();
    }

    auto* sfCue = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), row);
    if (!sfCue) return;
    sfCue->path = pathToStore;

    ShowHelpers::reloadEngineCueAudio(*m_model, m_model->activeListIdx(), row);
    m_model->dirty = true;

    m_selRow = row;
    refresh();
    emit rowSelected(row);   // so inspector reloads the updated path
    emit cueListModified();
}

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

    std::string err;
    ShowHelpers::insertEngineCue(*m_model, m_model->activeListIdx(), beforeRow, std::move(cd), err);

    // Explicitly initialize diagonal crosspoint for the new audio cue so the
    // inspector grid always shows "0.0" on first display — no lazy defaults.
    if (const mcp::Cue* c = m_model->cues().cueAt(beforeRow)) {
        const int srcCh = c->audioFile.isLoaded() ? c->audioFile.metadata().channels : 1;
        const int outCh = m_model->channelCount();
        m_model->cues().initCueRouting(beforeRow, srcCh, outCh);
    }

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
    // Always use the absolute list ID — never -1 — so cut+paste across lists works.
    cd.targetListId = m_model->sf.cueLists[static_cast<size_t>(m_model->activeListIdx())].numericId;

    // Auto-assign target for cue types that reference another cue.
    if (autoTarget >= 0) {
        const std::string& t = cd.type;
        if (t == "fade" || t == "start" || t == "stop" || t == "arm" || t == "devamp" || t == "goto") {
            cd.target = autoTarget;
            // Resolve the cue number so the target survives index shifts and cross-list paste.
            if (const auto* sfTgt = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), autoTarget))
                cd.targetCueNumber = sfTgt->cueNumber;
        }
    }

    // Default group mode for new group cues.
    if (cd.type == "group") cd.groupMode = "timeline";

    std::string err;
    ShowHelpers::insertEngineCue(*m_model, m_model->activeListIdx(), beforeRow, std::move(cd), err);
    m_selRow = beforeRow;
    m_model->cues().setSelectedIndex(beforeRow);
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
        ShowHelpers::removeEngineCue(*m_model, m_model->activeListIdx(), r);

    const int newCount = m_model->cues().cueCount();
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
        children.insert(children.begin(), ShowHelpers::sfRemoveAt(m_model->sf, m_model->activeListIdx(), rows[i]));

    // Build a group CueData containing the removed cues as children.
    mcp::ShowFile::CueData groupCd;
    groupCd.type      = "group";
    groupCd.groupMode = "sync";
    groupCd.cueNumber = ShowHelpers::nextCueNumber(m_model->sf);
    groupCd.name      = "Group";
    groupCd.children  = std::move(children);

    // Insert the group at the position where the first selected cue was.
    ShowHelpers::sfInsertBefore(m_model->sf, m_model->activeListIdx(), rows.front(), std::move(groupCd));

    std::string err;
    ShowHelpers::rebuildCueList(*m_model, m_model->activeListIdx(), err);
    m_selRow = rows.front();
    emit rowSelected(m_selRow);
    emit cueListModified();
    refresh();
}
