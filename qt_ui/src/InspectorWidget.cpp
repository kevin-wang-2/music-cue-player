#include "InspectorWidget.h"
#include "AppModel.h"
#include "AutomationView.h"
#include "MidiInputManager.h"
#include "FaderWidget.h"
#include "MCImport.h"
#include "MusicContextView.h"
#include "PythonEditor.h"
#include "ScriptEditorWidget.h"
#include "ShowHelpers.h"
#include "SyncGroupView.h"
#include "TimelineGroupView.h"
#include "WaveformView.h"

#include "engine/CueList.h"
#include "engine/MusicContext.h"
#include "engine/plugin/ParameterInfo.h"

#include "engine/Cue.h"
#include "engine/FadeData.h"
#include "engine/Timecode.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QKeyEvent>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFrame>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

// ── AudioPathWidget ────────────────────────────────────────────────────────
// Clickable, droppable label that shows an audio file path.
// Uses std::function callbacks to avoid Q_OBJECT on a local class.
class AudioPathWidget : public QFrame {
public:
    std::function<void(const QString&)> onPathChanged;

    explicit AudioPathWidget(QWidget* parent = nullptr) : QFrame(parent) {
        setAcceptDrops(true);
        setCursor(Qt::PointingHandCursor);
        setFrameShape(QFrame::StyledPanel);
        setFrameShadow(QFrame::Sunken);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumHeight(26);
        setStyleSheet(
            "AudioPathWidget { background:#0e0e0e; border:1px solid #333; border-radius:3px; }"
            "AudioPathWidget[hover=true] { border-color:#5a8ad0; background:#141a22; }"
        );

        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(6, 2, 6, 2);
        m_label = new QLabel(this);
        m_label->setStyleSheet("color:#bbb; background:transparent;");
        m_label->setTextInteractionFlags(Qt::NoTextInteraction);
        lay->addWidget(m_label, 1);

        auto* icon = new QLabel("📂", this);
        icon->setStyleSheet("color:#666; background:transparent; font-size:11px;");
        lay->addWidget(icon);
    }

    void setPath(const QString& absPath) {
        m_path = absPath;
        const QString fname = QString::fromStdString(
            std::filesystem::path(absPath.toStdString()).filename().string());
        m_label->setText(fname.isEmpty() ? "(no file)" : fname);
        setToolTip(absPath);
    }

    const QString& currentPath() const { return m_path; }

protected:
    void mousePressEvent(QMouseEvent* ev) override {
        if (ev->button() != Qt::LeftButton) return;
        const QString p = QFileDialog::getOpenFileName(
            this, "Replace Audio File",
            m_path.isEmpty() ? QString() : QString::fromStdString(
                std::filesystem::path(m_path.toStdString()).parent_path().string()),
            "Audio Files (*.wav *.aif *.aiff *.mp3 *.flac *.ogg *.m4a *.aac *.caf "
            "*.opus *.wv *.ape);;All Files (*)");
        if (!p.isEmpty() && onPathChanged) onPathChanged(p);
    }

    void enterEvent(QEnterEvent*) override {
        setProperty("hover", true);
        style()->unpolish(this); style()->polish(this);
    }
    void leaveEvent(QEvent*) override {
        setProperty("hover", false);
        style()->unpolish(this); style()->polish(this);
    }

    void dragEnterEvent(QDragEnterEvent* ev) override {
        if (ev->mimeData()->hasUrls()) {
            ev->acceptProposedAction();
            setProperty("hover", true);
            style()->unpolish(this); style()->polish(this);
        }
    }
    void dragLeaveEvent(QDragLeaveEvent*) override {
        setProperty("hover", false);
        style()->unpolish(this); style()->polish(this);
    }
    void dropEvent(QDropEvent* ev) override {
        setProperty("hover", false);
        style()->unpolish(this); style()->polish(this);
        static const QStringList kAudioExts = {
            ".wav",".aif",".aiff",".mp3",".flac",".ogg",".m4a",".aac",".caf",".opus",".wv",".ape"
        };
        for (const QUrl& url : ev->mimeData()->urls()) {
            if (!url.isLocalFile()) continue;
            const QString p = url.toLocalFile();
            const QString lp = p.toLower();
            for (const auto& ext : kAudioExts) {
                if (lp.endsWith(ext)) {
                    if (onPathChanged) onPathChanged(p);
                    ev->acceptProposedAction();
                    return;
                }
            }
        }
    }

private:
    QLabel*  m_label{nullptr};
    QString  m_path;
};

// ── Automation parameter path helpers ─────────────────────────────────────

struct AutoPathItem {
    QString label;
    QString data;      // "fader", "0", "/mixer/1/polarity", etc.
    bool    terminal;  // no further levels below
};

// Returns selectable items for one level of the path hierarchy.
static QList<AutoPathItem> pathItemsAtLevel(AppModel* model, const QString& prefix)
{

    const auto& as = model->sf.audioSetup;
    const int   nCh = static_cast<int>(as.channels.size());

    auto chLabel = [&](int idx) -> QString {
        if (idx >= 0 && idx < nCh && !as.channels[static_cast<size_t>(idx)].name.empty())
            return QString::fromStdString(as.channels[static_cast<size_t>(idx)].name);
        return QStringLiteral("Ch %1").arg(idx + 1);
    };

    QList<AutoPathItem> items;
    if (prefix.isEmpty()) {
        items.append({"Mixer", "mixer", false});
    } else if (prefix == QLatin1String("mixer")) {
        for (int ch = 0; ch < nCh; ++ch) {
            if (ch > 0 && as.channels[static_cast<size_t>(ch - 1)].linkedStereo) continue;
            items.append({chLabel(ch), QString::number(ch), false});
        }
    } else if (prefix.startsWith(QLatin1String("mixer/"))) {
        const QStringList parts = prefix.split('/');
        if (parts.size() == 2) {
            items.append({"Fader",      "fader",      true});
            items.append({"Mute",       "mute",       true});
            items.append({"Polarity",   "polarity",   true});
            items.append({"Crosspoint", "crosspoint", false});
            bool ok;
            const int mCh = parts[1].toInt(&ok);
            if (ok && mCh >= 0 && mCh < nCh) {
                const auto& chSends = as.channels[static_cast<size_t>(mCh)].sends;
                const bool hasActiveSend = std::any_of(chSends.begin(), chSends.end(),
                    [](const auto& s){ return s.isActive(); });
                if (hasActiveSend)
                    items.append({"Send", "send", false});
            }
            if (ok && mCh >= 0 && mCh < nCh - 1
                && as.channels[static_cast<size_t>(mCh)].linkedStereo) {
                const int sl = mCh + 1;
                items.append({chLabel(sl) + " Polarity",
                               QStringLiteral("/mixer/%1/polarity").arg(sl), true});
                items.append({chLabel(sl) + " Crosspoint",
                               QStringLiteral("/mixer/%1/crosspoint").arg(sl), false});
            }
        } else if (parts.size() == 3 && parts[2] == QLatin1String("send")) {
            bool ok; const int mCh = parts[1].toInt(&ok);
            if (ok && mCh >= 0 && mCh < nCh) {
                const auto& chSends = as.channels[static_cast<size_t>(mCh)].sends;
                for (int s = 0; s < static_cast<int>(chSends.size()); ++s) {
                    if (!chSends[static_cast<size_t>(s)].isActive()) continue;
                    const int dstCh = chSends[static_cast<size_t>(s)].dstChannel;
                    const QString dstLabel = (dstCh >= 0 && dstCh < nCh)
                        ? QStringLiteral("→ %1").arg(chLabel(dstCh))
                        : QStringLiteral("Send %1").arg(s + 1);
                    items.append({dstLabel, QString::number(s), false});
                }
            }
        } else if (parts.size() == 4 && parts[2] == QLatin1String("send")) {
            items.append({"Level", "level", true});
            items.append({"Mute",  "mute",  true});
            bool ok; bool ok2;
            const int mCh   = parts[1].toInt(&ok);
            const int mSlot = parts[3].toInt(&ok2);
            if (ok && ok2 && mCh >= 0 && mCh < nCh
                && mSlot >= 0 && mSlot < static_cast<int>(as.channels[static_cast<size_t>(mCh)].sends.size())) {
                const auto& ss = as.channels[static_cast<size_t>(mCh)].sends[static_cast<size_t>(mSlot)];
                const bool srcStereo = as.channels[static_cast<size_t>(mCh)].linkedStereo && (mCh + 1 < nCh);
                const int dst = ss.dstChannel;
                const bool dstStereo = (dst >= 0 && dst < nCh)
                    && as.channels[static_cast<size_t>(dst)].linkedStereo && (dst + 1 < nCh);
                if (!srcStereo && dstStereo)
                    items.append({"Pan", "panL", true});
                else if (srcStereo && dstStereo) {
                    items.append({"Pan L", "panL", true});
                    items.append({"Pan R", "panR", true});
                }
            }
        } else if (parts.size() == 3 && parts[2] == QLatin1String("crosspoint")) {
            for (int out = 0; out < nCh; ++out) {
                if (out > 0 && as.channels[static_cast<size_t>(out - 1)].linkedStereo) continue;
                items.append({QStringLiteral("→ %1").arg(chLabel(out)), QString::number(out), true});
            }
        } else if (parts.size() == 3 && parts[2] == QLatin1String("plugin")) {
            bool ok;
            const int mCh = parts[1].toInt(&ok);
            if (ok && mCh >= 0 && mCh < nCh) {
                const auto& pSlots = as.channels[static_cast<size_t>(mCh)].plugins;
                for (int s = 0; s < static_cast<int>(pSlots.size()); ++s) {
                    if (pSlots[static_cast<size_t>(s)].pluginId.empty()) continue;
                    items.append({QStringLiteral("Slot %1").arg(s + 1), QString::number(s), false});
                }
            }
        } else if (parts.size() == 4 && parts[2] == QLatin1String("plugin")) {
            bool ok; bool ok2;
            const int mCh   = parts[1].toInt(&ok);
            const int mSlot = parts[3].toInt(&ok2);
            if (ok && ok2 && mCh >= 0 && mCh < nCh) {
                const auto& pSlots = as.channels[static_cast<size_t>(mCh)].plugins;
                if (mSlot >= 0 && mSlot < static_cast<int>(pSlots.size())) {
                    auto proc = model->channelPlugin(mCh, mSlot);
                    if (proc && proc->getProcessor()) {
                        for (const auto& info : proc->getProcessor()->getParameters())
                            items.append({QString::fromStdString(info.name),
                                          QString::fromStdString(info.id), true});
                    }
                }
            }
        }
        if (parts.size() == 2) {
            // At channel level — also offer Plugin branch if any plugins loaded.
            bool ok; const int mCh = parts[1].toInt(&ok);
            if (ok && mCh >= 0 && mCh < nCh
                && !as.channels[static_cast<size_t>(mCh)].plugins.empty()) {
                items.append({"Plugin", "plugin", false});
            }
        }
    }
    return items;
}

// Human-readable label for one path segment given its data, level index, and parent segment.
static QString pathSegLabel(const mcp::ShowFile::AudioSetup& as,
                            const QString& data, int level,
                            const QString& parent = QString())
{
    const int nCh = static_cast<int>(as.channels.size());
    auto chLabel = [&](int idx) -> QString {
        if (idx >= 0 && idx < nCh && !as.channels[static_cast<size_t>(idx)].name.empty())
            return QString::fromStdString(as.channels[static_cast<size_t>(idx)].name);
        return QStringLiteral("Ch %1").arg(idx + 1);
    };
    if (data.startsWith('/')) {
        const QStringList p = data.mid(1).split('/');
        if (p.size() >= 3 && p[0] == QLatin1String("mixer")) {
            bool ok; const int ch = p[1].toInt(&ok);
            if (ok) {
                if (p[2] == QLatin1String("polarity"))   return chLabel(ch) + " Polarity";
                if (p[2] == QLatin1String("crosspoint")) return chLabel(ch) + " Crosspoint";
            }
        }
        return data;
    }
    switch (level) {
    case 0: return "Mixer";
    case 1: { bool ok; int ch = data.toInt(&ok); return ok ? chLabel(ch) : data; }
    case 2:
        if (data == QLatin1String("fader"))      return "Fader";
        if (data == QLatin1String("mute"))       return "Mute";
        if (data == QLatin1String("polarity"))   return "Polarity";
        if (data == QLatin1String("crosspoint")) return "Crosspoint";
        if (data == QLatin1String("plugin"))     return "Plugin";
        if (data == QLatin1String("send"))       return "Send";
        return data;
    case 3: {
        bool ok; int idx = data.toInt(&ok);
        if (!ok) return data;
        if (parent == QLatin1String("plugin"))
            return QStringLiteral("Slot %1").arg(idx + 1);
        if (parent == QLatin1String("send"))
            return QStringLiteral("Send %1").arg(idx + 1);
        return QStringLiteral("→ %1").arg(chLabel(idx));
    }
    case 4:
        if (data == QLatin1String("level")) return "Level";
        if (data == QLatin1String("mute"))  return "Mute";
        if (data == QLatin1String("panL"))  return "Pan L";
        if (data == QLatin1String("panR"))  return "Pan R";
        return data;
    default: return data;
    }
}

// ── AutoParamPickerDialog ──────────────────────────────────────────────────
// Modal dialog for browsing/searching automation parameters.
// Does not use Q_OBJECT; all connections via lambdas.
class AutoParamPickerDialog : public QDialog {
public:
    AutoParamPickerDialog(AppModel* model, const QString& currentPath, QWidget* parent = nullptr)
        : QDialog(parent), m_model(model), m_selected(currentPath)
    {
        setWindowTitle("Add Parameter");
        setMinimumWidth(320);
        setMinimumHeight(420);

        auto* vlay = new QVBoxLayout(this);
        vlay->setSpacing(8);

        auto* filterRow = new QHBoxLayout;
        filterRow->addWidget(new QLabel("Filter:"));
        m_filter = new QLineEdit;
        filterRow->addWidget(m_filter, 1);
        vlay->addLayout(filterRow);

        m_tree = new QTreeWidget;
        m_tree->setHeaderHidden(true);
        m_tree->setRootIsDecorated(true);
        m_tree->setUniformRowHeights(true);
        m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
        vlay->addWidget(m_tree, 1);

        auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        vlay->addWidget(btnBox);

        buildTree(currentPath);

        connect(btnBox, &QDialogButtonBox::accepted, this, [this]() {
            auto* item = m_tree->currentItem();
            if (item) {
                const QString path = item->data(0, Qt::UserRole).toString();
                if (!path.isEmpty()) { m_selected = path; accept(); }
            }
        });
        connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_tree, &QTreeWidget::itemDoubleClicked,
                this, [this](QTreeWidgetItem* item, int) {
            const QString path = item->data(0, Qt::UserRole).toString();
            if (!path.isEmpty()) { m_selected = path; accept(); }
        });
        connect(m_filter, &QLineEdit::textChanged, this, [this](const QString& text) {
            applyFilter(text);
        });
    }

    QString selectedPath() const { return m_selected; }

private:
    void addChildren(QTreeWidgetItem* parent, const QString& prefix, int depth = 0) {
        if (depth > 6) return;
        const auto items = pathItemsAtLevel(m_model, prefix);
        for (const auto& item : items) {
            const QString childPrefix = item.data.startsWith('/')
                ? item.data.mid(1)
                : (prefix.isEmpty() ? item.data : prefix + "/" + item.data);
            auto* treeItem = new QTreeWidgetItem(parent, {item.label});
            if (item.terminal) {
                treeItem->setData(0, Qt::UserRole, "/" + childPrefix);
                treeItem->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
            } else {
                treeItem->setData(0, Qt::UserRole, QString());
                addChildren(treeItem, childPrefix, depth + 1);
            }
        }
    }

    void buildTree(const QString& currentPath) {
        m_tree->clear();
        for (const auto& root : pathItemsAtLevel(m_model, "")) {
            auto* rootItem = new QTreeWidgetItem(m_tree, {root.label});
            rootItem->setData(0, Qt::UserRole, root.terminal ? "/" + root.data : QString());
            if (!root.terminal) addChildren(rootItem, root.data, 1);
        }
        m_tree->expandAll();
        if (!currentPath.isEmpty()) {
            QTreeWidgetItemIterator it(m_tree);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole).toString() == currentPath) {
                    m_tree->setCurrentItem(*it);
                    m_tree->scrollToItem(*it);
                    break;
                }
                ++it;
            }
        }
    }

    bool filterItem(QTreeWidgetItem* item, const QString& text) {
        const bool isLeaf    = !item->data(0, Qt::UserRole).toString().isEmpty();
        const bool selfMatch = isLeaf && item->text(0).contains(text, Qt::CaseInsensitive);
        bool childMatch = false;
        for (int i = 0; i < item->childCount(); ++i)
            childMatch |= filterItem(item->child(i), text);
        const bool show = selfMatch || childMatch;
        item->setHidden(!show);
        if (childMatch) item->setExpanded(true);
        return show;
    }

    void applyFilter(const QString& text) {
        if (text.isEmpty()) {
            QTreeWidgetItemIterator it(m_tree);
            while (*it) { (*it)->setHidden(false); ++it; }
            m_tree->expandAll();
            return;
        }
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
            filterItem(m_tree->topLevelItem(i), text);
    }

    AppModel*    m_model;
    QLineEdit*   m_filter{nullptr};
    QTreeWidget* m_tree{nullptr};
    QString      m_selected;
};

// ── style helpers ──────────────────────────────────────────────────────────

static const char* kXpCellStyle =
    "QLineEdit { background:#1a1a1a; color:#dddddd; border:1px solid #383838;"
    "  border-radius:2px; padding:1px 3px; font-size:11px; }"
    "QLineEdit:focus { border-color:#2a6ab8; }"
    "QLineEdit[disabled='true'] { background:#111; color:#444; }";

static QLineEdit* makeXpCell() {
    auto* e = new QLineEdit;
    e->setFixedSize(46, 22);
    e->setAlignment(Qt::AlignHCenter);
    e->setStyleSheet(kXpCellStyle);
    return e;
}

// Format a dB value for display in a crosspoint cell.
// Values at or below kFaderMin (-60) are stored as kFaderInf (-144) and shown as "-inf".
static QString fmtXpDb(float dB) {
    if (dB <= FaderWidget::kFaderMin + 0.05f) return QStringLiteral("-inf");
    return QString::number(static_cast<double>(dB), 'f', 1);
}

// ── ctor ───────────────────────────────────────────────────────────────────

InspectorWidget::InspectorWidget(AppModel* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_tabs = new QTabWidget(this);
    lay->addWidget(m_tabs);

    buildBasicTab();
    buildMarkerTab();
    buildMCTab();
    buildTriggersTab();
    buildLevelsTab();
    buildTrimTab();
    buildCurveTab();
    buildModeTab();
    buildTimeTab();
    buildTimelineTab();
    buildScriptTab();
    buildSnapshotTab();
    buildAutomationTab();
    buildNetworkTab();
    buildMidiTab();
    buildTimecodeTab();
}

// ── tab builders ───────────────────────────────────────────────────────────

void InspectorWidget::buildBasicTab() {
    m_basicPage = new QWidget;
    auto* form = new QFormLayout(m_basicPage);
    form->setContentsMargins(8, 8, 8, 8);

    m_editNum  = new QLineEdit;
    m_editName = new QLineEdit;
    m_spinPreWait = new QDoubleSpinBox;
    m_spinPreWait->setRange(0.0, 9999.0);
    m_spinPreWait->setDecimals(3);
    m_spinPreWait->setSuffix(" s");
    m_comboGoQuantize = new QComboBox;
    m_comboGoQuantize->addItems({"None", "Next bar", "Next beat"});

    m_chkAutoCont   = new QCheckBox("Auto-continue");
    m_chkAutoFollow = new QCheckBox("Auto-follow");

    m_spinDurationBasic = new QDoubleSpinBox;
    m_spinDurationBasic->setRange(0.0, 99999.0);
    m_spinDurationBasic->setDecimals(3);
    m_spinDurationBasic->setSuffix(" s");
    m_spinDurationBasic->setSpecialValueText("(to end)");

    // Audio file path row — only visible for Audio cues
    m_audioPathWidget = new AudioPathWidget;
    static_cast<AudioPathWidget*>(m_audioPathWidget)->onPathChanged =
        [this](const QString& p) { replaceAudioFile(p); };
    // Wrap in a QWidget so we can hide the whole form row cleanly
    m_audioFileRow = new QWidget;
    {
        auto* lay = new QHBoxLayout(m_audioFileRow);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(m_audioPathWidget);
    }
    form->addRow("File:", m_audioFileRow);

    form->addRow("Cue #:",    m_editNum);
    form->addRow("Name:",     m_editName);
    form->addRow("Pre-wait:", m_spinPreWait);
    form->addRow("Duration:", m_spinDurationBasic);
    form->addRow("Quantize:", m_comboGoQuantize);

    auto* flagRow = new QHBoxLayout;
    flagRow->addWidget(m_chkAutoCont);
    flagRow->addWidget(m_chkAutoFollow);
    flagRow->addStretch();
    form->addRow("", flagRow);

    // Devamp section
    m_devampGroup = new QGroupBox("Devamp options");
    auto* dvLay = new QFormLayout(m_devampGroup);
    m_comboDevampMode = new QComboBox;
    m_comboDevampMode->addItems({"Next slice", "Next cue (stop current)", "Next cue (keep current)"});
    m_chkDevampPreVamp = new QCheckBox("Skip pre-vamp loops");
    dvLay->addRow("Mode:", m_comboDevampMode);
    dvLay->addRow("", m_chkDevampPreVamp);
    form->addRow(m_devampGroup);

    // Arm section
    m_armGroup = new QGroupBox("Arm options");
    auto* armLay = new QFormLayout(m_armGroup);
    m_spinArmStart = new QDoubleSpinBox;
    m_spinArmStart->setRange(0.0, 99999.0);
    m_spinArmStart->setDecimals(3);
    m_spinArmStart->setSuffix(" s");
    armLay->addRow("Pre-load from:", m_spinArmStart);
    form->addRow(m_armGroup);

    m_tabs->addTab(m_basicPage, "Basic");

    connect(m_editNum,  &QLineEdit::editingFinished, this, &InspectorWidget::onBasicChanged);
    connect(m_editName, &QLineEdit::editingFinished, this, &InspectorWidget::onBasicChanged);
    connect(m_spinPreWait, &QDoubleSpinBox::editingFinished,
            this, &InspectorWidget::onBasicChanged);
    connect(m_comboGoQuantize, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &InspectorWidget::onBasicChanged);
    connect(m_chkAutoCont,   &QCheckBox::toggled, this, &InspectorWidget::onBasicChanged);
    connect(m_chkAutoFollow, &QCheckBox::toggled, this, &InspectorWidget::onBasicChanged);
    connect(m_comboDevampMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &InspectorWidget::onBasicChanged);
    connect(m_chkDevampPreVamp, &QCheckBox::toggled,
            this, &InspectorWidget::onBasicChanged);
    connect(m_spinArmStart, &QDoubleSpinBox::editingFinished,
            this, &InspectorWidget::onBasicChanged);

    connect(m_spinDurationBasic, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        m_model->cues().setCueDuration(m_cueIdx, m_spinDurationBasic->value());
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
}

void InspectorWidget::buildLevelsTab() {
    m_levelsPage = new QWidget;
    auto* outerLay = new QVBoxLayout(m_levelsPage);
    outerLay->setContentsMargins(0, 0, 0, 0);

    m_levelsScroll = new QScrollArea;
    m_levelsScroll->setWidgetResizable(true);
    m_levelsScroll->setFrameShape(QFrame::NoFrame);
    outerLay->addWidget(m_levelsScroll);

    m_levelsContent = new QWidget;
    m_levelsScroll->setWidget(m_levelsContent);

    m_tabs->addTab(m_levelsPage, "Levels");
}

void InspectorWidget::buildTrimTab() {
    m_trimPage = new QWidget;
    auto* lay = new QHBoxLayout(m_trimPage);
    lay->setContentsMargins(8, 8, 8, 8);

    m_trimFader = new FaderWidget("Trim", m_trimPage);
    lay->addWidget(m_trimFader);
    lay->addStretch();

    connect(m_trimFader, &FaderWidget::dragStarted, this, [this]() {
        if (!m_loading && m_cueIdx >= 0) m_model->pushUndo();
    });
    connect(m_trimFader, &FaderWidget::valueChanged,
            this, &InspectorWidget::onTrimFaderChanged);

    m_tabs->addTab(m_trimPage, "Trim");
}

void InspectorWidget::buildTimeTab() {
    m_timePage = new QWidget;
    auto* pageLay = new QVBoxLayout(m_timePage);
    pageLay->setContentsMargins(0, 0, 0, 0);
    pageLay->setSpacing(0);

    // Wrap everything in a scroll area so short panels scroll instead of squishing
    auto* scroll = new QScrollArea(m_timePage);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pageLay->addWidget(scroll);

    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* lay = new QVBoxLayout(content);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    // ── Audio-only section (start/duration/waveform) ────────────────────────
    m_audioTimeSection = new QWidget(content);
    auto* audioLay = new QVBoxLayout(m_audioTimeSection);
    audioLay->setContentsMargins(0, 0, 0, 0);
    audioLay->setSpacing(6);

    auto* form = new QFormLayout;
    form->setSpacing(4);

    m_spinStart = new QDoubleSpinBox;
    m_spinStart->setRange(0.0, 99999.0);
    m_spinStart->setDecimals(3);
    m_spinStart->setSuffix(" s");

    m_spinDuration = new QDoubleSpinBox;
    m_spinDuration->setRange(0.0, 99999.0);
    m_spinDuration->setDecimals(3);
    m_spinDuration->setSuffix(" s");
    m_spinDuration->setSpecialValueText("(to end)");

    form->addRow("Start:", m_spinStart);
    form->addRow("Duration:", m_spinDuration);
    audioLay->addLayout(form);

    m_waveform = new WaveformView(m_model, m_audioTimeSection);
    audioLay->addWidget(m_waveform, 1);

    lay->addWidget(m_audioTimeSection, 1);

    // ── SyncGroup visual editor ──────────────────────────────────────────────
    m_syncGroupView = new SyncGroupView(m_model, content);
    m_syncGroupView->hide();
    lay->addWidget(m_syncGroupView, 1);

    // ── Marker editor panel (shared by audio and SyncGroup) ──────────────────
    m_markerPanel = new QGroupBox("Marker", content);
    auto* mform = new QFormLayout(m_markerPanel);
    mform->setSpacing(4);
    m_markerLabel = new QLabel(m_markerPanel);
    m_markerLabel->setStyleSheet("color:#aaa; font-size:11px;");

    m_markerTimeSpin = new QDoubleSpinBox(m_markerPanel);
    m_markerTimeSpin->setRange(0.0, 99999.0);
    m_markerTimeSpin->setDecimals(3);
    m_markerTimeSpin->setSuffix(" s");

    m_markerNameEdit = new QLineEdit(m_markerPanel);
    m_markerNameEdit->setPlaceholderText("(no name)");

    m_comboMarkerAnchor = new QComboBox(m_markerPanel);

    mform->addRow(m_markerLabel);
    mform->addRow("Time:", m_markerTimeSpin);
    mform->addRow("Name:", m_markerNameEdit);
    mform->addRow("Anchor:", m_comboMarkerAnchor);
    m_markerPanel->hide();
    lay->addWidget(m_markerPanel);

    // SyncGroupView signals
    connect(m_syncGroupView, &SyncGroupView::markerSelected, this, [this](int mi) {
        if (mi < 0 || m_cueIdx < 0) {
            m_selMarker = -1;
            m_markerPanel->hide();
            return;
        }
        const mcp::Cue* c = m_model->cues().cueAt(m_cueIdx);
        if (!c || mi >= (int)c->markers.size()) { m_markerPanel->hide(); return; }
        m_selMarker = mi;
        m_loading = true;
        m_markerLabel->setText(QString("Marker %1").arg(mi + 1));
        m_markerTimeSpin->setValue(c->markers[mi].time);
        m_markerNameEdit->setText(QString::fromStdString(c->markers[mi].name));
        m_loading = false;
        refreshMarkerAnchorCombo();
        m_markerPanel->show();
    });
    connect(m_syncGroupView, &SyncGroupView::cueModified, this, [this]() {
        emit cueEdited();
    });
    connect(m_syncGroupView, &SyncGroupView::rulerClicked, this, [this](double timeSec) {
        if (m_cueIdx < 0) return;
        m_model->cues().setCueTimelineArmSec(m_cueIdx, timeSec);
        emit cueEdited();
    });

    m_tabs->addTab(m_timePage, "Time & Loop");

    // Start / duration edits
    connect(m_spinStart, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        m_model->cues().setCueStartTime(m_cueIdx, m_spinStart->value());
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
    connect(m_spinDuration, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        m_model->cues().setCueDuration(m_cueIdx, m_spinDuration->value());
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });

    // Arm click on waveform body: just visual cursor, does NOT change startTime
    // (armPositionChanged is intentionally NOT connected to setCueStartTime)

    // Marker selection from waveform
    connect(m_waveform, &WaveformView::markerSelectionChanged, this, [this](int mi) {
        m_selMarker = mi;
        if (mi < 0 || m_cueIdx < 0) {
            m_markerPanel->hide();
            return;
        }
        const mcp::Cue* c = m_model->cues().cueAt(m_cueIdx);
        if (!c || mi >= (int)c->markers.size()) { m_markerPanel->hide(); return; }

        m_loading = true;
        m_markerLabel->setText(QString("Marker %1").arg(mi + 1));
        m_markerTimeSpin->setValue(c->markers[mi].time);
        m_markerNameEdit->setText(QString::fromStdString(c->markers[mi].name));
        m_loading = false;
        refreshMarkerAnchorCombo();
        m_markerPanel->show();
    });

    // Marker time edit
    connect(m_markerTimeSpin, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (m_loading || m_cueIdx < 0 || m_selMarker < 0) return;
        m_model->pushUndo();
        m_model->cues().setCueMarkerTime(m_cueIdx, m_selMarker, m_markerTimeSpin->value());
        ShowHelpers::syncSfFromCues(*m_model);
        if (m_syncGroupView->isVisible()) m_syncGroupView->update();
        emit cueEdited();
    });

    // Marker name edit
    connect(m_markerNameEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_loading || m_cueIdx < 0 || m_selMarker < 0) return;
        m_model->pushUndo();
        m_model->cues().setCueMarkerName(m_cueIdx, m_selMarker,
                                        m_markerNameEdit->text().toStdString());
        ShowHelpers::syncSfFromCues(*m_model);
        if (m_syncGroupView->isVisible()) m_syncGroupView->update();
        emit cueEdited();
    });

    // Anchor Marker cue combo (data is QPoint(listIdx, cueIdxInList), or (-1,-1) for none)
    connect(m_comboMarkerAnchor, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (m_loading || m_cueIdx < 0 || m_selMarker < 0) return;
        m_model->pushUndo();
        const QPoint sel = m_comboMarkerAnchor->currentData().toPoint();
        const int cueIdxInList = sel.y();  // -1 for "(none)"
        m_model->cues().setMarkerAnchor(m_cueIdx, m_selMarker, cueIdxInList);
        ShowHelpers::syncSfFromCues(*m_model);
        // Persist anchorMarkerListId — syncSfFromCues doesn't preserve it
        if (auto* sfCue = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), m_cueIdx)) {
            if (m_selMarker < (int)sfCue->markers.size()) {
                const int listIdx = sel.x();
                sfCue->markers[m_selMarker].anchorMarkerListId =
                    (listIdx < 0 || listIdx == m_model->activeListIdx()) ? -1
                    : m_model->sf.cueLists[listIdx].numericId;
            }
        }
        emit cueEdited();
    });
}

void InspectorWidget::buildCurveTab() {
    m_curvePage = new QWidget;
    auto* form = new QFormLayout(m_curvePage);
    form->setContentsMargins(8, 8, 8, 8);

    m_comboCurve = new QComboBox;
    m_comboCurve->addItems({"Linear", "Equal power"});

    m_chkStopWhenDone = new QCheckBox("Stop target when done");

    form->addRow("Curve:", m_comboCurve);
    form->addRow("", m_chkStopWhenDone);

    m_tabs->addTab(m_curvePage, "Curve");

    connect(m_comboCurve, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        m_model->cues().setCueFadeCurve(m_cueIdx,
            idx == 1 ? mcp::FadeData::Curve::EqualPower : mcp::FadeData::Curve::Linear);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
    connect(m_chkStopWhenDone, &QCheckBox::toggled, this, [this](bool v) {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        m_model->cues().setCueFadeStopWhenDone(m_cueIdx, v);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
}

void InspectorWidget::buildModeTab() {
    m_modePage = new QWidget;
    auto* form = new QFormLayout(m_modePage);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);

    m_comboGroupMode = new QComboBox;
    m_comboGroupMode->addItem("Timeline",           static_cast<int>(mcp::GroupData::Mode::Timeline));
    m_comboGroupMode->addItem("Playlist",           static_cast<int>(mcp::GroupData::Mode::Playlist));
    m_comboGroupMode->addItem("Start First & Enter",static_cast<int>(mcp::GroupData::Mode::StartFirst));
    m_comboGroupMode->addItem("Synchronization",    static_cast<int>(mcp::GroupData::Mode::Sync));
    form->addRow("Mode:", m_comboGroupMode);

    m_chkGroupRandom = new QCheckBox("Random order (Playlist only)");
    form->addRow("", m_chkGroupRandom);

    m_tabs->addTab(m_modePage, "Mode");

    connect(m_comboGroupMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (m_loading || m_cueIdx < 0) return;
        const mcp::Cue* c = m_model->cues().cueAt(m_cueIdx);
        if (!c || !c->groupData) return;
        m_model->pushUndo();
        auto mode = static_cast<mcp::GroupData::Mode>(
            m_comboGroupMode->currentData().toInt());
        m_model->cues().setCueGroupMode(m_cueIdx, mode);
        ShowHelpers::syncSfFromCues(*m_model);
        // Show/hide Time and Timeline tabs based on new mode
        const bool isTimeline = (mode == mcp::GroupData::Mode::Timeline);
        const bool isSyncMode = (mode == mcp::GroupData::Mode::Sync);
        m_tabs->setTabVisible(m_tabs->indexOf(m_timePage),     isSyncMode);
        m_tabs->setTabVisible(m_tabs->indexOf(m_timelinePage), isTimeline);
        m_chkGroupRandom->setEnabled(mode == mcp::GroupData::Mode::Playlist);
        if (isSyncMode) loadTime();
        emit cueEdited();
    });

    connect(m_chkGroupRandom, &QCheckBox::toggled, this, [this](bool v) {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        m_model->cues().setCueGroupRandom(m_cueIdx, v);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
}

void InspectorWidget::buildMCTab() {
    m_mcPage = new QWidget;
    auto* outerLay = new QVBoxLayout(m_mcPage);
    outerLay->setContentsMargins(8, 6, 8, 6);
    outerLay->setSpacing(4);

    m_chkAttachMC = new QCheckBox("Attach Music Context");
    outerLay->addWidget(m_chkAttachMC);

    m_chkInheritParentMC = new QCheckBox("Inherit parent cue's Music Context");
    m_chkInheritParentMC->hide();
    outerLay->addWidget(m_chkInheritParentMC);

    m_mcContent = new QWidget;
    auto* contentLay = new QVBoxLayout(m_mcContent);
    contentLay->setContentsMargins(0, 4, 0, 0);
    contentLay->setSpacing(4);

    m_chkApplyBefore = new QCheckBox("Apply before cue start (extrapolate first point)");
    contentLay->addWidget(m_chkApplyBefore);

    m_mcView = new MusicContextView(m_model, m_mcContent);
    contentLay->addWidget(m_mcView);

    // Button row: Import and Inherit from child (wrapped in m_mcBtnRow so it can be hidden)
    {
        m_mcBtnRow = new QWidget(m_mcContent);
        auto* btnRow = new QHBoxLayout(m_mcBtnRow);
        btnRow->setContentsMargins(0, 0, 0, 0);
        btnRow->setSpacing(4);

        auto* btnImport = new QPushButton("Import...", m_mcBtnRow);
        btnRow->addWidget(btnImport);

        auto* btnInherit = new QPushButton("Inherit from child...", m_mcBtnRow);
        btnRow->addWidget(btnInherit);
        btnRow->addStretch();

        contentLay->addWidget(m_mcBtnRow);

        // Import button: open MIDI or SMT file
        connect(btnImport, &QPushButton::clicked, this, [this]() {
            if (m_cueIdx < 0) return;
            const QString path = QFileDialog::getOpenFileName(
                this, "Import Music Context",
                {},
                "MIDI files (*.mid *.midi);;Steinberg SMT (*.smt);;All files (*)");
            if (path.isEmpty()) return;

            auto* mc = m_model->cues().musicContextOf(m_cueIdx);
            if (!mc) {
                // Ensure MC is attached first
                auto newMc = std::make_unique<mcp::MusicContext>();
                mcp::MusicContext::Point p;
                p.bar=1; p.beat=1; p.bpm=120.0;
                p.isRamp=false; p.hasTimeSig=true; p.timeSigNum=4; p.timeSigDen=4;
                newMc->points.push_back(p);
                m_model->cues().setCueMusicContext(m_cueIdx, std::move(newMc));
                mc = m_model->cues().musicContextOf(m_cueIdx);
            }

            m_model->pushUndo();
            std::string err;
            const std::string ps = path.toStdString();
            if (path.endsWith(".smt", Qt::CaseInsensitive))
                err = MCImport::fromSmt(ps, *mc);
            else
                err = MCImport::fromMidi(ps, *mc);

            if (!err.empty()) {
                QMessageBox::warning(this, "Import Error",
                                     QString::fromStdString(err));
                return;
            }

            m_model->cues().markMCDirty(m_cueIdx);
            m_mcView->setCueIndex(m_cueIdx);
            m_mcView->update();
            ShowHelpers::syncSfFromCues(*m_model);
            emit cueEdited();
        });

        // Inherit From Child button: copy MC from a direct child with MC
        connect(btnInherit, &QPushButton::clicked, this, [this]() {
            if (m_cueIdx < 0) return;
            const mcp::Cue* c = m_model->cues().cueAt(m_cueIdx);
            if (!c || c->type != mcp::CueType::Group) return;

            // Collect direct children that have a MusicContext
            std::vector<int> childrenWithMC;
            for (int ci = m_cueIdx + 1;
                 ci <= m_cueIdx + c->childCount && ci < m_model->cues().cueCount(); ++ci) {
                const mcp::Cue* child = m_model->cues().cueAt(ci);
                if (child && child->parentIndex == m_cueIdx && m_model->cues().hasMusicContext(ci))
                    childrenWithMC.push_back(ci);
                // Skip over nested group descendants
                if (child && child->type == mcp::CueType::Group)
                    ci += child->childCount;
            }

            if (childrenWithMC.empty()) {
                QMessageBox::information(this, "Inherit MC",
                    "No direct children with a Music Context found.");
                return;
            }

            // Build menu of choices
            QMenu menu(this);
            for (int ci : childrenWithMC) {
                const mcp::Cue* child = m_model->cues().cueAt(ci);
                const QString label = QString("Q%1 %2")
                    .arg(QString::fromStdString(child->cueNumber))
                    .arg(QString::fromStdString(child->name));
                menu.addAction(label, this, [this, ci]() {
                    if (!m_model->cues().hasMusicContext(ci)) return;
                    m_model->pushUndo();
                    // Share by index — no copy; if child is deleted, link auto-clears on rebuild.
                    m_model->cues().setCueMCSource(m_cueIdx, ci);
                    m_mcView->setCueIndex(m_cueIdx);
                    m_mcView->update();
                    ShowHelpers::syncSfFromCues(*m_model);
                    emit cueEdited();
                });
            }
            menu.exec(QCursor::pos());
        });
    }

    // Property panel for selected point
    m_mcPropGroup = new QWidget;
    auto* propLay = new QFormLayout(m_mcPropGroup);
    propLay->setContentsMargins(0, 4, 0, 0);
    propLay->setSpacing(3);

    m_comboPtType = new QComboBox; m_comboPtType->addItems({"Jump", "Ramp"});
    m_spinPtBpm   = new QDoubleSpinBox;
    m_spinPtBpm->setRange(10.0, 999.0); m_spinPtBpm->setDecimals(2); m_spinPtBpm->setSuffix(" BPM");

    auto* tsRow = new QHBoxLayout;
    m_spinTSNum = new QSpinBox; m_spinTSNum->setRange(1, 32);
    m_spinTSDen = new QSpinBox; m_spinTSDen->setRange(1, 32);
    m_chkTSInherit = new QCheckBox("inherit");
    tsRow->addWidget(m_spinTSNum);
    tsRow->addWidget(new QLabel("/"));
    tsRow->addWidget(m_spinTSDen);
    tsRow->addWidget(m_chkTSInherit);
    tsRow->addStretch();

    m_lblPtPos = new QLabel("—");

    propLay->addRow("Type:",     m_comboPtType);
    propLay->addRow("BPM:",      m_spinPtBpm);
    propLay->addRow("Time Sig:", tsRow);
    propLay->addRow("Position:", m_lblPtPos);
    m_mcPropGroup->hide();
    contentLay->addWidget(m_mcPropGroup);
    contentLay->addStretch();

    m_mcContent->hide();
    outerLay->addWidget(m_mcContent);
    outerLay->addStretch();

    m_tabs->addTab(m_mcPage, "Music");

    // ── signals ────────────────────────────────────────────────────────────
    connect(m_chkAttachMC, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading || m_cueIdx < 0) return;
        if (on) {
            // Detach any parent-inherit link first
            m_model->pushUndo();
            m_model->cues().setCueMCSource(m_cueIdx, -1);
            // Create default MC: 4/4, 120 BPM, bar 1 beat 1
            auto mc = std::make_unique<mcp::MusicContext>();
            mcp::MusicContext::Point p;
            p.bar = 1; p.beat = 1; p.bpm = 120.0;
            p.isRamp = false; p.hasTimeSig = true; p.timeSigNum = 4; p.timeSigDen = 4;
            mc->points.push_back(p);
            m_model->cues().setCueMusicContext(m_cueIdx, std::move(mc));
        } else {
            m_model->pushUndo();
            m_model->cues().setCueMusicContext(m_cueIdx, nullptr);
        }
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
        // Reload the tab without losing the current tab selection
        const int cIdx = m_cueIdx;
        m_cueIdx = -1;
        setCueIndex(cIdx);
    });

    connect(m_chkInheritParentMC, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading || m_cueIdx < 0) return;
        const mcp::Cue* c = m_model->cues().cueAt(m_cueIdx);
        if (!c) return;
        m_model->pushUndo();
        if (on) {
            // Link to parent's MC; clear any own MC first
            m_model->cues().setCueMusicContext(m_cueIdx, nullptr);
            m_model->cues().setCueMCSource(m_cueIdx, c->parentIndex);
        } else {
            m_model->cues().setCueMCSource(m_cueIdx, -1);
        }
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
        const int cIdx = m_cueIdx;
        m_cueIdx = -1;
        setCueIndex(cIdx);
    });

    connect(m_chkApplyBefore, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading || m_cueIdx < 0) return;
        auto* mc = m_model->cues().musicContextOf(m_cueIdx);
        if (!mc) return;
        m_model->pushUndo();
        mc->applyBeforeStart = on;
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });

    connect(m_mcView, &MusicContextView::pointSelected, this, [this](int pt) {
        m_selMCPt = pt;
        loadMCPropPanel();
    });

    connect(m_mcView, &MusicContextView::mcChanged, this, [this] {
        if (m_cueIdx < 0) return;
        ShowHelpers::syncSfFromCues(*m_model);
        loadMCPropPanel();  // refresh position label after drag
        emit cueEdited();
    });

    // Property panel changes
    auto onPropChanged = [this] {
        if (m_loading || m_cueIdx < 0 || m_selMCPt < 0) return;
        auto* mc = m_model->cues().musicContextOf(m_cueIdx);
        if (!mc || m_selMCPt >= (int)mc->points.size()) return;
        m_model->pushUndo();
        auto& pt = mc->points[m_selMCPt];
        pt.isRamp    = (m_comboPtType->currentIndex() == 1) && (m_selMCPt > 0);
        pt.bpm       = m_spinPtBpm->value();
        const bool inherit = m_chkTSInherit->isChecked() && (m_selMCPt > 0);
        pt.hasTimeSig = !inherit;
        if (pt.hasTimeSig) { pt.timeSigNum = m_spinTSNum->value(); pt.timeSigDen = m_spinTSDen->value(); }
        mc->markDirty();
        m_mcView->update();
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    };
    connect(m_comboPtType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onPropChanged);
    connect(m_spinPtBpm,   &QDoubleSpinBox::editingFinished, this, onPropChanged);
    connect(m_spinTSNum,   &QSpinBox::editingFinished,       this, onPropChanged);
    connect(m_spinTSDen,   &QSpinBox::editingFinished,       this, onPropChanged);
    connect(m_chkTSInherit, &QCheckBox::toggled, this, [this, onPropChanged](bool) {
        const bool inh = m_chkTSInherit->isChecked();
        m_spinTSNum->setEnabled(!inh);
        m_spinTSDen->setEnabled(!inh);
        onPropChanged();
    });
}

void InspectorWidget::loadMCPropPanel() {
    if (m_cueIdx < 0) { m_mcPropGroup->hide(); return; }
    const auto* c = m_model->cues().cueAt(m_cueIdx);
    const auto* mc = m_model->cues().musicContextOf(m_cueIdx);
    if (!mc || m_selMCPt < 0 || m_selMCPt >= (int)mc->points.size()) {
        m_mcPropGroup->hide();
        return;
    }
    m_loading = true;
    const auto& pt = mc->points[m_selMCPt];
    m_comboPtType->setCurrentIndex(pt.isRamp ? 1 : 0);
    m_comboPtType->setEnabled(m_selMCPt > 0);
    m_spinPtBpm->setValue(pt.bpm);
    const bool inherit = !pt.hasTimeSig && (m_selMCPt > 0);
    m_chkTSInherit->setChecked(inherit);
    m_chkTSInherit->setEnabled(m_selMCPt > 0);
    m_spinTSNum->setValue(pt.hasTimeSig ? pt.timeSigNum : 4);
    m_spinTSDen->setValue(pt.hasTimeSig ? pt.timeSigDen : 4);
    m_spinTSNum->setEnabled(!inherit);
    m_spinTSDen->setEnabled(!inherit);
    m_lblPtPos->setText(QString("%1 | %2").arg(pt.bar).arg(pt.beat));
    m_mcPropGroup->show();
    m_loading = false;
}

void InspectorWidget::buildTimelineTab() {
    m_timelinePage = new QWidget;
    auto* lay = new QVBoxLayout(m_timelinePage);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_timelineView = new TimelineGroupView(m_model, m_timelinePage);
    lay->addWidget(m_timelineView);
    lay->addStretch(1);

    m_tabs->addTab(m_timelinePage, "Timeline");

    connect(m_timelineView, &TimelineGroupView::childOffsetChanged,
            this, [this](int childFlatIdx, double newOffsetSec) {
        m_model->pushUndo();
        m_model->cues().setCueTimelineOffset(childFlatIdx, newOffsetSec);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });

    connect(m_timelineView, &TimelineGroupView::childTrimChanged,
            this, [this](int childFlatIdx, double newOffsetSec,
                         double newStartTimeSec, double newDurationSec) {
        m_model->pushUndo();
        m_model->cues().setCueTimelineOffset(childFlatIdx, newOffsetSec);
        m_model->cues().setCueStartTime(childFlatIdx, newStartTimeSec);
        m_model->cues().setCueDuration(childFlatIdx, newDurationSec);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });

    connect(m_timelineView, &TimelineGroupView::rulerClicked,
            this, [this](double timeSec) {
        if (m_cueIdx < 0) return;
        m_model->cues().setCueTimelineArmSec(m_cueIdx, timeSec);
        emit cueEdited();   // refresh table so "armed" state appears
    });
}

void InspectorWidget::buildMarkerTab() {
    m_markerPage = new QWidget;
    auto* form = new QFormLayout(m_markerPage);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);

    m_comboMarkerTargetList = new QComboBox;
    m_comboMarkerTarget     = new QComboBox;
    m_comboMarkerMkIdx      = new QComboBox;
    form->addRow("Target list:", m_comboMarkerTargetList);
    form->addRow("Target cue:",  m_comboMarkerTarget);
    form->addRow("Marker:",      m_comboMarkerMkIdx);

    m_tabs->addTab(m_markerPage, "Marker");

    connect(m_comboMarkerTargetList, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (m_loading) return;
        refreshMarkerTargetCombo();
        refreshMarkerMkIdxCombo();
        onBasicChanged();
    });
    connect(m_comboMarkerTarget, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (m_loading) return;
        refreshMarkerMkIdxCombo();
        onBasicChanged();
    });
    connect(m_comboMarkerMkIdx, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (m_loading) return;
        onBasicChanged();
    });
}

// ── public API ─────────────────────────────────────────────────────────────

void InspectorWidget::setCueIndex(int idx) {
    m_cueIdx    = idx;
    m_selMarker = -1;
    m_loading   = true;

    const mcp::Cue* c = (idx >= 0) ? m_model->cues().cueAt(idx) : nullptr;

    const bool isAudio    = c && c->type == mcp::CueType::Audio;
    const bool isFade     = c && c->type == mcp::CueType::Fade;
    const bool isDevamp   = c && c->type == mcp::CueType::Devamp;
    const bool isArm      = c && c->type == mcp::CueType::Arm;
    const bool isGroup    = c && c->type == mcp::CueType::Group;
    const bool isMCCue    = c && c->type == mcp::CueType::MusicContext;
    const bool isSyncGroup = isGroup && c->groupData &&
                             c->groupData->mode == mcp::GroupData::Mode::Sync;
    const bool isTimeline  = isGroup && c->groupData &&
                             c->groupData->mode == mcp::GroupData::Mode::Timeline;

    const bool isAutomation  = c && c->type == mcp::CueType::Automation;
    const bool hasMC = isAudio || isMCCue || isAutomation || (isGroup && (isSyncGroup || isTimeline));

    m_tabs->setTabVisible(m_tabs->indexOf(m_mcPage),       hasMC);
    m_tabs->setTabVisible(m_tabs->indexOf(m_levelsPage),   isAudio || isFade);
    m_tabs->setTabVisible(m_tabs->indexOf(m_trimPage),     isAudio);
    m_tabs->setTabVisible(m_tabs->indexOf(m_timePage),     isAudio || isSyncGroup);
    m_tabs->setTabVisible(m_tabs->indexOf(m_curvePage),    isFade);
    m_tabs->setTabVisible(m_tabs->indexOf(m_modePage),     isGroup);
    m_tabs->setTabVisible(m_tabs->indexOf(m_timelinePage), isTimeline);

    const bool isMarkerCue   = c && c->type == mcp::CueType::Marker;
    const bool isNetworkCue  = c && c->type == mcp::CueType::Network;
    const bool isMidiCue     = c && c->type == mcp::CueType::Midi;
    const bool isTimecodeCue = c && c->type == mcp::CueType::Timecode;
    const bool isScriptlet   = c && c->type == mcp::CueType::Scriptlet;
    const bool isSnapshot    = c && c->type == mcp::CueType::Snapshot;
    m_tabs->setTabVisible(m_tabs->indexOf(m_scriptPage),       isScriptlet);
    m_tabs->setTabVisible(m_tabs->indexOf(m_snapshotPage),     isSnapshot);
    m_tabs->setTabVisible(m_tabs->indexOf(m_automationPage),   isAutomation);
    m_tabs->setTabVisible(m_tabs->indexOf(m_markerPage),    isMarkerCue);
    m_tabs->setTabVisible(m_tabs->indexOf(m_networkPage),   isNetworkCue);
    m_tabs->setTabVisible(m_tabs->indexOf(m_midiPage),      isMidiCue);
    m_tabs->setTabVisible(m_tabs->indexOf(m_timecodePage),  isTimecodeCue);
    m_tabs->setTabVisible(m_tabs->indexOf(m_triggersPage),  c != nullptr);
    m_spinDurationBasic->setVisible(true);
    m_devampGroup->setVisible(isDevamp);
    m_armGroup->setVisible(isArm);

    // Music Context tab
    if (hasMC) {
        bool mcAttached = c && m_model->cues().hasMusicContext(idx);
        // MC cues always have an attached MC — auto-create if missing.
        if (isMCCue && !mcAttached) {
            auto mc = std::make_unique<mcp::MusicContext>();
            mcp::MusicContext::Point p;
            p.bar = 1; p.beat = 1; p.bpm = 120.0;
            p.hasTimeSig = true; p.timeSigNum = 4; p.timeSigDen = 4;
            mc->points.push_back(p);
            m_model->cues().setCueMusicContext(idx, std::move(mc));
            ShowHelpers::syncSfFromCues(*m_model);
            mcAttached = true;
        }

        // Parent MC inheritance: show checkbox only when parent has an MC
        const int parentIdx = c ? c->parentIndex : -1;
        const bool parentHasMC = (parentIdx >= 0) &&
                                  (m_model->cues().musicContextOf(parentIdx) != nullptr);
        const bool isInheriting = parentHasMC &&
                                   (m_model->cues().cueMCSourceIdx(idx) == parentIdx);
        m_chkInheritParentMC->setVisible(parentHasMC);
        m_chkInheritParentMC->setChecked(isInheriting);

        // For MC cues the attach checkbox is always checked and disabled
        if (isMCCue) {
            m_chkAttachMC->setChecked(true);
            m_chkAttachMC->setEnabled(false);
        } else {
            m_chkAttachMC->setEnabled(!isInheriting);
            m_chkAttachMC->setChecked(mcAttached && !isInheriting);
        }

        // When inheriting, show content read-only (no own MC edit, no button row)
        const bool showContent = mcAttached || isInheriting;
        m_mcContent->setVisible(showContent);
        m_selMCPt = -1;
        if (showContent) {
            const auto* mc = m_model->cues().musicContextOf(idx);
            if (mc) m_chkApplyBefore->setChecked(mc->applyBeforeStart);
            m_chkApplyBefore->setEnabled(!isInheriting);
            m_mcBtnRow->setVisible(!isInheriting);
            m_mcView->setCueIndex(idx);
            m_mcView->setEnabled(!isInheriting);
            m_mcPropGroup->hide();
        }
    } else {
        m_chkInheritParentMC->hide();
    }

    loadBasic();
    if (isAudio || isFade) rebuildLevelsForCue();
    if (isAudio) { loadTrim(); loadTime(); }
    if (isSyncGroup) loadTime();
    if (isFade)  loadCurve();
    if (isGroup) {
        loadMode();
        if (isTimeline) m_timelineView->setGroupCueIndex(idx);
    }
    if (isScriptlet)   loadScript();
    if (isSnapshot)    loadSnapshotTab();
    if (isAutomation)  loadAutomationTab();
    if (isNetworkCue)  loadNetwork();
    if (isMidiCue)     loadMidi();
    if (isTimecodeCue) loadTimecode();
    if (c)             loadTriggers();

    // Pass MC to timeline views for bar/beat ruler
    {
        const mcp::MusicContext* mc = c ? m_model->cues().musicContextOf(idx) : nullptr;
        const double startTime = (c && c->type == mcp::CueType::Audio) ? c->startTime : 0.0;
        if (m_waveform)      m_waveform->setMusicContext(mc, startTime);
        if (m_timelineView)  m_timelineView->setMusicContext(mc);
        if (m_syncGroupView) m_syncGroupView->setMusicContext(mc);
    }

    m_markerPanel->hide();
    m_loading = false;
}

void InspectorWidget::updatePlayhead() {
    if (m_waveform) m_waveform->updatePlayhead();
}

void InspectorWidget::clearTimelineArm() {
    if (m_timelineView)
        m_timelineView->clearArmCursor();
    if (m_syncGroupView)
        m_syncGroupView->clearArmCursor();
}

int InspectorWidget::currentTabIndex() const {
    return m_tabs->currentIndex();
}

void InspectorWidget::restoreTabIndex(int idx) {
    if (idx >= 0 && idx < m_tabs->count() && m_tabs->isTabVisible(idx))
        m_tabs->setCurrentIndex(idx);
}

// ── load helpers ───────────────────────────────────────────────────────────

void InspectorWidget::loadBasic() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;
    const bool en = c != nullptr;

    m_editNum->setEnabled(en);
    m_editName->setEnabled(en);
    m_spinPreWait->setEnabled(en);
    m_comboGoQuantize->setEnabled(en);
    m_chkAutoCont->setEnabled(en);
    m_chkAutoFollow->setEnabled(en);

    const bool isAudio = c && c->type == mcp::CueType::Audio;
    m_audioFileRow->setVisible(isAudio);

    if (!c) {
        m_editNum->clear(); m_editName->clear();
        m_spinPreWait->setValue(0.0);
        m_comboGoQuantize->setCurrentIndex(0);
        m_chkAutoCont->setChecked(false);
        m_chkAutoFollow->setChecked(false);
        return;
    }

    if (isAudio) {
        namespace fs = std::filesystem;
        const fs::path base(m_model->baseDir);
        fs::path p(c->path);
        if (p.is_relative() && !m_model->baseDir.empty()) p = base / p;
        static_cast<AudioPathWidget*>(m_audioPathWidget)->setPath(
            QString::fromStdString(p.string()));
    }

    m_editNum->setText(QString::fromStdString(c->cueNumber));
    m_editName->setText(QString::fromStdString(c->name));
    m_spinPreWait->setValue(c->preWaitSeconds);
    m_comboGoQuantize->setCurrentIndex(std::clamp(c->goQuantize, 0, 2));
    // Fade cues treat stored duration=0 as "use 3 s default" — show the real value.
    const double effectiveDur = (c->type == mcp::CueType::Fade && c->duration == 0.0)
                                ? 3.0 : c->duration;
    m_spinDurationBasic->setValue(effectiveDur);
    m_chkAutoCont->setChecked(c->autoContinue);
    m_chkAutoFollow->setChecked(c->autoFollow);
    if (c->type == mcp::CueType::Devamp) {
        m_comboDevampMode->setCurrentIndex(c->devampMode);
        m_chkDevampPreVamp->setChecked(c->devampPreVamp);
    }
    if (c->type == mcp::CueType::Arm)
        m_spinArmStart->setValue(c->armStartTime);
    if (c->type == mcp::CueType::Marker) {
        // Read targetListId from ShowFile (engine Cue doesn't store it)
        int targetListId = -1;
        if (const auto* sfCue = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), m_cueIdx))
            targetListId = sfCue->targetListId;

        m_comboMarkerTargetList->clear();
        m_comboMarkerTargetList->addItem("(Same list)", -1);
        for (int li = 0; li < (int)m_model->sf.cueLists.size(); ++li) {
            m_comboMarkerTargetList->addItem(
                QString::fromStdString(m_model->sf.cueLists[li].name),
                m_model->sf.cueLists[li].numericId);
        }
        for (int j = 0; j < m_comboMarkerTargetList->count(); ++j) {
            if (m_comboMarkerTargetList->itemData(j).toInt() == targetListId) {
                m_comboMarkerTargetList->setCurrentIndex(j);
                break;
            }
        }

        refreshMarkerTargetCombo();
        refreshMarkerMkIdxCombo();
    }
}

void InspectorWidget::replaceAudioFile(const QString& newAbsPath) {
    if (m_cueIdx < 0) return;
    const mcp::Cue* c = m_model->cues().cueAt(m_cueIdx);
    if (!c || c->type != mcp::CueType::Audio) return;

    m_model->pushUndo();

    namespace fs = std::filesystem;
    const fs::path base(m_model->baseDir);
    fs::path np(newAbsPath.toStdString());
    std::error_code ec;
    fs::path rel = fs::relative(np, base, ec);
    const std::string relStr = rel.string();
    std::string pathToStore = (!ec && relStr.find("..") == std::string::npos)
                              ? relStr : np.string();

    auto* sfCue = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), m_cueIdx);
    if (!sfCue) return;
    sfCue->path = pathToStore;

    ShowHelpers::reloadEngineCueAudio(*m_model, m_model->activeListIdx(), m_cueIdx);
    m_model->dirty = true;

    // Update the path widget immediately (show the resolved absolute path)
    static_cast<AudioPathWidget*>(m_audioPathWidget)->setPath(newAbsPath);

    emit cueEdited();
}

void InspectorWidget::loadTrim() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;
    m_trimFader->setValue(c ? static_cast<float>(c->trim) : 0.0f);
}

void InspectorWidget::loadTime() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;

    const bool isSyncGroup = c && c->type == mcp::CueType::Group && c->groupData &&
                             c->groupData->mode == mcp::GroupData::Mode::Sync;

    if (isSyncGroup) {
        m_audioTimeSection->hide();
        m_syncGroupView->show();
        m_markerPanel->hide();
        loadSyncSection();
        return;
    }

    m_audioTimeSection->show();
    m_syncGroupView->hide();

    if (!c) {
        m_spinStart->setValue(0.0);
        m_spinDuration->setValue(0.0);
        m_waveform->setCueIndex(-1);
        return;
    }
    m_spinStart->setValue(c->startTime);
    m_spinDuration->setValue(c->duration);
    m_waveform->setCueIndex(m_cueIdx);
}

void InspectorWidget::loadSyncSection() {
    m_syncGroupView->setGroupCueIndex(m_cueIdx);
}

void InspectorWidget::loadCurve() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;
    if (!c || !c->fadeData) {
        m_comboCurve->setCurrentIndex(0);
        m_chkStopWhenDone->setChecked(false);
        return;
    }
    m_comboCurve->setCurrentIndex(
        c->fadeData->curve == mcp::FadeData::Curve::EqualPower ? 1 : 0);
    m_chkStopWhenDone->setChecked(c->fadeData->stopWhenDone);
}

void InspectorWidget::loadMode() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;
    if (!c || !c->groupData) return;

    const auto mode = c->groupData->mode;
    for (int i = 0; i < m_comboGroupMode->count(); ++i) {
        if (m_comboGroupMode->itemData(i).toInt() == static_cast<int>(mode)) {
            m_comboGroupMode->setCurrentIndex(i);
            break;
        }
    }
    m_chkGroupRandom->setChecked(c->groupData->random);
    m_chkGroupRandom->setEnabled(mode == mcp::GroupData::Mode::Playlist);
}

// ── rebuildLevelsForCue ────────────────────────────────────────────────────
//
// Layout (Audio):
//   Fader row:  [Master]  [Out1] [Out2] [Out3] ...
//   Crosspoint:
//       headers:         Out1    Out2   ...
//       row "Src 1":    [0.0]   [—]    ...
//       row "Src 2":    [—]     [0.0]  ...
//
// Layout (Fade): master-target fader + per-output target faders (with en checkboxes)

void InspectorWidget::rebuildLevelsForCue() {
    delete m_levelsContent;
    m_outFaders.clear();
    m_fadeOutFaders.clear();
    m_xpCells.clear();
    m_fadeXpCells.clear();
    m_masterFader     = nullptr;
    m_fadeMasterFader = nullptr;

    m_levelsContent = new QWidget;
    auto* lay = new QVBoxLayout(m_levelsContent);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(8);
    m_levelsScroll->setWidget(m_levelsContent);

    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;
    if (!c) return;

    if (c->type == mcp::CueType::Audio) {
        const int outCh = m_model->channelCount();
        const int srcCh = c->audioFile.isLoaded() ? c->audioFile.metadata().channels : 1;

        // ── Fader row ──────────────────────────────────────────────────
        auto* fadersRow = new QHBoxLayout;
        fadersRow->setSpacing(4);

        // Master fader (labelled "main")
        m_masterFader = new FaderWidget("main", m_levelsContent);
        m_masterFader->setValue(static_cast<float>(c->level));
        fadersRow->addWidget(m_masterFader);

        // Separator
        auto* sep = new QFrame(m_levelsContent);
        sep->setFrameShape(QFrame::VLine);
        sep->setStyleSheet("color:#333;");
        fadersRow->addWidget(sep);

        // Per-output faders
        for (int o = 0; o < outCh; ++o) {
            float dB = 0.0f;
            if (o < (int)c->routing.outLevelDb.size())
                dB = c->routing.outLevelDb[o];
            auto* fw = new FaderWidget(m_model->channelName(o), m_levelsContent);
            fw->setValue(dB);
            m_outFaders.push_back(fw);
            fadersRow->addWidget(fw);
            connect(fw, &FaderWidget::dragStarted, this, [this]() {
                if (!m_loading && m_cueIdx >= 0) m_model->pushUndo();
            });
            connect(fw, &FaderWidget::valueChanged, this, [this, o](float dB) {
                onLevelFaderChanged(o, dB);
            });
        }
        fadersRow->addStretch();
        connect(m_masterFader, &FaderWidget::dragStarted, this, [this]() {
            if (!m_loading && m_cueIdx >= 0) m_model->pushUndo();
        });
        connect(m_masterFader, &FaderWidget::valueChanged,
                this, &InspectorWidget::onMasterFaderChanged);
        lay->addLayout(fadersRow);

        // ── Crosspoint grid ────────────────────────────────────────────
        if (srcCh > 0 && outCh > 0) {
            auto* xpGroup = new QGroupBox("Crosspoint", m_levelsContent);
            xpGroup->setStyleSheet(
                "QGroupBox{color:#888;border:1px solid #2a2a2a;"
                "margin-top:8px;padding-top:6px;border-radius:4px;}"
                "QGroupBox::title{subcontrol-origin:margin;left:8px;}");
            auto* grid = new QGridLayout(xpGroup);
            grid->setSpacing(3);
            grid->setContentsMargins(6, 12, 6, 6);

            constexpr int kCellW = 46;
            constexpr int kLblW  = 40;
            constexpr int kSp    = 3;
            // Compact fixed width: label col + outCh data cols + outCh gaps + margins + border
            xpGroup->setFixedWidth(kLblW + outCh * (kCellW + kSp) + 14);
            // Absorb any extra horizontal space in an empty stretch column
            grid->setColumnStretch(outCh + 1, 1);

            for (int o = 0; o < outCh; ++o) {
                auto* lbl = new QLabel(m_model->channelName(o), xpGroup);
                lbl->setFixedWidth(kCellW);
                lbl->setAlignment(Qt::AlignHCenter);
                lbl->setStyleSheet("color:#888;font-size:10px;");
                grid->addWidget(lbl, 0, o + 1);
            }

            m_xpCells.resize(srcCh, std::vector<QLineEdit*>(outCh, nullptr));
            for (int s = 0; s < srcCh; ++s) {
                auto* rowLbl = new QLabel(QString("Src %1").arg(s + 1), xpGroup);
                rowLbl->setFixedWidth(kLblW);
                rowLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                rowLbl->setStyleSheet("color:#888;font-size:11px;");
                grid->addWidget(rowLbl, s + 1, 0);

                for (int o = 0; o < outCh; ++o) {
                    std::optional<float> xp;
                    if (s < (int)c->routing.xpoint.size()
                        && o < (int)c->routing.xpoint[s].size())
                        xp = c->routing.xpoint[s][o];
                    // No lazy fallback: setCueXpoint always initializes diagonal explicitly.
                    // Brand-new cues (xpoint empty) fall through to cell->clear() below,
                    // and the first edit via editingFinished will initialize properly.

                    auto* cell = makeXpCell();
                    if (xp.has_value())
                        cell->setText(fmtXpDb(*xp));
                    else
                        cell->clear();

                    m_xpCells[s][o] = cell;
                    grid->addWidget(cell, s + 1, o + 1);

                    connect(cell, &QLineEdit::editingFinished, this, [this, s, o]() {
                        if (m_loading || m_cueIdx < 0) return;
                        if (s >= (int)m_xpCells.size()) return;
                        if (o >= (int)m_xpCells[s].size()) return;
                        auto* ce = m_xpCells[s][o];
                        if (!ce) return;
                        m_model->pushUndo();
                        const QString txt = ce->text().trimmed();
                        if (txt.isEmpty()) {
                            m_model->cues().setCueXpoint(m_cueIdx, s, o, std::nullopt);
                        } else {
                            bool ok = false;
                            float dB = txt.toFloat(&ok);
                            // Non-numeric or below-floor → -inf (true silence, 0 linear)
                            if (!ok || dB < FaderWidget::kFaderMin)
                                dB = FaderWidget::kFaderInf;
                            else
                                dB = std::min(dB, FaderWidget::kFaderMax);
                            m_model->cues().setCueXpoint(m_cueIdx, s, o, dB);
                            ce->setText(dB <= FaderWidget::kFaderMin + 0.05f
                                ? QStringLiteral("-inf")
                                : QString::number(static_cast<double>(dB), 'f', 1));
                        }
                        ShowHelpers::syncSfFromCues(*m_model);
                        emit cueEdited();
                    });
                }
            }

            lay->addWidget(xpGroup, 0, Qt::AlignLeft);
        }

    } else if (c->type == mcp::CueType::Fade && c->fadeData) {
        const auto& fd = *c->fadeData;
        const int outCh = m_model->channelCount();

        // Helper: create a toggleable fader for a fade target.
        // Clicking the label toggles the "enabled" state; dragging sets target dB.
        auto makeFadeFader = [&](const QString& lbl, bool enabled, float targetDb)
                -> FaderWidget* {
            auto* fw = new FaderWidget(lbl, m_levelsContent);
            fw->setToggleable(true);
            fw->setValue(targetDb);
            fw->setActivated(enabled);
            return fw;
        };

        // ── Fader row: [Master] | [Out1] [Out2] … ─────────────────────────
        auto* fadersRow = new QHBoxLayout;
        fadersRow->setSpacing(4);

        m_fadeMasterFader = makeFadeFader("Master",
            fd.masterLevel.enabled, fd.masterLevel.targetDb);
        fadersRow->addWidget(m_fadeMasterFader);

        auto* sep = new QFrame(m_levelsContent);
        sep->setFrameShape(QFrame::VLine);
        sep->setStyleSheet("color:#333;");
        fadersRow->addWidget(sep);

        m_model->cues().setCueFadeOutTargetCount(m_cueIdx, outCh);
        for (int o = 0; o < outCh; ++o) {
            bool  enabled = (o < (int)fd.outLevels.size()) ? fd.outLevels[o].enabled  : false;
            float target  = (o < (int)fd.outLevels.size()) ? fd.outLevels[o].targetDb : 0.0f;
            auto* fw = makeFadeFader(m_model->channelName(o), enabled, target);
            m_fadeOutFaders.push_back(fw);
            fadersRow->addWidget(fw);
            connect(fw, &FaderWidget::dragStarted, this, [this]() {
                if (!m_loading && m_cueIdx >= 0) m_model->pushUndo();
            });
            connect(fw, &FaderWidget::toggled, this, [this, o](bool en) {
                if (m_loading || m_cueIdx < 0) return;
                const mcp::Cue* c2 = m_model->cues().cueAt(m_cueIdx);
                if (!c2 || !c2->fadeData) return;
                float tgt = (o < (int)c2->fadeData->outLevels.size())
                    ? c2->fadeData->outLevels[o].targetDb : 0.0f;
                m_model->cues().setCueFadeOutTarget(m_cueIdx, o, en, tgt);
                ShowHelpers::syncSfFromCues(*m_model);
                emit cueEdited();
            });
            connect(fw, &FaderWidget::valueChanged, this, [this, o](float dB) {
                onFadeOutTargetChanged(o, dB);
            });
        }
        fadersRow->addStretch();

        connect(m_fadeMasterFader, &FaderWidget::dragStarted, this, [this]() {
            if (!m_loading && m_cueIdx >= 0) m_model->pushUndo();
        });
        connect(m_fadeMasterFader, &FaderWidget::toggled, this, [this](bool en) {
            if (m_loading || m_cueIdx < 0) return;
            const mcp::Cue* c2 = m_model->cues().cueAt(m_cueIdx);
            if (!c2 || !c2->fadeData) return;
            m_model->cues().setCueFadeMasterTarget(m_cueIdx, en,
                c2->fadeData->masterLevel.targetDb);
            ShowHelpers::syncSfFromCues(*m_model);
            emit cueEdited();
        });
        connect(m_fadeMasterFader, &FaderWidget::valueChanged,
                this, &InspectorWidget::onFadeMasterTargetChanged);
        lay->addLayout(fadersRow);

        // ── Crosspoint fade targets ────────────────────────────────────────
        // Determine grid dimensions from existing xpTargets or target cue.
        int xpSrcCh = 0;
        int xpOutCh = outCh;
        if (!fd.xpTargets.empty()) {
            xpSrcCh = (int)fd.xpTargets.size();
            if (!fd.xpTargets[0].empty()) xpOutCh = (int)fd.xpTargets[0].size();
        } else {
            const int tIdx = fd.resolvedTargetIdx;
            const mcp::Cue* tgt = (tIdx >= 0) ? m_model->cues().cueAt(tIdx) : nullptr;
            if (tgt && tgt->audioFile.isLoaded())
                xpSrcCh = tgt->audioFile.metadata().channels;
        }

        if (xpSrcCh > 0 && xpOutCh > 0) {
            // Ensure matrix is sized (no-op if already correct).
            m_model->cues().setCueFadeXpSize(m_cueIdx, xpSrcCh, xpOutCh);
            // Re-read fd pointer (setCueFadeXpSize may reallocate vectors).
            const auto& fd2 = *m_model->cues().cueAt(m_cueIdx)->fadeData;

            auto* xpGroup = new QGroupBox("Crosspoint fade", m_levelsContent);
            xpGroup->setStyleSheet(
                "QGroupBox{color:#888;border:1px solid #2a2a2a;"
                "margin-top:8px;padding-top:6px;border-radius:4px;}"
                "QGroupBox::title{subcontrol-origin:margin;left:8px;}");
            auto* grid = new QGridLayout(xpGroup);
            grid->setSpacing(3);
            grid->setContentsMargins(6, 12, 6, 6);

            constexpr int kCellW = 46;
            constexpr int kLblW  = 40;
            constexpr int kSp    = 3;
            xpGroup->setFixedWidth(kLblW + xpOutCh * (kCellW + kSp) + 14);
            grid->setColumnStretch(xpOutCh + 1, 1);

            for (int o = 0; o < xpOutCh; ++o) {
                auto* lbl = new QLabel(m_model->channelName(o), xpGroup);
                lbl->setFixedWidth(kCellW);
                lbl->setAlignment(Qt::AlignHCenter);
                lbl->setStyleSheet("color:#888;font-size:10px;");
                grid->addWidget(lbl, 0, o + 1);
            }

            m_fadeXpCells.resize(xpSrcCh, std::vector<QLineEdit*>(xpOutCh, nullptr));
            for (int s = 0; s < xpSrcCh; ++s) {
                auto* rowLbl = new QLabel(QString("Src %1").arg(s + 1), xpGroup);
                rowLbl->setFixedWidth(kLblW);
                rowLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                rowLbl->setStyleSheet("color:#888;font-size:11px;");
                grid->addWidget(rowLbl, s + 1, 0);

                for (int o = 0; o < xpOutCh; ++o) {
                    bool  en  = false;
                    float tdb = 0.0f;
                    if (s < (int)fd2.xpTargets.size() &&
                        o < (int)fd2.xpTargets[s].size()) {
                        en  = fd2.xpTargets[s][o].enabled;
                        tdb = fd2.xpTargets[s][o].targetDb;
                    }

                    auto* cell = makeXpCell();
                    if (en)
                        cell->setText(fmtXpDb(tdb));
                    else
                        cell->clear();
                    cell->setPlaceholderText("—");

                    m_fadeXpCells[s][o] = cell;
                    grid->addWidget(cell, s + 1, o + 1);

                    connect(cell, &QLineEdit::editingFinished, this, [this, s, o]() {
                        if (m_loading || m_cueIdx < 0) return;
                        if (s >= (int)m_fadeXpCells.size()) return;
                        if (o >= (int)m_fadeXpCells[s].size()) return;
                        auto* ce = m_fadeXpCells[s][o];
                        if (!ce) return;
                        m_model->pushUndo();
                        const QString txt = ce->text().trimmed();
                        if (txt.isEmpty()) {
                            // Empty = deactivate (distinct from -inf which activates at silence)
                            m_model->cues().setCueFadeXpTarget(m_cueIdx, s, o, false, 0.0f);
                        } else {
                            bool ok = false;
                            float dB = txt.toFloat(&ok);
                            // Non-numeric or below-floor → -inf (activate at true silence)
                            if (!ok || dB < FaderWidget::kFaderMin)
                                dB = FaderWidget::kFaderInf;
                            else
                                dB = std::min(dB, FaderWidget::kFaderMax);
                            m_model->cues().setCueFadeXpTarget(m_cueIdx, s, o, true, dB);
                            ce->setText(dB <= FaderWidget::kFaderMin + 0.05f
                                ? QStringLiteral("-inf")
                                : QString::number(static_cast<double>(dB), 'f', 1));
                        }
                        ShowHelpers::syncSfFromCues(*m_model);
                        emit cueEdited();
                    });
                }
            }
            lay->addWidget(xpGroup, 0, Qt::AlignLeft);
        }
    }

    lay->addStretch();
}

// ── Marker cue combo helpers ──────────────────────────────────────────────

void InspectorWidget::refreshMarkerTargetCombo() {
    const bool wasLoading = m_loading;
    m_loading = true;
    m_comboMarkerTarget->clear();

    // Determine which list to scan for target cues
    const int listId = m_comboMarkerTargetList
                       ? m_comboMarkerTargetList->currentData().toInt() : -1;
    int targetListIdx = m_model->activeListIdx();
    if (listId >= 0) {
        for (int li = 0; li < (int)m_model->sf.cueLists.size(); ++li) {
            if (m_model->sf.cueLists[li].numericId == listId) { targetListIdx = li; break; }
        }
    }

    if (targetListIdx >= 0 && targetListIdx < m_model->listCount()) {
        auto& cl = m_model->cueListAt(targetListIdx);
        for (int i = 0; i < cl.cueCount(); ++i) {
            const auto* c = cl.cueAt(i);
            if (!c) continue;
            const bool isAudio = c->type == mcp::CueType::Audio;
            const bool isSyncGroup = c->type == mcp::CueType::Group && c->groupData &&
                                     c->groupData->mode == mcp::GroupData::Mode::Sync;
            if (!isAudio && !isSyncGroup) continue;
            m_comboMarkerTarget->addItem(
                QString("Q%1 %2").arg(QString::fromStdString(c->cueNumber))
                                  .arg(QString::fromStdString(c->name)), i);
        }
    }

    // Select the current target (flat index within the target list)
    const mcp::Cue* cur = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;
    if (cur && cur->targetIndex >= 0) {
        for (int j = 0; j < m_comboMarkerTarget->count(); ++j) {
            if (m_comboMarkerTarget->itemData(j).toInt() == cur->targetIndex) {
                m_comboMarkerTarget->setCurrentIndex(j);
                break;
            }
        }
    }
    m_loading = wasLoading;
}

void InspectorWidget::refreshMarkerMkIdxCombo() {
    const bool wasLoading = m_loading;
    m_loading = true;
    m_comboMarkerMkIdx->clear();
    const mcp::Cue* cur = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;
    const int ti = (m_comboMarkerTarget->count() > 0)
                   ? m_comboMarkerTarget->currentData().toInt() : -1;

    // Resolve the target list (same as in refreshMarkerTargetCombo)
    const int listId = m_comboMarkerTargetList
                       ? m_comboMarkerTargetList->currentData().toInt() : -1;
    int targetListIdx = m_model->activeListIdx();
    if (listId >= 0) {
        for (int li = 0; li < (int)m_model->sf.cueLists.size(); ++li) {
            if (m_model->sf.cueLists[li].numericId == listId) { targetListIdx = li; break; }
        }
    }

    const mcp::Cue* target = (ti >= 0 && targetListIdx < m_model->listCount())
                             ? m_model->cueListAt(targetListIdx).cueAt(ti) : nullptr;
    if (target) {
        for (int mi = 0; mi < (int)target->markers.size(); ++mi) {
            const auto& mk = target->markers[static_cast<size_t>(mi)];
            const QString label = mk.name.empty()
                ? QString("Marker %1 (%2 s)").arg(mi + 1).arg(mk.time, 0, 'f', 3)
                : QString("%1 (%2 s)").arg(QString::fromStdString(mk.name)).arg(mk.time, 0, 'f', 3);
            m_comboMarkerMkIdx->addItem(label, mi);
        }
    }
    // Select the current markerIndex
    if (cur && cur->markerIndex >= 0) {
        for (int j = 0; j < m_comboMarkerMkIdx->count(); ++j) {
            if (m_comboMarkerMkIdx->itemData(j).toInt() == cur->markerIndex) {
                m_comboMarkerMkIdx->setCurrentIndex(j);
                break;
            }
        }
    }
    m_loading = wasLoading;
}

void InspectorWidget::refreshMarkerAnchorCombo() {
    const bool wasLoading = m_loading;
    m_loading = true;
    m_comboMarkerAnchor->clear();
    m_comboMarkerAnchor->addItem("(none)", QPoint(-1, -1));

    const mcp::Cue* audioCue = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;
    if (audioCue && m_selMarker >= 0 && m_selMarker < (int)audioCue->markers.size()) {
        const int activeListIdx = m_model->activeListIdx();
        // Show Marker cues from all lists that point to this cue+marker
        for (int li = 0; li < m_model->listCount(); ++li) {
            auto& cl = m_model->cueListAt(li);
            for (int i = 0; i < cl.cueCount(); ++i) {
                const auto* c = cl.cueAt(i);
                if (!c || c->type != mcp::CueType::Marker) continue;
                // For same-list: check engine targetIndex/markerIndex
                if (li == activeListIdx) {
                    if (c->targetIndex != m_cueIdx || c->markerIndex != m_selMarker) continue;
                } else {
                    // Cross-list: skip — engine targetIndex is ambiguous across lists
                    continue;
                }
                const QString listPrefix = (li != activeListIdx)
                    ? QString("[%1] ").arg(QString::fromStdString(m_model->sf.cueLists[li].name))
                    : QString();
                m_comboMarkerAnchor->addItem(
                    listPrefix + QString("Q%1 %2")
                        .arg(QString::fromStdString(c->cueNumber))
                        .arg(QString::fromStdString(c->name)),
                    QPoint(li, i));
            }
        }

        // Select current anchor (read anchorMarkerListId from sf to handle cross-list)
        int anchorListIdx = activeListIdx;
        int anchorCueIdx = audioCue->markers[static_cast<size_t>(m_selMarker)].anchorMarkerCueIdx;
        if (const auto* sfCue = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), m_cueIdx)) {
            if (m_selMarker < (int)sfCue->markers.size()) {
                const int anchorListId = sfCue->markers[m_selMarker].anchorMarkerListId;
                if (anchorListId >= 0) {
                    for (int li = 0; li < (int)m_model->sf.cueLists.size(); ++li) {
                        if (m_model->sf.cueLists[li].numericId == anchorListId) {
                            anchorListIdx = li; break;
                        }
                    }
                }
            }
        }
        for (int j = 0; j < m_comboMarkerAnchor->count(); ++j) {
            const QPoint p = m_comboMarkerAnchor->itemData(j).toPoint();
            if (p.x() == anchorListIdx && p.y() == anchorCueIdx) {
                m_comboMarkerAnchor->setCurrentIndex(j);
                break;
            }
        }
    }
    m_loading = wasLoading;
}

// ── slot implementations ───────────────────────────────────────────────────

void InspectorWidget::onBasicChanged() {
    if (m_loading || m_cueIdx < 0) return;
    const mcp::Cue* c = m_model->cues().cueAt(m_cueIdx);
    if (!c) return;
    m_model->pushUndo();

    ShowHelpers::setCueNumberChecked(*m_model, m_cueIdx,
                                     m_editNum->text().toStdString());
    m_model->cues().setCueName(m_cueIdx, m_editName->text().toStdString());
    m_model->cues().setCuePreWait(m_cueIdx, m_spinPreWait->value());
    m_model->cues().setCueGoQuantize(m_cueIdx, m_comboGoQuantize->currentIndex());
    m_model->cues().setCueAutoContinue(m_cueIdx, m_chkAutoCont->isChecked());
    m_model->cues().setCueAutoFollow(m_cueIdx, m_chkAutoFollow->isChecked());
    if (c->type == mcp::CueType::Devamp) {
        m_model->cues().setCueDevampMode(m_cueIdx, m_comboDevampMode->currentIndex());
        m_model->cues().setCueDevampPreVamp(m_cueIdx, m_chkDevampPreVamp->isChecked());
    }
    if (c->type == mcp::CueType::Arm)
        m_model->cues().setCueArmStartTime(m_cueIdx, m_spinArmStart->value());
    if (c->type == mcp::CueType::Marker) {
        // Target: stored as item data (flat index within the target list)
        const int ti = m_comboMarkerTarget->currentData().toInt();
        m_model->cues().setCueTarget(m_cueIdx, ti);
        // Marker index: stored as item data
        const int mi = m_comboMarkerMkIdx->currentData().toInt();
        m_model->cues().setCueMarkerIndex(m_cueIdx, mi);
    }

    ShowHelpers::syncSfFromCues(*m_model);

    // Write targetListId back — syncSfFromCues overwrites target fields but doesn't preserve it
    if (c->type == mcp::CueType::Marker && m_comboMarkerTargetList) {
        const int targetListId = m_comboMarkerTargetList->currentData().toInt();
        if (auto* sfCue = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), m_cueIdx))
            sfCue->targetListId = targetListId;
    }

    emit cueEdited();
}

void InspectorWidget::onMasterFaderChanged(float dB) {
    if (m_loading || m_cueIdx < 0) return;
    m_model->cues().setCueLevel(m_cueIdx, static_cast<double>(dB));
    ShowHelpers::syncSfFromCues(*m_model);
    emit cueEdited();
}

void InspectorWidget::onLevelFaderChanged(int outCh, float dB) {
    if (m_loading || m_cueIdx < 0) return;
    m_model->cues().setCueOutLevel(m_cueIdx, outCh, dB);
    ShowHelpers::syncSfFromCues(*m_model);
    emit cueEdited();
}

void InspectorWidget::onTrimFaderChanged(float dB) {
    if (m_loading || m_cueIdx < 0) return;
    m_model->cues().setCueTrim(m_cueIdx, static_cast<double>(dB));
    ShowHelpers::syncSfFromCues(*m_model);
    emit cueEdited();
}

void InspectorWidget::onFadeMasterTargetChanged(float dB) {
    if (m_loading || m_cueIdx < 0) return;
    const mcp::Cue* c = m_model->cues().cueAt(m_cueIdx);
    if (!c || !c->fadeData) return;
    m_model->cues().setCueFadeMasterTarget(m_cueIdx, c->fadeData->masterLevel.enabled, dB);
    ShowHelpers::syncSfFromCues(*m_model);
    emit cueEdited();
}

void InspectorWidget::onFadeOutTargetChanged(int outCh, float dB) {
    if (m_loading || m_cueIdx < 0) return;
    const mcp::Cue* c = m_model->cues().cueAt(m_cueIdx);
    if (!c || !c->fadeData) return;
    bool en = (outCh < (int)c->fadeData->outLevels.size())
        ? c->fadeData->outLevels[outCh].enabled : false;
    m_model->cues().setCueFadeOutTarget(m_cueIdx, outCh, en, dB);
    ShowHelpers::syncSfFromCues(*m_model);
    emit cueEdited();
}

// ── Network tab ────────────────────────────────────────────────────────────

// ── Script tab ─────────────────────────────────────────────────────────────

void InspectorWidget::buildScriptTab() {
    m_scriptPage = new QWidget;
    auto* lay = new QVBoxLayout(m_scriptPage);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(0);

    m_editScript = new ScriptEditorWidget;
    lay->addWidget(m_editScript, 1);

    m_tabs->addTab(m_scriptPage, "Script");

    connect(m_editScript, &ScriptEditorWidget::codeChanged, this, [this](const QString& code) {
        if (m_loading || m_cueIdx < 0) return;
        m_model->cues().setCueScriptletCode(m_cueIdx, code.toStdString());
        m_model->scriptletErrorCues.erase(m_cueIdx);
        m_model->scriptletErrors.erase(m_cueIdx);
        m_editScript->clearErrorLines();
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });

    connect(m_model, &AppModel::cueListChanged, this, [this]() {
        if (m_cueIdx >= 0) refreshScriptErrors();
    });
}

void InspectorWidget::loadScript() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;
    if (!c) return;
    const QString cueNum = QString::fromStdString(c->cueNumber);
    m_loading = true;
    m_editScript->setContext("cue_" + cueNum);
    m_editScript->setCode(QString::fromStdString(c->scriptletCode));
    m_loading = false;
    refreshScriptErrors();
}

void InspectorWidget::refreshScriptErrors() {
    if (!m_editScript || m_cueIdx < 0) return;
    const auto it = m_model->scriptletErrors.find(m_cueIdx);
    if (it == m_model->scriptletErrors.end()) {
        m_editScript->clearErrorLines();
        return;
    }
    static const QRegularExpression lineRe(R"(line (\d+))");
    const QString errStr = QString::fromStdString(it->second);
    int errorLine = -1;
    auto mi = lineRe.globalMatch(errStr);
    while (mi.hasNext())
        errorLine = mi.next().captured(1).toInt();  // take last (deepest frame)
    if (errorLine > 0)
        m_editScript->markErrorLine(errorLine);
    else
        m_editScript->clearErrorLines();
}

void InspectorWidget::buildSnapshotTab() {
    m_snapshotPage = new QWidget;
    auto* form = new QFormLayout(m_snapshotPage);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);

    m_comboSnapshot = new QComboBox;
    m_comboSnapshot->setToolTip("Snapshot to recall when this cue fires");
    form->addRow(tr("Recall:"), m_comboSnapshot);

    m_tabs->addTab(m_snapshotPage, tr("Snapshot"));

    connect(m_comboSnapshot, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (m_loading || m_cueIdx < 0) return;
        const int snapId = (idx > 0) ? m_comboSnapshot->itemData(idx).toInt() : -1;
        m_model->cues().setCueSnapshotId(m_cueIdx, snapId);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
}

void InspectorWidget::loadSnapshotTab() {
    if (m_cueIdx < 0) return;
    const auto* c = m_model->cues().cueAt(m_cueIdx);
    if (!c) return;

    m_loading = true;
    m_comboSnapshot->clear();
    m_comboSnapshot->addItem(tr("(none)"), -1);

    const auto& sl = m_model->sf.snapshotList;
    int selectIdx = 0;
    for (int i = 0; i < static_cast<int>(sl.snapshots.size()); ++i) {
        const auto& slot = sl.snapshots[static_cast<size_t>(i)];
        if (!slot) continue;
        const QString label = QString("[%1] %2").arg(i + 1)
                              .arg(QString::fromStdString(slot->name));
        m_comboSnapshot->addItem(label, slot->id);
        if (slot->id == c->snapshotId)
            selectIdx = m_comboSnapshot->count() - 1;
    }
    m_comboSnapshot->setCurrentIndex(selectIdx);
    m_loading = false;
}

double InspectorWidget::currentAutomationParamValue(const std::string& path) const {
    const auto& as = m_model->sf.audioSetup;
    // Parse channel index from "/mixer/{ch}/..."
    int ch = -1;
    if (path.size() > 7 && path.compare(0, 7, "/mixer/") == 0) {
        const size_t pos   = 7;
        const size_t slash = path.find('/', pos);
        try { ch = std::stoi(path.substr(pos, slash == std::string::npos
                                              ? std::string::npos : slash - pos)); }
        catch (...) {}
    }
    if (ch < 0 || ch >= (int)as.channels.size()) return 0.0;
    const auto& chan = as.channels[static_cast<size_t>(ch)];
    // Send params must be checked first — their paths also contain /mute etc.
    if (path.find("/send/") != std::string::npos) {
        const std::string sendMid = "/send/";
        const auto it = path.find(sendMid);
        if (it != std::string::npos) {
            const std::string rest = path.substr(it + sendMid.size());
            const auto sl = rest.find('/');
            if (sl != std::string::npos) {
                int slot = -1;
                try { slot = std::stoi(rest.substr(0, sl)); } catch (...) {}
                const std::string param = rest.substr(sl + 1);
                if (slot >= 0 && slot < (int)chan.sends.size()) {
                    const auto& s = chan.sends[static_cast<size_t>(slot)];
                    if (param == "level") return s.levelDb;
                    if (param == "mute")  return s.muted ? 1.0 : 0.0;
                    if (param == "panL")  return s.panL;
                    if (param == "panR")  return s.panR;
                }
            }
        }
        return 0.0;
    }
    if (path.find("/fader")    != std::string::npos) return chan.masterGainDb;
    if (path.find("/mute")     != std::string::npos) return chan.mute     ? 1.0 : 0.0;
    if (path.find("/polarity") != std::string::npos) return chan.phaseInvert ? 1.0 : 0.0;
    if (path.find("/crosspoint/") != std::string::npos) {
        const std::string mid = "/crosspoint/";
        const auto it = path.find(mid);
        int out = -1;
        try { out = std::stoi(path.substr(it + mid.size())); } catch (...) {}
        if (out >= 0) {
            for (const auto& xe : as.xpEntries)
                if (xe.ch == ch && xe.out == out) return xe.db;
            return (ch == out) ? 0.0 : -144.0;  // default diagonal / off
        }
    }
    return 0.0;
}

// ---------------------------------------------------------------------------
// Automation tab — breadcrumb path bar

void InspectorWidget::rebuildAutoPathBar(const QString& path) {

    const auto& as = m_model->sf.audioSetup;
    const int   nCh = static_cast<int>(as.channels.size());

    // Clear the bar's existing widgets.
    auto* barLayout = qobject_cast<QHBoxLayout*>(m_autoPathBar->layout());
    while (QLayoutItem* it = barLayout->takeAt(0)) {
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
    }

    // Parse path into segments; remap slave-channel paths to show through master.
    QStringList segments;
    if (!path.isEmpty()) {
        QString p = path;
        if (p.startsWith('/')) p = p.mid(1);
        segments = p.split('/');
    }
    if (segments.size() >= 3 && segments[0] == QLatin1String("mixer")) {
        bool ok; const int pathCh = segments[1].toInt(&ok);
        if (ok && pathCh > 0 && pathCh < nCh
            && as.channels[static_cast<size_t>(pathCh - 1)].linkedStereo) {
            const QString slaveFull = "/mixer/" + segments[1] + "/" + segments[2];
            segments[1] = QString::number(pathCh - 1);
            segments[2] = slaveFull;
        }
    }

    if (segments.isEmpty()) {
        auto* placeholder = new QLabel(tr("(no parameter)"), m_autoPathBar);
        placeholder->setStyleSheet("color: #777; font-style: italic;");
        barLayout->addWidget(placeholder);
        barLayout->addStretch();
        return;
    }

    // Create one flat-styled button per segment with "›" separators.
    const QStringList segsCopy = segments;
    for (int i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            auto* sep = new QLabel(QStringLiteral("›"), m_autoPathBar);
            sep->setStyleSheet("color: #888; margin: 0 1px;");
            barLayout->addWidget(sep);
        }
        const QString label = pathSegLabel(as, segments[i], i,
                                           i > 0 ? segments[i - 1] : QString());
        auto* btn = new QPushButton(label, m_autoPathBar);
        btn->setFlat(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(
            "QPushButton { padding: 1px 5px; border: none; border-radius: 3px; }"
            "QPushButton:hover { background: rgba(255,255,255,0.13); }");

        const int levelCopy = i;
        connect(btn, &QPushButton::clicked, this, [this, levelCopy, segsCopy]() {
            if (m_loading || m_cueIdx < 0) return;
            // Compute the prefix that leads to this level.
            QString pfx;
            for (int j = 0; j < levelCopy; ++j) {
                const QString& d = segsCopy[j];
                pfx = d.startsWith('/') ? d.mid(1) : (pfx.isEmpty() ? d : pfx + "/" + d);
            }
            const auto pitems = pathItemsAtLevel(m_model, pfx);
            if (pitems.isEmpty()) return;

            QMenu menu(this);
            for (const auto& pi : pitems) {
                auto* act = menu.addAction(pi.label);
                act->setCheckable(true);
                act->setChecked(pi.data == segsCopy[levelCopy]);
                const QString piData  = pi.data;
                const bool    piTerm  = pi.terminal;
                connect(act, &QAction::triggered, this,
                        [this, levelCopy, segsCopy, piData, piTerm]() {
                    // Keep levels 0..levelCopy-1, replace levelCopy with new choice,
                    // then auto-complete remaining levels by always picking first item.
                    QStringList newSegs = segsCopy.mid(0, levelCopy);
                    newSegs << piData;
                    if (!piTerm) {
                        QString pfx2;
                        for (const auto& d : newSegs)
                            pfx2 = d.startsWith('/') ? d.mid(1)
                                                     : (pfx2.isEmpty() ? d : pfx2 + "/" + d);
                        for (int guard = 0; guard < 6; ++guard) {
                            const auto next = pathItemsAtLevel(m_model, pfx2);
                            if (next.isEmpty()) break;
                            const auto& first = next[0];
                            newSegs << first.data;
                            pfx2 = first.data.startsWith('/') ? first.data.mid(1)
                                 : (pfx2.isEmpty() ? first.data : pfx2 + "/" + first.data);
                            if (first.terminal) break;
                        }
                    }
                    QString newPath;
                    for (const auto& d : newSegs)
                        newPath = d.startsWith('/') ? d
                                : (newPath.isEmpty() ? "/" + d : newPath + "/" + d);
                    commitAutoPath(newPath);
                });
            }
            if (auto* src = qobject_cast<QPushButton*>(sender()))
                menu.exec(src->mapToGlobal(QPoint(0, src->height())));
        });
        barLayout->addWidget(btn);
    }
    barLayout->addStretch();
}

// Apply per-parameter range/domain to the automation view for the given path.
static void applyAutoParamMeta(AutomationView* view, AppModel* model, const std::string& path) {
    const auto mode = mcp::Cue::automationParamMode(path);
    view->setParamMode(mode);

    if (path.find("/plugin/") == std::string::npos) {
        view->resetParamRange();
        // Non-plugin paths
        if (path.find("/fader") != std::string::npos
            || (path.find("/send/") != std::string::npos && path.find("/level") != std::string::npos))
            view->setParamDomain(mcp::AutoParam::Domain::FaderTaper);
        else if (path.find("/crosspoint") != std::string::npos)
            view->setParamDomain(mcp::AutoParam::Domain::DB);
        else
            view->setParamDomain(mcp::AutoParam::Domain::Linear);
        return;
    }

    // Parse /mixer/{ch}/plugin/{slot}/{paramId}
    const size_t mixerOff = path.find("/mixer/");
    if (mixerOff == std::string::npos) { view->resetParamRange(); return; }
    const size_t sl1 = path.find('/', mixerOff + 7);
    if (sl1 == std::string::npos) { view->resetParamRange(); return; }
    int ch = -1;
    try { ch = std::stoi(path.substr(mixerOff + 7, sl1 - (mixerOff + 7))); } catch (...) {}
    const size_t plugOff = path.find("/plugin/", sl1);
    if (plugOff == std::string::npos || ch < 0) { view->resetParamRange(); return; }
    const std::string rest = path.substr(plugOff + 8);
    const size_t sl2 = rest.find('/');
    if (sl2 == std::string::npos) { view->resetParamRange(); return; }
    int slot = -1;
    try { slot = std::stoi(rest.substr(0, sl2)); } catch (...) {}
    const std::string paramId = rest.substr(sl2 + 1);
    if (slot < 0 || paramId.empty()) { view->resetParamRange(); return; }

    auto wrapper = model->channelPlugin(ch, slot);
    if (!wrapper || !wrapper->getProcessor()) { view->resetParamRange(); return; }

    for (const auto& info : wrapper->getProcessor()->getParameters()) {
        if (info.id != paramId) continue;
        view->setParamRange(static_cast<double>(info.minValue),
                            static_cast<double>(info.maxValue),
                            QString::fromStdString(info.unit));
        view->setParamDomain(info.domain);
        return;
    }
    view->resetParamRange();
}

void InspectorWidget::commitAutoPath(const QString& path) {
    if (m_cueIdx < 0) return;
    const std::string pathStr = path.toStdString();
    m_model->cues().setCueAutomationPath(m_cueIdx, pathStr);
    rebuildAutoPathBar(path);
    applyAutoParamMeta(m_automationView, m_model, pathStr);
    const double curVal = currentAutomationParamValue(pathStr);
    const double dur    = m_spinAutoDuration->value();
    const std::vector<mcp::Cue::AutomationPoint> flat{{0.0, curVal}, {dur, curVal}};
    m_model->cues().setCueAutomationCurve(m_cueIdx, flat);
    m_automationView->setPoints(flat);
    ShowHelpers::syncSfFromCues(*m_model);
    emit cueEdited();
}

void InspectorWidget::openAutoParamPicker() {
    if (m_cueIdx < 0) return;
    const auto* c = m_model->cues().cueAt(m_cueIdx);
    const QString curPath = c ? QString::fromStdString(c->automationPath) : QString();
    AutoParamPickerDialog dlg(m_model, curPath, this);
    if (dlg.exec() == QDialog::Accepted) {
        const QString newPath = dlg.selectedPath();
        if (!newPath.isEmpty() && newPath != curPath)
            commitAutoPath(newPath);
    }
}

void InspectorWidget::buildAutomationTab() {
    m_automationPage = new QWidget;
    auto* vlay = new QVBoxLayout(m_automationPage);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(6);

    auto* form = new QFormLayout;
    form->setSpacing(6);

    // Path bar row: breadcrumb frame on the left, browse button on the right.
    m_autoPathWidget = new QWidget;
    auto* pathRowLayout = new QHBoxLayout(m_autoPathWidget);
    pathRowLayout->setContentsMargins(0, 0, 0, 0);
    pathRowLayout->setSpacing(4);

    m_autoPathBar = new QFrame;
    m_autoPathBar->setFrameShape(QFrame::StyledPanel);
    m_autoPathBar->setFrameShadow(QFrame::Sunken);
    m_autoPathBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* barLayout = new QHBoxLayout(m_autoPathBar);
    barLayout->setContentsMargins(4, 2, 4, 2);
    barLayout->setSpacing(2);
    pathRowLayout->addWidget(m_autoPathBar, 1);

    auto* searchBtn = new QPushButton(tr("…"), m_autoPathWidget);
    searchBtn->setFixedWidth(28);
    searchBtn->setToolTip(tr("Browse parameters…"));
    connect(searchBtn, &QPushButton::clicked, this, &InspectorWidget::openAutoParamPicker);
    pathRowLayout->addWidget(searchBtn);

    form->addRow(tr("Parameter:"), m_autoPathWidget);

    m_spinAutoDuration = new QDoubleSpinBox;
    m_spinAutoDuration->setRange(0.01, 3600.0);
    m_spinAutoDuration->setDecimals(2);
    m_spinAutoDuration->setSuffix(" s");
    form->addRow(tr("Duration:"), m_spinAutoDuration);

    // Quantize row — visible only when an MC is attached
    m_autoQuantizeRow = new QWidget;
    {
        auto* qlay = new QHBoxLayout(m_autoQuantizeRow);
        qlay->setContentsMargins(0, 0, 0, 0);
        qlay->setSpacing(4);
        m_comboAutoQuantize = new QComboBox;
        m_comboAutoQuantize->addItems({tr("None"), tr("Bar"), tr("Beat")});
        qlay->addWidget(m_comboAutoQuantize);
        qlay->addStretch();
    }
    m_autoQuantizeRow->hide();
    form->addRow(tr("Quantize:"), m_autoQuantizeRow);

    vlay->addLayout(form);

    m_automationView = new AutomationView;
    m_automationView->setMinimumHeight(140);
    vlay->addWidget(m_automationView, 1);

    auto* helpLbl = new QLabel(tr("Left-click: add point  ·  Drag: move  ·  Right-click: delete"));
    helpLbl->setStyleSheet("color: #666; font-size: 10px;");
    vlay->addWidget(helpLbl);

    m_tabs->addTab(m_automationPage, tr("Automation"));

    connect(m_spinAutoDuration, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
        if (m_loading || m_cueIdx < 0) return;
        m_model->cues().setCueAutomationDuration(m_cueIdx, v);
        m_automationView->setDuration(v);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });

    connect(m_automationView, &AutomationView::curveChanged,
            this, [this](const std::vector<mcp::Cue::AutomationPoint>& pts) {
        if (m_loading || m_cueIdx < 0) return;
        m_model->cues().setCueAutomationCurve(m_cueIdx, pts);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });

    connect(m_comboAutoQuantize, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int mode) {
        if (m_loading) return;
        m_automationView->setQuantize(mode);
    });
}

void InspectorWidget::loadAutomationTab() {
    if (m_cueIdx < 0) return;
    const auto* c = m_model->cues().cueAt(m_cueIdx);
    if (!c) return;

    m_loading = true;


    const QString curPath = QString::fromStdString(c->automationPath);
    rebuildAutoPathBar(curPath);

    m_spinAutoDuration->setValue(c->automationDuration);

    applyAutoParamMeta(m_automationView, m_model, c->automationPath);
    m_automationView->setDuration(c->automationDuration);

    // Wire up MC for bar/beat grid
    const mcp::MusicContext* mc = m_model->cues().musicContextOf(m_cueIdx);
    m_automationView->setMusicContext(mc);
    m_automationView->setQuantize(m_comboAutoQuantize->currentIndex());
    m_autoQuantizeRow->setVisible(mc != nullptr);

    m_loading = false;

    // If path is empty (new cue), auto-commit the first available path.
    if (c->automationPath.empty()) {
        QString firstPath;
        QString pfx;
        for (int guard = 0; guard < 8; ++guard) {
            const auto items = pathItemsAtLevel(m_model, pfx);
            if (items.isEmpty()) break;
            const auto& first = items[0];
            pfx = first.data.startsWith('/') ? first.data.mid(1)
                : (pfx.isEmpty() ? first.data : pfx + "/" + first.data);
            firstPath = "/" + pfx;
            if (first.terminal) break;
        }
        if (!firstPath.isEmpty()) commitAutoPath(firstPath);
        return;
    }

    // Seed flat curve if missing.
    if (c->automationCurve.empty()) {
        const double curVal = currentAutomationParamValue(c->automationPath);
        const double dur    = c->automationDuration;
        const std::vector<mcp::Cue::AutomationPoint> flat{{0.0, curVal}, {dur, curVal}};
        m_model->cues().setCueAutomationCurve(m_cueIdx, flat);
        ShowHelpers::syncSfFromCues(*m_model);
        m_automationView->setPoints(flat);
    } else {
        m_automationView->setPoints(c->automationCurve);
    }
    // setPoints() calls ensureHandles() which may add default handles not yet in the engine.
    // Save the augmented point list back so handles persist across saves.
    {
        const auto& viewPts = m_automationView->points();
        if (viewPts.size() != c->automationCurve.size()) {
            m_model->cues().setCueAutomationCurve(m_cueIdx, viewPts);
            ShowHelpers::syncSfFromCues(*m_model);
        }
    }
}

void InspectorWidget::buildNetworkTab() {
    m_networkPage = new QWidget;
    auto* form = new QFormLayout(m_networkPage);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);

    m_comboPatch = new QComboBox;
    m_comboPatch->setToolTip("Network output patch to send to");
    form->addRow("Destination:", m_comboPatch);

    m_editNetCmd = new QPlainTextEdit;
    m_editNetCmd->setPlaceholderText("OSC:  /address arg1 arg2\nText: any text");
    m_editNetCmd->setMinimumHeight(80);
    m_editNetCmd->setStyleSheet(
        "QPlainTextEdit { background:#1e1e1e; color:#ddd; border:1px solid #444; "
        "  border-radius:3px; font-family:monospace; font-size:12px; }");
    form->addRow("Command:", m_editNetCmd);

    m_tabs->addTab(m_networkPage, "Network");

    connect(m_comboPatch, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (m_loading || m_cueIdx < 0) return;
        m_model->cues().setCueNetworkPatch(m_cueIdx, idx - 1);  // -1 = "(none)" item
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
    connect(m_editNetCmd, &QPlainTextEdit::textChanged, this, [this]() {
        if (m_loading || m_cueIdx < 0) return;
        m_model->cues().setCueNetworkCommand(m_cueIdx, m_editNetCmd->toPlainText().toStdString());
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
}

void InspectorWidget::loadNetwork() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;
    if (!c) return;

    // Rebuild patch combo from current network setup
    m_comboPatch->blockSignals(true);
    m_comboPatch->clear();
    m_comboPatch->addItem("(none)");
    const int numPatches = m_model->cues().networkPatchCount();
    for (int i = 0; i < numPatches; ++i)
        m_comboPatch->addItem(QString::fromStdString(m_model->cues().networkPatchName(i)));
    // Select current patch (+1 because item 0 is "(none)")
    const int patchIdx = c->networkPatchIdx;
    m_comboPatch->setCurrentIndex(patchIdx + 1);
    m_comboPatch->blockSignals(false);

    m_editNetCmd->blockSignals(true);
    m_editNetCmd->setPlainText(QString::fromStdString(c->networkCommand));
    m_editNetCmd->blockSignals(false);
}

// ── MIDI tab ───────────────────────────────────────────────────────────────

void InspectorWidget::buildMidiTab() {
    m_midiPage = new QWidget;
    auto* form = new QFormLayout(m_midiPage);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);

    // Patch
    m_comboMidiPatch = new QComboBox;
    form->addRow("Patch:", m_comboMidiPatch);

    // Message type
    m_comboMidiType = new QComboBox;
    m_comboMidiType->addItem("Note On",         "note_on");
    m_comboMidiType->addItem("Note Off",        "note_off");
    m_comboMidiType->addItem("Program Change",  "program_change");
    m_comboMidiType->addItem("Control Change",  "control_change");
    m_comboMidiType->addItem("Pitchbend",       "pitchbend");
    form->addRow("Type:", m_comboMidiType);

    // Channel (always visible)
    m_spinMidiCh = new QSpinBox;
    m_spinMidiCh->setRange(1, 16);
    form->addRow("Channel:", m_spinMidiCh);

    // Note (Note On/Off)
    m_spinMidiNote = new QSpinBox;
    m_spinMidiNote->setRange(0, 127);
    m_lblMidiNote = new QLabel("Note:");
    form->addRow(m_lblMidiNote, m_spinMidiNote);

    // Velocity (Note On/Off)
    m_spinMidiVel = new QSpinBox;
    m_spinMidiVel->setRange(0, 127);
    m_lblMidiVel = new QLabel("Velocity:");
    form->addRow(m_lblMidiVel, m_spinMidiVel);

    // Program (Program Change)
    m_spinMidiProg = new QSpinBox;
    m_spinMidiProg->setRange(0, 127);
    m_lblMidiProg = new QLabel("Program:");
    form->addRow(m_lblMidiProg, m_spinMidiProg);

    // Controller number (Control Change)
    m_spinMidiCC = new QSpinBox;
    m_spinMidiCC->setRange(0, 127);
    m_lblMidiCC = new QLabel("Controller:");
    form->addRow(m_lblMidiCC, m_spinMidiCC);

    // Controller value (Control Change)
    m_spinMidiCCVal = new QSpinBox;
    m_spinMidiCCVal->setRange(0, 127);
    m_lblMidiCCVal = new QLabel("Value:");
    form->addRow(m_lblMidiCCVal, m_spinMidiCCVal);

    // Pitchbend value
    m_spinMidiBend = new QSpinBox;
    m_spinMidiBend->setRange(-8192, 8191);
    m_lblMidiBend = new QLabel("Bend:");
    form->addRow(m_lblMidiBend, m_spinMidiBend);

    m_tabs->addTab(m_midiPage, "MIDI");

    // Save helpers
    auto saveMidi = [this]() {
        if (m_loading || m_cueIdx < 0) return;
        const QString typeKey = m_comboMidiType->currentData().toString();
        const int ch   = m_spinMidiCh->value();
        int data1 = 0, data2 = 0;
        if (typeKey == "note_on" || typeKey == "note_off") {
            data1 = m_spinMidiNote->value();
            data2 = m_spinMidiVel->value();
        } else if (typeKey == "program_change") {
            data1 = m_spinMidiProg->value();
        } else if (typeKey == "control_change") {
            data1 = m_spinMidiCC->value();
            data2 = m_spinMidiCCVal->value();
        } else if (typeKey == "pitchbend") {
            data1 = m_spinMidiBend->value();
        }
        m_model->cues().setCueMidiMessage(m_cueIdx, typeKey.toStdString(), ch, data1, data2);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    };

    connect(m_comboMidiPatch, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (m_loading || m_cueIdx < 0) return;
        m_model->cues().setCueMidiPatch(m_cueIdx, idx - 1);  // -1 = "(none)" item
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
    connect(m_comboMidiType, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, saveMidi](int) {
        updateMidiFields();
        saveMidi();
    });
    connect(m_spinMidiCh,    QOverload<int>::of(&QSpinBox::valueChanged), this, saveMidi);
    connect(m_spinMidiNote,  QOverload<int>::of(&QSpinBox::valueChanged), this, saveMidi);
    connect(m_spinMidiVel,   QOverload<int>::of(&QSpinBox::valueChanged), this, saveMidi);
    connect(m_spinMidiProg,  QOverload<int>::of(&QSpinBox::valueChanged), this, saveMidi);
    connect(m_spinMidiCC,    QOverload<int>::of(&QSpinBox::valueChanged), this, saveMidi);
    connect(m_spinMidiCCVal, QOverload<int>::of(&QSpinBox::valueChanged), this, saveMidi);
    connect(m_spinMidiBend,  QOverload<int>::of(&QSpinBox::valueChanged), this, saveMidi);
}

void InspectorWidget::updateMidiFields() {
    const QString t = m_comboMidiType->currentData().toString();
    const bool isNote = (t == "note_on" || t == "note_off");
    const bool isProg = (t == "program_change");
    const bool isCC   = (t == "control_change");
    const bool isBend = (t == "pitchbend");

    m_lblMidiNote->setVisible(isNote);  m_spinMidiNote->setVisible(isNote);
    m_lblMidiVel->setVisible(isNote);   m_spinMidiVel->setVisible(isNote);
    m_lblMidiProg->setVisible(isProg);  m_spinMidiProg->setVisible(isProg);
    m_lblMidiCC->setVisible(isCC);      m_spinMidiCC->setVisible(isCC);
    m_lblMidiCCVal->setVisible(isCC);   m_spinMidiCCVal->setVisible(isCC);
    m_lblMidiBend->setVisible(isBend);  m_spinMidiBend->setVisible(isBend);
}

void InspectorWidget::loadMidi() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;
    if (!c) return;

    m_loading = true;

    // Rebuild patch combo
    m_comboMidiPatch->blockSignals(true);
    m_comboMidiPatch->clear();
    m_comboMidiPatch->addItem("(none)");
    const int numPatches = m_model->cues().midiPatchCount();
    for (int i = 0; i < numPatches; ++i)
        m_comboMidiPatch->addItem(QString::fromStdString(m_model->cues().midiPatchName(i)));
    m_comboMidiPatch->setCurrentIndex(c->midiPatchIdx + 1);
    m_comboMidiPatch->blockSignals(false);

    // Message type
    m_comboMidiType->blockSignals(true);
    {
        const QString key = QString::fromStdString(c->midiMessageType.empty()
                                                    ? "note_on" : c->midiMessageType);
        for (int i = 0; i < m_comboMidiType->count(); ++i) {
            if (m_comboMidiType->itemData(i).toString() == key) {
                m_comboMidiType->setCurrentIndex(i);
                break;
            }
        }
    }
    m_comboMidiType->blockSignals(false);

    m_spinMidiCh->setValue(c->midiChannel);

    // Data fields
    const QString t = m_comboMidiType->currentData().toString();
    if (t == "note_on" || t == "note_off") {
        m_spinMidiNote->setValue(c->midiData1);
        m_spinMidiVel->setValue(c->midiData2);
    } else if (t == "program_change") {
        m_spinMidiProg->setValue(c->midiData1);
    } else if (t == "control_change") {
        m_spinMidiCC->setValue(c->midiData1);
        m_spinMidiCCVal->setValue(c->midiData2);
    } else if (t == "pitchbend") {
        m_spinMidiBend->setValue(c->midiData1);
    }

    m_loading = false;
    updateMidiFields();
}

// ── Timecode tab ───────────────────────────────────────────────────────────

void InspectorWidget::buildTimecodeTab() {
    m_timecodePage = new QWidget;
    auto* form = new QFormLayout(m_timecodePage);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);

    // TC type (LTC / MTC)
    m_comboTcType = new QComboBox;
    m_comboTcType->addItem("LTC (Linear Timecode — audio output)", "ltc");
    m_comboTcType->addItem("MTC (MIDI Timecode — MIDI output)",    "mtc");
    form->addRow("Type:", m_comboTcType);

    // FPS
    m_comboTcFps = new QComboBox;
    m_comboTcFps->addItem("24 fps",           "24fps");
    m_comboTcFps->addItem("25 fps",           "25fps");
    m_comboTcFps->addItem("30 fps non-drop",  "30fps_nd");
    m_comboTcFps->addItem("30 fps drop",      "30fps_df");
    m_comboTcFps->addItem("23.976 fps",       "23.976fps");
    m_comboTcFps->addItem("24.975 fps",       "24.975fps");
    m_comboTcFps->addItem("29.97 fps ND",     "29.97fps_nd");
    m_comboTcFps->addItem("29.97 fps DF",     "29.97fps_df");
    form->addRow("FPS:", m_comboTcFps);

    // Start TC
    m_editTcStart = new QLineEdit;
    m_editTcStart->setPlaceholderText("hh:mm:ss:ff");
    form->addRow("Start TC:", m_editTcStart);

    // End TC
    m_editTcEnd = new QLineEdit;
    m_editTcEnd->setPlaceholderText("hh:mm:ss:ff");
    form->addRow("End TC:", m_editTcEnd);

    // Duration (read-only display)
    m_lblTcDuration = new QLabel("—");
    form->addRow("Duration:", m_lblTcDuration);

    // LTC output channel row
    {
        m_ltcRow = new QWidget;
        auto* hlay = new QHBoxLayout(m_ltcRow);
        hlay->setContentsMargins(0, 0, 0, 0);
        m_lblLtcCh = new QLabel("Output Channel:");
        m_spinLtcCh = new QSpinBox;
        m_spinLtcCh->setRange(0, 63);
        m_spinLtcCh->setToolTip("0-based physical output channel for the LTC signal");
        hlay->addWidget(m_lblLtcCh);
        hlay->addWidget(m_spinLtcCh);
        hlay->addStretch();
        form->addRow(m_ltcRow);
    }

    // MTC MIDI patch row
    {
        m_mtcRow = new QWidget;
        auto* hlay = new QHBoxLayout(m_mtcRow);
        hlay->setContentsMargins(0, 0, 0, 0);
        m_lblMtcPatch = new QLabel("MIDI Patch:");
        m_comboMtcPatch = new QComboBox;
        hlay->addWidget(m_lblMtcPatch);
        hlay->addWidget(m_comboMtcPatch, 1);
        form->addRow(m_mtcRow);
    }

    m_tabs->addTab(m_timecodePage, "Timecode");

    // Save helper
    auto saveTimecode = [this]() {
        if (m_loading || m_cueIdx < 0) return;
        const QString tcTypeKey = m_comboTcType->currentData().toString();
        const QString tcFpsKey  = m_comboTcFps->currentData().toString();

        mcp::TcFps fps = mcp::TcFps::Fps25;
        mcp::tcFpsFromString(tcFpsKey.toStdString(), fps);

        mcp::TcPoint startTC, endTC;
        mcp::tcFromString(m_editTcStart->text().toStdString(), startTC);
        mcp::tcFromString(m_editTcEnd->text().toStdString(),   endTC);

        m_model->cues().setCueTcType (m_cueIdx, tcTypeKey.toStdString());
        m_model->cues().setCueTcFps  (m_cueIdx, fps);
        m_model->cues().setCueTcStart(m_cueIdx, startTC);
        m_model->cues().setCueTcEnd  (m_cueIdx, endTC);
        m_model->cues().setCueTcLtcChannel(m_cueIdx, m_spinLtcCh->value());
        // MTC patch: combo index 0 = "(none)" → -1
        m_model->cues().setCueTcMidiPatch(m_cueIdx, m_comboMtcPatch->currentIndex() - 1);

        // Update duration display
        if (startTC < endTC) {
            const int64_t dFrames = mcp::tcToFrames(endTC, fps) - mcp::tcToFrames(startTC, fps);
            const mcp::TcRate r = mcp::tcRateFor(fps);
            const double secs = static_cast<double>(dFrames) * r.rateDen / (r.nomFPS * r.rateNum);
            const int hh = static_cast<int>(secs / 3600);
            const int mm = static_cast<int>(secs / 60) % 60;
            const int ss = static_cast<int>(secs) % 60;
            m_lblTcDuration->setText(QString("%1h %2m %3s")
                .arg(hh).arg(mm, 2, 10, QChar('0')).arg(ss, 2, 10, QChar('0')));
        } else {
            m_lblTcDuration->setText("—");
        }

        updateTimecodeFields();
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    };

    connect(m_comboTcType, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, saveTimecode](int) { saveTimecode(); });
    connect(m_comboTcFps,  QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, saveTimecode](int) { saveTimecode(); });
    connect(m_editTcStart, &QLineEdit::editingFinished, this, saveTimecode);
    connect(m_editTcEnd,   &QLineEdit::editingFinished, this, saveTimecode);
    connect(m_spinLtcCh,   QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, saveTimecode](int) { saveTimecode(); });
    connect(m_comboMtcPatch, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, saveTimecode](int) { saveTimecode(); });
}

void InspectorWidget::updateTimecodeFields() {
    const bool isLtc = (m_comboTcType->currentData().toString() == "ltc");
    m_ltcRow->setVisible(isLtc);
    m_mtcRow->setVisible(!isLtc);
}

void InspectorWidget::loadTimecode() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues().cueAt(m_cueIdx) : nullptr;
    if (!c) return;

    m_loading = true;

    // Type
    m_comboTcType->blockSignals(true);
    {
        const QString key = QString::fromStdString(c->tcType.empty() ? "ltc" : c->tcType);
        for (int i = 0; i < m_comboTcType->count(); ++i) {
            if (m_comboTcType->itemData(i).toString() == key) {
                m_comboTcType->setCurrentIndex(i); break;
            }
        }
    }
    m_comboTcType->blockSignals(false);

    // FPS
    m_comboTcFps->blockSignals(true);
    {
        const QString key = QString::fromStdString(mcp::tcFpsToString(c->tcFps));
        for (int i = 0; i < m_comboTcFps->count(); ++i) {
            if (m_comboTcFps->itemData(i).toString() == key) {
                m_comboTcFps->setCurrentIndex(i); break;
            }
        }
    }
    m_comboTcFps->blockSignals(false);

    // TC points
    m_editTcStart->setText(QString::fromStdString(mcp::tcToString(c->tcStartTC)));
    m_editTcEnd->setText  (QString::fromStdString(mcp::tcToString(c->tcEndTC)));

    // Duration display
    if (c->tcStartTC < c->tcEndTC) {
        const int64_t dFrames = mcp::tcToFrames(c->tcEndTC, c->tcFps)
                              - mcp::tcToFrames(c->tcStartTC, c->tcFps);
        const mcp::TcRate r = mcp::tcRateFor(c->tcFps);
        const double secs = static_cast<double>(dFrames) * r.rateDen / (r.nomFPS * r.rateNum);
        const int hh = static_cast<int>(secs / 3600);
        const int mm = static_cast<int>(secs / 60) % 60;
        const int ss = static_cast<int>(secs) % 60;
        m_lblTcDuration->setText(QString("%1h %2m %3s")
            .arg(hh).arg(mm, 2, 10, QChar('0')).arg(ss, 2, 10, QChar('0')));
    } else {
        m_lblTcDuration->setText("—");
    }

    // LTC channel
    m_spinLtcCh->setValue(c->tcLtcChannel);

    // MTC MIDI patch combo
    m_comboMtcPatch->blockSignals(true);
    m_comboMtcPatch->clear();
    m_comboMtcPatch->addItem("(none)");
    const int numPatches = m_model->cues().midiPatchCount();
    for (int i = 0; i < numPatches; ++i)
        m_comboMtcPatch->addItem(QString::fromStdString(m_model->cues().midiPatchName(i)));
    m_comboMtcPatch->setCurrentIndex(c->tcMidiPatchIdx + 1);
    m_comboMtcPatch->blockSignals(false);

    m_loading = false;
    updateTimecodeFields();
}

// ── Triggers tab ──────────────────────────────────────────────────────────

void InspectorWidget::buildTriggersTab() {
    // m_triggersPage is the direct child of QTabWidget (needed for indexOf).
    // All visible content lives inside a QScrollArea so the tab scrolls instead
    // of compressing when the inspector panel is shorter than the content.
    m_triggersPage = new QWidget;
    auto* pageLayout = new QVBoxLayout(m_triggersPage);
    pageLayout->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(m_triggersPage);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    pageLayout->addWidget(scroll);

    auto* inner = new QWidget;
    auto* outer = new QVBoxLayout(inner);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(12);

    // ---- Hotkey ----
    auto* hkBox = new QGroupBox("Hotkey Trigger");
    auto* hkLay = new QHBoxLayout(hkBox);
    m_chkHotkeyEnable = new QCheckBox;
    m_editHotkey = new QLineEdit;
    m_editHotkey->setPlaceholderText("Click to capture key…");
    m_editHotkey->setReadOnly(true);
    m_editHotkey->setMinimumWidth(160);
    m_btnHotkeyClear = new QPushButton("✕");
    m_btnHotkeyClear->setFixedWidth(28);
    hkLay->addWidget(m_chkHotkeyEnable);
    hkLay->addWidget(m_editHotkey, 1);
    hkLay->addWidget(m_btnHotkeyClear);
    outer->addWidget(hkBox);

    m_editHotkey->installEventFilter(this);  // FocusIn/Out sets capture mode, KeyPress captures

    connect(m_btnHotkeyClear, &QPushButton::clicked, this, [this]() {
        m_editHotkey->clear();
        m_hotkeyCapturing = false;
        saveTriggers();
    });
    connect(m_chkHotkeyEnable, &QCheckBox::toggled, this, [this](bool) {
        if (!m_loading) saveTriggers();
    });

    // ---- MIDI Trigger ----
    auto* midiBox = new QGroupBox("MIDI Trigger");
    auto* midiGrid = new QGridLayout(midiBox);
    midiGrid->setSpacing(4);
    m_chkMidiTrigEnable = new QCheckBox;
    m_comboMidiTrigType = new QComboBox;
    for (const char* s : {"Note On","Note Off","Control Change","Program Change","Pitch Bend"})
        m_comboMidiTrigType->addItem(s);
    m_spinMidiTrigCh = new QSpinBox; m_spinMidiTrigCh->setRange(0,16);
    m_spinMidiTrigCh->setSpecialValueText("Any");
    m_spinMidiTrigD1 = new QSpinBox; m_spinMidiTrigD1->setRange(0,127);
    m_spinMidiTrigD2 = new QSpinBox; m_spinMidiTrigD2->setRange(-1,127);
    m_spinMidiTrigD2->setSpecialValueText("Any");
    m_btnMidiCapture = new QPushButton("Capture");

    midiGrid->addWidget(m_chkMidiTrigEnable, 0, 0);
    midiGrid->addWidget(m_comboMidiTrigType, 0, 1);
    midiGrid->addWidget(new QLabel("Ch:"), 0, 2);
    midiGrid->addWidget(m_spinMidiTrigCh,   0, 3);
    midiGrid->addWidget(new QLabel("D1:"),  0, 4);
    midiGrid->addWidget(m_spinMidiTrigD1,   0, 5);
    midiGrid->addWidget(new QLabel("D2:"),  0, 6);
    midiGrid->addWidget(m_spinMidiTrigD2,   0, 7);
    midiGrid->addWidget(m_btnMidiCapture,   0, 8);
    outer->addWidget(midiBox);

    connect(m_chkMidiTrigEnable, &QCheckBox::toggled, this, [this](bool) { if (!m_loading) saveTriggers(); });
    connect(m_comboMidiTrigType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { if (!m_loading) saveTriggers(); });
    connect(m_spinMidiTrigCh, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { if (!m_loading) saveTriggers(); });
    connect(m_spinMidiTrigD1, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { if (!m_loading) saveTriggers(); });
    connect(m_spinMidiTrigD2, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { if (!m_loading) saveTriggers(); });

    connect(m_btnMidiCapture, &QPushButton::clicked, this, [this]() {
        m_btnMidiCapture->setText("Listening…");
        m_btnMidiCapture->setEnabled(false);
        m_model->midiIn.armCapture([this](mcp::MidiMsgType t, int ch, int d1, int d2) {
            m_loading = true;
            m_chkMidiTrigEnable->setChecked(true);
            m_comboMidiTrigType->setCurrentIndex(static_cast<int>(t));
            m_spinMidiTrigCh->setValue(ch);
            m_spinMidiTrigD1->setValue(d1);
            // NoteOn/Off: ignore velocity (use Any = -1) so any velocity triggers
            const int d2save = (t == mcp::MidiMsgType::NoteOn || t == mcp::MidiMsgType::NoteOff)
                               ? -1 : d2;
            m_spinMidiTrigD2->setValue(d2save);
            m_loading = false;
            m_btnMidiCapture->setText("Capture");
            m_btnMidiCapture->setEnabled(true);
            saveTriggers();
        });
    });

    // ---- OSC Trigger ----
    auto* oscBox = new QGroupBox("OSC Trigger");
    auto* oscLay = new QHBoxLayout(oscBox);
    m_chkOscTrigEnable = new QCheckBox;
    m_editOscPath = new QLineEdit;
    m_editOscPath->setPlaceholderText("/my/custom/path");
    oscLay->addWidget(m_chkOscTrigEnable);
    oscLay->addWidget(m_editOscPath, 1);
    outer->addWidget(oscBox);

    connect(m_chkOscTrigEnable, &QCheckBox::toggled, this, [this](bool) { if (!m_loading) saveTriggers(); });
    connect(m_editOscPath, &QLineEdit::editingFinished, this, [this]() { if (!m_loading) saveTriggers(); });

    outer->addStretch();
    scroll->setWidget(inner);
    m_tabs->addTab(m_triggersPage, "Triggers");
}

void InspectorWidget::loadTriggers() {
    if (m_cueIdx < 0 || !m_model) return;
    const auto* sfCue = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), m_cueIdx);
    if (!sfCue) return;
    const auto& tr = sfCue->triggers;

    m_loading = true;
    m_chkHotkeyEnable->setChecked(tr.hotkey.enabled);
    m_editHotkey->setText(QString::fromStdString(tr.hotkey.keyString));

    m_chkMidiTrigEnable->setChecked(tr.midi.enabled);
    m_comboMidiTrigType->setCurrentIndex(static_cast<int>(tr.midi.type));
    m_spinMidiTrigCh->setValue(tr.midi.channel);
    m_spinMidiTrigD1->setValue(tr.midi.data1);
    m_spinMidiTrigD2->setValue(tr.midi.data2);

    m_chkOscTrigEnable->setChecked(tr.osc.enabled);
    m_editOscPath->setText(QString::fromStdString(tr.osc.path));
    m_loading = false;
}

void InspectorWidget::saveTriggers() {
    if (m_cueIdx < 0 || !m_model) return;
    auto* sfCue = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), m_cueIdx);
    if (!sfCue) return;

    auto& tr = sfCue->triggers;

    tr.hotkey.enabled   = m_chkHotkeyEnable->isChecked();
    tr.hotkey.keyString = m_editHotkey->text().toStdString();

    tr.midi.enabled = m_chkMidiTrigEnable->isChecked();
    tr.midi.type    = static_cast<mcp::MidiMsgType>(m_comboMidiTrigType->currentIndex());
    tr.midi.channel = m_spinMidiTrigCh->value();
    tr.midi.data1   = m_spinMidiTrigD1->value();
    tr.midi.data2   = m_spinMidiTrigD2->value();

    const std::string oscPath = m_editOscPath->text().toStdString();
    // Reject system vocabulary paths
    if (!oscPath.empty() && mcp::isSystemOscPath(oscPath)) {
        m_editOscPath->setStyleSheet("QLineEdit { border: 1px solid #cc3333; }");
        return;
    }
    m_editOscPath->setStyleSheet("");
    tr.osc.enabled = m_chkOscTrigEnable->isChecked();
    tr.osc.path    = oscPath;

    m_model->dirty = true;
    emit m_model->dirtyChanged(true);
}

// Hotkey capture: FocusIn enters capture mode, KeyPress records the key.
bool InspectorWidget::eventFilter(QObject* obj, QEvent* ev) {
    if (obj != m_editHotkey)
        return QWidget::eventFilter(obj, ev);

    if (ev->type() == QEvent::FocusIn) {
        m_hotkeyCapturing = true;
        m_editHotkey->setPlaceholderText("Press any key…");
        return QWidget::eventFilter(obj, ev);
    }
    if (ev->type() == QEvent::FocusOut) {
        if (m_hotkeyCapturing) {
            m_hotkeyCapturing = false;
            m_editHotkey->setPlaceholderText("Click to capture key…");
        }
        return QWidget::eventFilter(obj, ev);
    }
    if (ev->type() == QEvent::KeyPress && m_hotkeyCapturing) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Shift || ke->key() == Qt::Key_Control ||
            ke->key() == Qt::Key_Alt   || ke->key() == Qt::Key_Meta)
            return QWidget::eventFilter(obj, ev);
        const QKeySequence ks(ke->keyCombination());
        m_editHotkey->setText(ks.toString());
        m_hotkeyCapturing = false;
        m_editHotkey->setPlaceholderText("Click to capture key…");
        saveTriggers();
        return true;
    }
    return QWidget::eventFilter(obj, ev);
}
