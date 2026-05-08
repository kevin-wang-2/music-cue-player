#include "ShowInfoDialog.h"
#include "AppModel.h"

#include "engine/Cue.h"
#include "engine/CueList.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QString>

// ---------------------------------------------------------------------------

static const char* kBg        = "#0d0d0d";
static const char* kLabelColor = "#666";
static const char* kValueColor = "#eeeeee";
static const char* kMemoColor  = "#88ccff";
static const char* kEmptyColor = "#444";

ShowInfoDialog::ShowInfoDialog(AppModel* model, QWidget* parent)
    : QDialog(parent, Qt::Window | Qt::WindowStaysOnTopHint)
    , m_model(model)
{
    setWindowTitle("Show Information");
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumWidth(480);

    setStyleSheet(QString("QDialog { background:%1; }").arg(kBg));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 24);
    root->setSpacing(20);

    auto makeSection = [&](const QString& labelText, QLabel*& valueOut) {
        auto* lbl = new QLabel(labelText, this);
        lbl->setStyleSheet(QString("color:%1; font-size:11px; font-family:monospace;"
                                   " letter-spacing:1px; text-transform:uppercase;")
                               .arg(kLabelColor));
        root->addWidget(lbl);

        valueOut = makeValueLabel();
        root->addWidget(valueOut);
    };

    makeSection("CURRENT CUE", m_lblCurrentCue);
    makeSection("NEXT CUE",    m_lblNextCue);

    // Memo section
    auto* memoLabel = new QLabel("CURRENT MEMO", this);
    memoLabel->setStyleSheet(QString("color:%1; font-size:11px; font-family:monospace;"
                                     " letter-spacing:1px;").arg(kLabelColor));
    root->addWidget(memoLabel);

    m_lblMemo = new QLabel("—", this);
    m_lblMemo->setWordWrap(true);
    m_lblMemo->setStyleSheet(
        QString("color:%1; font-size:22px; font-family:monospace;"
                " background:#111; border:1px solid #222; border-radius:4px;"
                " padding:10px 14px;").arg(kMemoColor));
    m_lblMemo->setMinimumHeight(60);
    m_lblMemo->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    root->addWidget(m_lblMemo);

    connect(model, &AppModel::showInfoChanged, this, &ShowInfoDialog::refresh);

    refresh();
}

// static
QLabel* ShowInfoDialog::makeValueLabel() {
    auto* lbl = new QLabel("—");
    lbl->setStyleSheet(
        QString("color:%1; font-size:26px; font-weight:bold; font-family:monospace;"
                " background:#111; border:1px solid #222; border-radius:4px;"
                " padding:10px 14px;").arg(kValueColor));
    lbl->setMinimumHeight(60);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return lbl;
}

// static
QString ShowInfoDialog::cueText(const QString& number, const QString& name) {
    if (number.isEmpty() && name.isEmpty()) return {};
    if (number.isEmpty()) return name;
    if (name.isEmpty())   return number;
    return number + "  /  " + name;
}

void ShowInfoDialog::refresh() {
    // Current Cue
    {
        const int idx = m_model->m_currentCueIdx;
        const mcp::Cue* c = (idx >= 0) ? m_model->cues.cueAt(idx) : nullptr;
        if (c) {
            const QString text = cueText(
                QString::fromStdString(c->cueNumber),
                QString::fromStdString(c->name));
            m_lblCurrentCue->setText(text.isEmpty() ? "—" : text);
            m_lblCurrentCue->setStyleSheet(
                QString("color:%1; font-size:26px; font-weight:bold; font-family:monospace;"
                        " background:#111; border:1px solid #222; border-radius:4px;"
                        " padding:10px 14px;").arg(kValueColor));
        } else {
            m_lblCurrentCue->setText("—");
            m_lblCurrentCue->setStyleSheet(
                QString("color:%1; font-size:26px; font-weight:bold; font-family:monospace;"
                        " background:#111; border:1px solid #222; border-radius:4px;"
                        " padding:10px 14px;").arg(kEmptyColor));
        }
    }

    // Next Cue (= selected cue)
    {
        const int idx = m_model->cues.selectedIndex();
        const mcp::Cue* c = (idx >= 0) ? m_model->cues.cueAt(idx) : nullptr;
        if (c) {
            const QString text = cueText(
                QString::fromStdString(c->cueNumber),
                QString::fromStdString(c->name));
            m_lblNextCue->setText(text.isEmpty() ? "—" : text);
            m_lblNextCue->setStyleSheet(
                QString("color:%1; font-size:26px; font-weight:bold; font-family:monospace;"
                        " background:#111; border:1px solid #222; border-radius:4px;"
                        " padding:10px 14px;").arg(kValueColor));
        } else {
            m_lblNextCue->setText("—");
            m_lblNextCue->setStyleSheet(
                QString("color:%1; font-size:26px; font-weight:bold; font-family:monospace;"
                        " background:#111; border:1px solid #222; border-radius:4px;"
                        " padding:10px 14px;").arg(kEmptyColor));
        }
    }

    // Current Memo
    {
        const QString memo = QString::fromStdString(m_model->m_currentMemo);
        if (memo.isEmpty()) {
            m_lblMemo->setText("—");
            m_lblMemo->setStyleSheet(
                QString("color:%1; font-size:22px; font-family:monospace;"
                        " background:#111; border:1px solid #222; border-radius:4px;"
                        " padding:10px 14px;").arg(kEmptyColor));
        } else {
            m_lblMemo->setText(memo);
            m_lblMemo->setStyleSheet(
                QString("color:%1; font-size:22px; font-family:monospace;"
                        " background:#111; border:1px solid #222; border-radius:4px;"
                        " padding:10px 14px;").arg(kMemoColor));
        }
    }
}
