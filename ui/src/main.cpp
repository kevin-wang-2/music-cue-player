#include "tinyfiledialogs.h"

#include "engine/AudioEngine.h"
#include "engine/CueList.h"
#include "engine/Scheduler.h"
#include "engine/ShowFile.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static float dBToLinear(double dB) {
    return (dB <= -144.0) ? 0.0f : static_cast<float>(std::pow(10.0, dB / 20.0));
}

static std::string fmtDuration(double s) {
    if (s <= 0.0) return "--:--";
    const int m  = static_cast<int>(s) / 60;
    const int sc = static_cast<int>(s) % 60;
    const int ms = static_cast<int>((s - std::floor(s)) * 10);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d.%d", m, sc, ms);
    return buf;
}

static std::string fmtPlayhead(double s) {
    if (s <= 0.0) return "0:00.0";
    return fmtDuration(s);
}

static ImVec4 typeColor(const std::string& t) {
    if (t == "audio") return ImVec4(0.40f, 0.70f, 1.00f, 1.0f);
    if (t == "start") return ImVec4(0.40f, 0.90f, 0.50f, 1.0f);
    if (t == "stop")  return ImVec4(1.00f, 0.45f, 0.45f, 1.0f);
    if (t == "fade")  return ImVec4(0.95f, 0.70f, 0.20f, 1.0f);
    if (t == "arm")   return ImVec4(0.75f, 0.40f, 0.95f, 1.0f);
    return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

struct AddDialog {
    bool  open          = false;
    int   type          = 0;     // 0=audio 1=start 2=stop 3=fade
    char  path[512]     = {};
    char  name[256]     = {};
    float preWait       = 0.0f;
    char  targetCueNum[32] = {}; // start/stop target
    // Fade-specific fields
    char  fadeTargetNum[32] = {};
    float fadeTargetValue   = 0.0f;
    float fadeDuration      = 3.0f;  // = cd.duration for fade cues
    int   fadeCurve         = 0;     // 0=Linear 1=EqualPower
    bool  fadeStopWhenDone  = false;
    std::string errorMsg;
};

struct App {
    // Engine stack — order matters (scheduler needs engine, cues needs both)
    mcp::AudioEngine engine;
    mcp::Scheduler   scheduler{engine};
    mcp::CueList     cues{engine, scheduler};

    mcp::ShowFile    sf;
    std::string      showPath;    // empty = unsaved
    std::string      baseDir;
    bool             dirty = false;
    bool             engineOk = false;
    std::string      engineError;

    // Inspector editing buffers (for selected cue)
    char     insp_cueNum[32]  = {};
    char     insp_name[256]   = {};
    float    insp_preWait     = 0.0f;
    float    insp_startTime   = 0.0f;
    float    insp_duration    = 0.0f;
    float    insp_level       = 0.0f;   // dB
    float    insp_trim        = 0.0f;   // dB
    bool     insp_ac          = false;
    bool     insp_af          = false;
    int      insp_lastSel     = -2;  // force refresh on first frame

    // Fader label inline editing (double-click dB text to type value)
    int  insp_faderEdit  = -1;    // 0=level, 1=trim; -1=none
    bool insp_faderFocus = false;
    char insp_faderBuf[16] = {};

    // Fade cue inspector buffers
    float insp_fadeTargetValue  = 0.0f;
    int   insp_fadeCurve        = 0;     // 0=Linear 1=EqualPower
    bool  insp_fadeStopWhenDone = false;
    char  insp_fadeTargetNum[32] = {};

    AddDialog addDlg;
    bool wantOpenDlg   = false;
    bool wantSaveAsDlg = false;
    bool wantTitleDlg  = false;

    // Inline table-cell editing
    int  inline_editRow  = -1;   // row being edited; -1 = none
    int  inline_editCol  = -1;   // 0 = cue#, 2 = name, 4 = duration
    char inline_editBuf[256] = {};
    bool inline_needFocus = false;

    // Drop notification: shown briefly after files are dragged in
    std::string dropMsg;
    double      dropMsgExpiry = 0.0;  // glfwGetTime() deadline
};

// ---------------------------------------------------------------------------
// Cue-number helpers
// ---------------------------------------------------------------------------

// Return the first "1", "2", "3", ... string not already used as a cue number.
static std::string nextCueNumber(const mcp::ShowFile& sf) {
    const auto& cues = sf.cueLists.empty() ? std::vector<mcp::ShowFile::CueData>{} : sf.cueLists[0].cues;
    int n = 1;
    while (true) {
        std::string candidate = std::to_string(n);
        bool taken = false;
        for (const auto& cd : cues)
            if (cd.cueNumber == candidate) { taken = true; break; }
        if (!taken) return candidate;
        ++n;
    }
}

// Find array index of the cue whose cueNumber == num; returns -1 if not found or num is empty.
static int findCueByNumber(const mcp::ShowFile& sf, const std::string& num) {
    if (num.empty() || sf.cueLists.empty()) return -1;
    for (int i = 0; i < (int)sf.cueLists[0].cues.size(); ++i)
        if (sf.cueLists[0].cues[i].cueNumber == num) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Show file helpers
// ---------------------------------------------------------------------------

static bool rebuildCueList(App& app, std::string& err) {
    app.cues.clear();
    if (app.sf.cueLists.empty()) return true;
    auto& cds  = app.sf.cueLists[0].cues;
    const auto base = std::filesystem::path(app.baseDir);

    // Pass 1: resolve targetCueNumber → target index for start/stop/fade/arm cues.
    // Empty cue numbers are intentionally preserved — they are valid but cannot be referenced.
    for (auto& cd : cds) {
        if ((cd.type == "start" || cd.type == "stop" || cd.type == "fade" || cd.type == "arm") &&
            !cd.targetCueNumber.empty()) {
            int idx = findCueByNumber(app.sf, cd.targetCueNumber);
            if (idx >= 0) cd.target = idx;
        }
    }

    // Pass 2: build CueList.
    for (int i = 0; i < (int)cds.size(); ++i) {
        const auto& cd = cds[i];
        bool ok = true;
        if (cd.type == "audio") {
            auto p = std::filesystem::path(cd.path);
            if (p.is_relative()) p = base / p;
            ok = app.cues.addCue(p.string(), cd.name, cd.preWait);
        } else if (cd.type == "start") {
            app.cues.addStartCue(cd.target, cd.name, cd.preWait);
        } else if (cd.type == "stop") {
            app.cues.addStopCue(cd.target, cd.name, cd.preWait);
        } else if (cd.type == "fade") {
            const mcp::FadeData::Curve curve = (cd.fadeCurve == "equalpower")
                ? mcp::FadeData::Curve::EqualPower : mcp::FadeData::Curve::Linear;
            app.cues.addFadeCue(cd.target, cd.targetCueNumber, cd.fadeParameter,
                                cd.fadeTargetValue, curve, cd.fadeStopWhenDone,
                                cd.name, cd.preWait);
        } else if (cd.type == "arm") {
            app.cues.addArmCue(cd.target, cd.name, cd.preWait);
        }
        if (ok) {
            app.cues.setCueCueNumber  (i, cd.cueNumber);
            app.cues.setCueStartTime  (i, cd.startTime);
            app.cues.setCueDuration   (i, cd.duration);
            app.cues.setCueLevel      (i, cd.level);
            app.cues.setCueTrim       (i, cd.trim);
            app.cues.setCueAutoContinue(i, cd.autoContinue);
            app.cues.setCueAutoFollow  (i, cd.autoFollow);
        } else if (cd.type == "audio") {
            err += "  [" + cd.cueNumber + "] failed: " + cd.path + "\n";
        }
    }
    return true;
}

static void saveShow(App& app) {
    if (app.showPath.empty()) return;
    std::string err;
    app.sf.save(app.showPath, err);
    if (err.empty()) app.dirty = false;
}

static void syncSfFromCues(App& app) {
    // Keep ShowFile in sync with whatever CueList holds after live edits.
    // Only called when the UI edits a field.
    if (app.sf.cueLists.empty()) return;
    auto& cds = app.sf.cueLists[0].cues;
    for (int i = 0; i < app.cues.cueCount() && i < (int)cds.size(); ++i) {
        const auto* c = app.cues.cueAt(i);
        if (!c) continue;
        cds[i].cueNumber    = c->cueNumber;
        cds[i].name         = c->name;
        cds[i].preWait      = c->preWaitSeconds;
        cds[i].startTime    = c->startTime;
        cds[i].duration     = c->duration;
        cds[i].level        = c->level;
        cds[i].trim         = c->trim;
        cds[i].autoContinue = c->autoContinue;
        cds[i].autoFollow   = c->autoFollow;
        if (c->type == mcp::CueType::Fade && c->fadeData) {
            const auto& fd      = *c->fadeData;
            cds[i].targetCueNumber  = fd.targetCueNumber;
            cds[i].target           = fd.resolvedTargetIdx;
            cds[i].fadeParameter    = fd.parameter;
            cds[i].fadeTargetValue  = fd.targetValue;
            cds[i].fadeStopWhenDone = fd.stopWhenDone;
            cds[i].fadeCurve        = (fd.curve == mcp::FadeData::Curve::EqualPower)
                                      ? "equalpower" : "linear";
        }
    }
    app.dirty = true;
}

// ---------------------------------------------------------------------------
// Inspector panel
// ---------------------------------------------------------------------------

static void renderInspector(App& app) {
    const int sel = app.cues.selectedIndex();
    const mcp::Cue* c = (sel >= 0 && sel < app.cues.cueCount())
                        ? app.cues.cueAt(sel) : nullptr;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.13f, 0.14f, 1.0f));
    ImGui::BeginChild("inspector", ImVec2(0, 0), ImGuiChildFlags_None);

    ImGui::Spacing();
    ImGui::SetCursorPosX(8);
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.f), "INSPECTOR");
    ImGui::Separator();
    ImGui::Spacing();

    if (!c) {
        ImGui::TextDisabled("  (no cue selected)");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    // Refresh editing buffers when selection changes
    if (app.insp_lastSel != sel) {
        std::strncpy(app.insp_cueNum, c->cueNumber.c_str(), sizeof(app.insp_cueNum) - 1);
        std::strncpy(app.insp_name,   c->name.c_str(),      sizeof(app.insp_name)   - 1);
        app.insp_preWait   = static_cast<float>(c->preWaitSeconds);
        app.insp_startTime = static_cast<float>(c->startTime);
        app.insp_duration  = static_cast<float>(c->duration);
        app.insp_level     = static_cast<float>(c->level);
        app.insp_trim      = static_cast<float>(c->trim);
        app.insp_ac        = c->autoContinue;
        app.insp_af        = c->autoFollow;
        app.insp_faderEdit = -1;  // cancel any ongoing fader label edit
        if (c->type == mcp::CueType::Fade && c->fadeData) {
            const auto& fd = *c->fadeData;
            std::strncpy(app.insp_fadeTargetNum, fd.targetCueNumber.c_str(),
                         sizeof(app.insp_fadeTargetNum) - 1);
            app.insp_fadeTargetValue  = static_cast<float>(fd.targetValue);
            app.insp_fadeCurve        = (fd.curve == mcp::FadeData::Curve::EqualPower) ? 1 : 0;
            app.insp_fadeStopWhenDone = fd.stopWhenDone;
        }
        app.insp_lastSel = sel;
    }

    const char* typeStr = c->type == mcp::CueType::Audio ? "audio"
                        : c->type == mcp::CueType::Start ? "start"
                        : c->type == mcp::CueType::Stop  ? "stop"
                        : c->type == mcp::CueType::Arm   ? "arm"
                        : "fade";

    // Row 1: Q number | Name | type badge
    ImGui::SetCursorPosX(8);
    ImGui::SetNextItemWidth(60);
    ImGui::InputText("##cuenum", app.insp_cueNum, sizeof(app.insp_cueNum));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        app.cues.setCueCueNumber(sel, app.insp_cueNum);
        syncSfFromCues(app);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cue number (user-visible, editable)");

    ImGui::SameLine(0, 8);
    ImGui::SetNextItemWidth(240);
    ImGui::InputText("Name", app.insp_name, sizeof(app.insp_name));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        app.cues.setCueName(sel, app.insp_name);
        syncSfFromCues(app);
    }

    ImGui::SameLine();
    ImGui::TextColored(typeColor(typeStr), "  [%s]", typeStr);

    ImGui::Spacing();
    ImGui::SetCursorPosX(8);
    ImGui::SetNextItemWidth(120);
    if (ImGui::DragFloat("Pre-Wait (s)", &app.insp_preWait, 0.01f, 0.0f, 60.0f, "%.3f")) {
        app.cues.setCuePreWait(sel, app.insp_preWait);
        syncSfFromCues(app);
    }

    // Shared fader constants and helpers — used by Audio (level/trim) and Fade (target).
    constexpr float kFaderW   = 30.0f;
    constexpr float kFaderH   = 90.0f;
    constexpr float kFaderMin = -60.0f;
    constexpr float kFaderMax = 10.0f;

    auto fmtdBLabel = [](float dB, char* buf, int sz) {
        if (dB <= -59.9f) std::snprintf(buf, sz, "-inf");
        else              std::snprintf(buf, sz, "%+.1f", static_cast<double>(dB));
    };

    // dB (clamped to UI floor) → linear gain.  Values at or below kFaderMin → 0.
    auto levelToGain = [&](float dB) -> float {
        return (dB <= kFaderMin) ? 0.0f : dBToLinear(static_cast<double>(dB));
    };

    // Commit a typed dB string to fader mode: 0=level, 1=trim, 2=fadeTarget.
    // Values < kFaderMin round to -inf (stored as kFaderMin).
    auto applyFaderText = [&](int mode) {
        try {
            float v;
            if (std::strncmp(app.insp_faderBuf, "-inf", 4) == 0) v = kFaderMin;
            else v = std::stof(app.insp_faderBuf);
            if (v < kFaderMin) v = kFaderMin;
            v = std::min(kFaderMax, v);
            if (mode == 0) {
                app.insp_level = v;
                app.cues.setCueLevel(sel, v);
                const int slot = app.cues.cueVoiceSlot(sel);
                if (slot >= 0 && app.engine.isVoiceActive(slot))
                    app.engine.setVoiceGain(slot, levelToGain(v + app.insp_trim));
            } else if (mode == 1) {
                app.insp_trim = v;
                app.cues.setCueTrim(sel, v);
                const int slot = app.cues.cueVoiceSlot(sel);
                if (slot >= 0 && app.engine.isVoiceActive(slot))
                    app.engine.setVoiceGain(slot, levelToGain(app.insp_level + v));
            } else {
                app.insp_fadeTargetValue = v;
                app.cues.setCueFadeTargetValue(sel, v);
            }
            syncSfFromCues(app);
        } catch (...) {}
        app.insp_faderEdit = -1;
    };

    // Render a fader group (label above, VSlider, dB text below with double-click edit).
    // editMode: the insp_faderEdit index for this fader (0=level, 1=trim, 2=fadeTarget).
    auto renderFaderGroup = [&](const char* label, const char* sliderId, const char* textId,
                                float& value, int editMode,
                                const char* tooltip) {
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", label);
        if (ImGui::VSliderFloat(sliderId, ImVec2(kFaderW, kFaderH),
                                &value, kFaderMin, kFaderMax, "")) {
            if (editMode == 0) { app.cues.setCueLevel(sel, value); }
            else if (editMode == 1) { app.cues.setCueTrim(sel, value); }
            else                    { app.cues.setCueFadeTargetValue(sel, value); }
            if (editMode <= 1) {
                const int slot = app.cues.cueVoiceSlot(sel);
                if (slot >= 0 && app.engine.isVoiceActive(slot))
                    app.engine.setVoiceGain(slot, levelToGain(app.insp_level + app.insp_trim));
            }
            syncSfFromCues(app);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nDouble-click to reset to 0 dB", tooltip);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            value = 0.0f;
            if (editMode == 0) { app.cues.setCueLevel(sel, 0.0); }
            else if (editMode == 1) { app.cues.setCueTrim(sel, 0.0); }
            else                    { app.cues.setCueFadeTargetValue(sel, 0.0); }
            if (editMode <= 1) {
                const int slot = app.cues.cueVoiceSlot(sel);
                if (slot >= 0 && app.engine.isVoiceActive(slot))
                    app.engine.setVoiceGain(slot, levelToGain(app.insp_level + app.insp_trim));
            }
            syncSfFromCues(app);
        }
        if (app.insp_faderEdit == editMode) {
            ImGui::SetNextItemWidth(kFaderW + 10);
            if (app.insp_faderFocus) { ImGui::SetKeyboardFocusHere(); app.insp_faderFocus = false; }
            ImGui::InputText(textId, app.insp_faderBuf, sizeof(app.insp_faderBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemDeactivated()) applyFaderText(editMode);
        } else {
            char dbBuf[16]; fmtdBLabel(value, dbBuf, sizeof(dbBuf));
            ImGui::TextDisabled("%s dB", dbBuf);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && app.insp_faderEdit < 0) {
                app.insp_faderEdit  = editMode;
                app.insp_faderFocus = true;
                fmtdBLabel(value, app.insp_faderBuf, sizeof(app.insp_faderBuf));
            }
        }
        ImGui::EndGroup();
    };

    if (c->type == mcp::CueType::Audio) {
        ImGui::SameLine(0, 20);
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Start (s)", &app.insp_startTime, 0.01f, 0.0f, 3600.0f, "%.3f")) {
            app.cues.setCueStartTime(sel, app.insp_startTime);
            syncSfFromCues(app);
        }
        ImGui::SameLine(0, 20);
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Duration (s)", &app.insp_duration, 0.01f, 0.0f, 3600.0f, "%.3f")) {
            app.cues.setCueDuration(sel, app.insp_duration);
            syncSfFromCues(app);
        }

        ImGui::Spacing();
        ImGui::SetCursorPosX(8);
        ImGui::TextDisabled("File: %s", c->path.c_str());
        ImGui::SetCursorPosX(8);
        ImGui::TextDisabled("Total: %s", fmtDuration(app.cues.cueTotalSeconds(sel)).c_str());

        // ---- Level & Trim faders ----
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SetCursorPosX(8);
        renderFaderGroup("Level", "##level", "##fe_lvl", app.insp_level, 0,
                         "Output level fader");
        ImGui::SameLine(0, 16);
        renderFaderGroup("Trim",  "##trim",  "##fe_trm", app.insp_trim,  1,
                         "Fine trim on top of level");

    } else if (c->type == mcp::CueType::Fade) {
        // ---- Fade cue parameters ----
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SetCursorPosX(8);

        const auto& fd = c->fadeData;
        if (fd) {
            const int tIdx = fd->resolvedTargetIdx;
            const mcp::Cue* tc = (tIdx >= 0 && tIdx < app.cues.cueCount())
                                  ? app.cues.cueAt(tIdx) : nullptr;
            if (tc)
                ImGui::TextDisabled("Target: Q%s  \"%s\"  param: %s",
                                    tc->cueNumber.c_str(), tc->name.c_str(),
                                    fd->parameter.c_str());
            else
                ImGui::TextDisabled("Target: (unresolved Q%s)  param: %s",
                                    fd->targetCueNumber.c_str(), fd->parameter.c_str());

            ImGui::Spacing();

            // Target value fader
            renderFaderGroup("Target", "##fadetgt", "##fe_ftgt",
                             app.insp_fadeTargetValue, 2, "Fade destination level");

            ImGui::SameLine(0, 20);

            // Duration (= fade length) + Curve stacked vertically
            ImGui::BeginGroup();
            ImGui::TextDisabled("Duration");
            ImGui::SetNextItemWidth(100);
            if (ImGui::DragFloat("##fadedur", &app.insp_duration,
                                 0.1f, 0.01f, 300.0f, "%.2f s")) {
                app.cues.setCueDuration(sel, app.insp_duration);
                syncSfFromCues(app);
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Curve");
            ImGui::SetNextItemWidth(100);
            static const char* curves[] = {"Linear", "EqualPower"};
            if (ImGui::Combo("##fadecurve", &app.insp_fadeCurve, curves, 2)) {
                app.cues.setCueFadeCurve(sel,
                    app.insp_fadeCurve == 1
                        ? mcp::FadeData::Curve::EqualPower
                        : mcp::FadeData::Curve::Linear);
                syncSfFromCues(app);
            }
            ImGui::Spacing();
            if (ImGui::Checkbox("Stop when done", &app.insp_fadeStopWhenDone)) {
                app.cues.setCueFadeStopWhenDone(sel, app.insp_fadeStopWhenDone);
                syncSfFromCues(app);
            }
            ImGui::EndGroup();
        }

    } else {
        // Start / Stop / Arm
        const mcp::Cue* tc = app.cues.cueAt(c->targetIndex);
        ImGui::SameLine(0, 20);
        if (tc)
            ImGui::TextDisabled("→ Q%s  \"%s\"", tc->cueNumber.c_str(), tc->name.c_str());
        else
            ImGui::TextDisabled("→ (unresolved)");
    }

    ImGui::Spacing();
    ImGui::SetCursorPosX(8);
    if (ImGui::Checkbox("Auto-Continue", &app.insp_ac)) {
        app.cues.setCueAutoContinue(sel, app.insp_ac);
        syncSfFromCues(app);
    }
    ImGui::SameLine(0, 24);
    if (ImGui::Checkbox("Auto-Follow", &app.insp_af)) {
        app.cues.setCueAutoFollow(sel, app.insp_af);
        syncSfFromCues(app);
    }

    // Playhead if playing
    if (app.cues.isCuePlaying(sel)) {
        const double pos   = app.cues.cuePlayheadSeconds(sel);
        const double total = app.cues.cueTotalSeconds(sel);
        ImGui::Spacing();
        ImGui::SetCursorPosX(8);
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "  %s / %s",
                           fmtPlayhead(pos).c_str(), fmtDuration(total).c_str());
        if (total > 0.0) {
            float pct = static_cast<float>(pos / total);
            ImGui::SetCursorPosX(8);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8);
            ImGui::ProgressBar(pct, ImVec2(-1, 4), "");
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Add Cue dialog
// ---------------------------------------------------------------------------

static void renderAddDialog(App& app) {
    if (app.addDlg.open) {
        ImGui::OpenPopup("Add Cue");
        app.addDlg.open = false;
    }

    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Add Cue", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    static const char* types[] = {"Audio", "Start", "Stop", "Fade", "Arm"};
    ImGui::SetNextItemWidth(100);
    ImGui::Combo("Type", &app.addDlg.type, types, 5);

    if (app.addDlg.type == 0) {
        ImGui::SetNextItemWidth(380);
        ImGui::InputText("File path", app.addDlg.path, sizeof(app.addDlg.path));
        ImGui::TextDisabled("  Tip: path relative to show file, or absolute");
    } else if (app.addDlg.type == 1 || app.addDlg.type == 2 || app.addDlg.type == 4) {
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("Target cue number", app.addDlg.targetCueNum,
                         sizeof(app.addDlg.targetCueNum));
        // Show which cue would be targeted
        if (app.addDlg.targetCueNum[0]) {
            int idx = findCueByNumber(app.sf, app.addDlg.targetCueNum);
            if (idx >= 0) {
                const mcp::Cue* tc = app.cues.cueAt(idx);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "→ \"%s\"",
                                   tc ? tc->name.c_str() : "?");
            } else {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "not found");
            }
        }
        if (app.addDlg.type == 4)
            ImGui::TextDisabled("  Arms the target cue so it fires with no I/O latency.");
    } else {
        // Fade cue fields
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("Target cue number", app.addDlg.fadeTargetNum,
                         sizeof(app.addDlg.fadeTargetNum));
        if (app.addDlg.fadeTargetNum[0]) {
            int idx = findCueByNumber(app.sf, app.addDlg.fadeTargetNum);
            const mcp::Cue* tc = idx >= 0 ? app.cues.cueAt(idx) : nullptr;
            ImGui::SameLine();
            if (tc) ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "→ \"%s\"", tc->name.c_str());
            else    ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "not found");
        }
        ImGui::TextDisabled("  Parameter: level");
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Target (dB)", &app.addDlg.fadeTargetValue, 0.1f, -60.0f, 10.0f, "%.1f");
        ImGui::SameLine(0, 20);
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Duration (s)", &app.addDlg.fadeDuration, 0.1f, 0.1f, 300.0f, "%.1f");
        ImGui::SetNextItemWidth(140);
        static const char* curves[] = {"Linear", "EqualPower"};
        ImGui::Combo("Curve##add", &app.addDlg.fadeCurve, curves, 2);
        ImGui::Checkbox("Stop when done##add", &app.addDlg.fadeStopWhenDone);
    }

    ImGui::SetNextItemWidth(260);
    ImGui::InputText("Name (optional)", app.addDlg.name, sizeof(app.addDlg.name));
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat("Pre-Wait (s)", &app.addDlg.preWait, 0.01f, 0.0f, 60.0f, "%.3f");

    if (!app.addDlg.errorMsg.empty())
        ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "  %s", app.addDlg.errorMsg.c_str());

    ImGui::Spacing();
    if (ImGui::Button("Add", ImVec2(80, 0))) {
        app.addDlg.errorMsg.clear();
        mcp::ShowFile::CueData cd;

        // Auto-assign next cue number for the new cue
        cd.cueNumber = nextCueNumber(app.sf);

        if (app.addDlg.type == 0) {
            cd.type = "audio";
            cd.path = app.addDlg.path;
            cd.name = app.addDlg.name[0] ? app.addDlg.name
                      : std::filesystem::path(cd.path).stem().string();
            cd.preWait = app.addDlg.preWait;

            auto p = std::filesystem::path(cd.path);
            if (!app.baseDir.empty() && p.is_relative())
                p = std::filesystem::path(app.baseDir) / p;

            bool ok = app.cues.addCue(p.string(), cd.name, cd.preWait);
            if (!ok) {
                app.addDlg.errorMsg = "Could not load audio file (check path/format)";
                goto done;
            }
            const int newIdx = app.cues.cueCount() - 1;
            app.cues.setCueCueNumber(newIdx, cd.cueNumber);
        } else if (app.addDlg.type == 1 || app.addDlg.type == 2 || app.addDlg.type == 4) {
            // Resolve target cue number → array index
            const std::string tnum(app.addDlg.targetCueNum);
            int tIdx = tnum.empty() ? -1 : findCueByNumber(app.sf, tnum);
            if (!tnum.empty() && tIdx < 0) {
                app.addDlg.errorMsg = "Cue number \"" + tnum + "\" not found";
                goto done;
            }
            cd.type             = (app.addDlg.type == 1) ? "start"
                                : (app.addDlg.type == 2) ? "stop" : "arm";
            cd.target           = tIdx;
            cd.targetCueNumber  = tnum;
            cd.name             = app.addDlg.name[0] ? app.addDlg.name
                                  : (cd.type + "(Q" + tnum + ")");
            cd.preWait          = app.addDlg.preWait;
            if (cd.type == "start")
                app.cues.addStartCue(cd.target, cd.name, cd.preWait);
            else if (cd.type == "stop")
                app.cues.addStopCue (cd.target, cd.name, cd.preWait);
            else
                app.cues.addArmCue  (cd.target, cd.name, cd.preWait);
            const int newIdx = app.cues.cueCount() - 1;
            app.cues.setCueCueNumber(newIdx, cd.cueNumber);
        } else {
            // Fade cue
            const std::string tnum(app.addDlg.fadeTargetNum);
            int tIdx = tnum.empty() ? -1 : findCueByNumber(app.sf, tnum);
            if (!tnum.empty() && tIdx < 0) {
                app.addDlg.errorMsg = "Cue number \"" + tnum + "\" not found";
                goto done;
            }
            cd.type             = "fade";
            cd.target           = tIdx;
            cd.targetCueNumber  = tnum;
            cd.fadeParameter    = "level";
            cd.fadeTargetValue  = app.addDlg.fadeTargetValue;
            cd.duration         = app.addDlg.fadeDuration;
            cd.fadeStopWhenDone = app.addDlg.fadeStopWhenDone;
            cd.fadeCurve        = (app.addDlg.fadeCurve == 1) ? "equalpower" : "linear";
            cd.preWait          = app.addDlg.preWait;
            cd.name             = app.addDlg.name[0] ? app.addDlg.name
                                  : ("fade(Q" + tnum + ")");

            const mcp::FadeData::Curve curve = (app.addDlg.fadeCurve == 1)
                ? mcp::FadeData::Curve::EqualPower : mcp::FadeData::Curve::Linear;
            app.cues.addFadeCue(tIdx, tnum, "level",
                                app.addDlg.fadeTargetValue, curve,
                                app.addDlg.fadeStopWhenDone, cd.name, cd.preWait);
            const int newIdx = app.cues.cueCount() - 1;
            app.cues.setCueCueNumber(newIdx, cd.cueNumber);
            app.cues.setCueDuration(newIdx, cd.duration);
        }

        if (app.sf.cueLists.empty()) app.sf.cueLists.push_back({"main", "Main", {}});
        app.sf.cueLists[0].cues.push_back(cd);
        app.dirty = true;

        // Reset dialog fields
        std::memset(app.addDlg.path,          0, sizeof(app.addDlg.path));
        std::memset(app.addDlg.name,          0, sizeof(app.addDlg.name));
        std::memset(app.addDlg.targetCueNum,  0, sizeof(app.addDlg.targetCueNum));
        std::memset(app.addDlg.fadeTargetNum, 0, sizeof(app.addDlg.fadeTargetNum));
        app.addDlg.preWait          = 0.0f;
        app.addDlg.fadeTargetValue  = 0.0f;
        app.addDlg.fadeDuration     = 3.0f;
        app.addDlg.fadeCurve        = 0;
        app.addDlg.fadeStopWhenDone = false;
        ImGui::CloseCurrentPopup();
    }
    done:
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        app.addDlg.errorMsg.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Cue table
// ---------------------------------------------------------------------------

static void renderCueTable(App& app) {
    const int selIdx = app.cues.selectedIndex();
    const int n      = app.cues.cueCount();

    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingFixedFit;

    if (!ImGui::BeginTable("cues", 6, tflags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#",        ImGuiTableColumnFlags_WidthFixed,   40);
    ImGui::TableSetupColumn("Type",     ImGuiTableColumnFlags_WidthFixed,   52);
    ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Target",   ImGuiTableColumnFlags_WidthFixed,   90);
    ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed,   70);
    ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthFixed,   90);
    ImGui::TableHeadersRow();

    // Deferred mutations applied after EndTable.
    int reorderSrc = -1, reorderDst = -1;
    int targetSetRow = -1, targetSetSrc = -1;

    for (int i = 0; i < n; ++i) {
        const mcp::Cue* c = app.cues.cueAt(i);
        if (!c) continue;

        const bool playing  = app.cues.isCuePlaying(i);
        const bool pending  = app.cues.isCuePending(i);
        const bool isSel    = (i == selIdx);
        const bool isEditing = (app.inline_editRow == i);

        ImGui::TableNextRow();

        // Pre-fetch column screen-X positions for double-click column detection.
        float colX[6];
        for (int ci = 0; ci < 6; ++ci) {
            ImGui::TableSetColumnIndex(ci);
            colX[ci] = ImGui::GetCursorScreenPos().x;
        }

        if (playing)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(30, 90, 40, 120));
        else if (pending)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(90, 70, 10, 120));

        // ---- Col 0: cue number ------------------------------------------------
        ImGui::TableSetColumnIndex(0);
        if (isEditing && app.inline_editCol == 0) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (app.inline_needFocus) { ImGui::SetKeyboardFocusHere(); app.inline_needFocus = false; }
            ImGui::InputText("##ie0", app.inline_editBuf, sizeof(app.inline_editBuf));
            if (ImGui::IsItemDeactivated()) {
                app.cues.setCueCueNumber(i, app.inline_editBuf);
                syncSfFromCues(app);
                app.inline_editRow = -1;
            }
        } else {
            const char* qn = c->cueNumber.empty() ? "-" : c->cueNumber.c_str();
            if (isSel) ImGui::TextColored(ImVec4(1, 0.85f, 0, 1), "▶ %s", qn);
            else        ImGui::TextDisabled("  %s", qn);
        }

        // ---- Col 1: type badge ------------------------------------------------
        ImGui::TableSetColumnIndex(1);
        const std::string tstr = (c->type == mcp::CueType::Audio) ? "audio"
                                : (c->type == mcp::CueType::Start) ? "start"
                                : (c->type == mcp::CueType::Stop)  ? "stop"
                                : (c->type == mcp::CueType::Arm)   ? "arm"
                                : "fade";
        ImGui::TextColored(typeColor(tstr), "%s", tstr.c_str());

        // ---- Col 2: name — selectable / inline editor / DnD ------------------
        ImGui::TableSetColumnIndex(2);
        if (isEditing && app.inline_editCol == 2) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (app.inline_needFocus) { ImGui::SetKeyboardFocusHere(); app.inline_needFocus = false; }
            ImGui::InputText("##ie2", app.inline_editBuf, sizeof(app.inline_editBuf));
            if (ImGui::IsItemDeactivated()) {
                app.cues.setCueName(i, app.inline_editBuf);
                syncSfFromCues(app);
                app.inline_editRow = -1;
            }
        } else {
            char lbl[64]; std::snprintf(lbl, sizeof(lbl), "##row%d", i);
            if (ImGui::Selectable(lbl, isSel, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 0)))
                app.cues.setSelectedIndex(i);
            ImGui::SetItemAllowOverlap();

            // Double-click → start inline edit for the column under the mouse.
            // Col 3 (Target) and Col 5 (Status) are excluded.
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && app.inline_editRow < 0) {
                app.cues.setSelectedIndex(i);
                const float mx  = ImGui::GetMousePos().x;

                if (mx < colX[1]) {
                    // Col 0: cue number
                    app.inline_editRow = i; app.inline_editCol = 0;
                    std::strncpy(app.inline_editBuf, c->cueNumber.c_str(), sizeof(app.inline_editBuf) - 1);
                } else if (mx < colX[3]) {
                    // Col 1 or 2: edit name (Type is display-only)
                    app.inline_editRow = i; app.inline_editCol = 2;
                    std::strncpy(app.inline_editBuf, c->name.c_str(), sizeof(app.inline_editBuf) - 1);
                } else if (mx >= colX[4] && mx < colX[5] && c->type == mcp::CueType::Audio) {
                    // Col 4: duration trim (audio cues only)
                    app.inline_editRow = i; app.inline_editCol = 4;
                    char tmp[32]; std::snprintf(tmp, sizeof(tmp), "%.3f", c->duration);
                    std::strncpy(app.inline_editBuf, tmp, sizeof(app.inline_editBuf) - 1);
                }
                if (app.inline_editRow == i) {
                    app.inline_editBuf[sizeof(app.inline_editBuf) - 1] = 0;
                    app.inline_needFocus = true;
                }
            }

            // DnD only when not inline-editing this row.
            if (!isEditing) {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload("CUE_IDX", &i, sizeof(int));
                    ImGui::Text("Q%s  %s",
                                c->cueNumber.empty() ? "-" : c->cueNumber.c_str(),
                                c->name.c_str());
                    ImGui::EndDragDropSource();
                }

                // Reorder drop target: draw an insertion line instead of row highlight.
                if (ImGui::BeginDragDropTarget()) {
                    const ImVec2 rMin = ImGui::GetItemRectMin();
                    const ImVec2 rMax = ImGui::GetItemRectMax();
                    const bool   after = ImGui::GetMousePos().y > (rMin.y + rMax.y) * 0.5f;
                    const float  lineY = after ? rMax.y : rMin.y;

                    // AcceptBeforeDelivery: draw line every frame while hovering.
                    // AcceptNoDrawDefaultRect: suppress ImGui's full-row highlight.
                    const ImGuiPayload* p = ImGui::AcceptDragDropPayload("CUE_IDX",
                        ImGuiDragDropFlags_AcceptBeforeDelivery |
                        ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                    if (p) {
                        if (p->IsDelivery()) {
                            reorderSrc = *(const int*)p->Data;
                            reorderDst = after ? i + 1 : i;
                        }
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        dl->AddCircleFilled(ImVec2(rMin.x + 4, lineY), 3.5f, IM_COL32(80, 150, 255, 220));
                        dl->AddLine(ImVec2(rMin.x + 8, lineY), ImVec2(rMax.x, lineY),
                                    IM_COL32(80, 150, 255, 220), 2.0f);
                    }
                    ImGui::EndDragDropTarget();
                }
            }

            ImGui::SameLine();
            ImGui::Text("%s", c->name.c_str());
        }

        // ---- Col 3: target — DnD drop zone for start/stop/fade -------------------
        ImGui::TableSetColumnIndex(3);
        if (c->type != mcp::CueType::Audio) {
            const int tgtIdx = (c->type == mcp::CueType::Fade && c->fadeData)
                               ? c->fadeData->resolvedTargetIdx : c->targetIndex;
            const mcp::Cue* tc = (tgtIdx >= 0 && tgtIdx < n)
                                  ? app.cues.cueAt(tgtIdx) : nullptr;
            const std::string tdisplay = tc
                ? ("Q" + (tc->cueNumber.empty() ? std::to_string(tgtIdx) : tc->cueNumber))
                : "  \xe2\x80\x94";  // — UTF-8 EM DASH

            const float cellW = ImGui::GetContentRegionAvail().x;
            const float cellH = ImGui::GetTextLineHeightWithSpacing();
            char tgtId[32]; std::snprintf(tgtId, sizeof(tgtId), "##tgt%d", i);
            ImGui::InvisibleButton(tgtId, ImVec2(cellW > 4 ? cellW : 60, cellH));

            const ImVec2 tpos = {
                ImGui::GetItemRectMin().x + 2,
                ImGui::GetItemRectMin().y + (cellH - ImGui::GetTextLineHeight()) * 0.5f
            };
            ImGui::GetWindowDrawList()->AddText(
                tpos,
                tc ? ImGui::GetColorU32(ImGuiCol_Text)
                   : ImGui::GetColorU32(ImGuiCol_TextDisabled),
                tdisplay.c_str());

            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("CUE_IDX")) {
                    targetSetRow = i;
                    targetSetSrc = *(const int*)p->Data;
                }
                ImGui::EndDragDropTarget();
            }
        } else {
            ImGui::TextDisabled("  \xe2\x80\x94");
        }

        // ---- Col 4: duration (or inline editor) ------------------------------
        ImGui::TableSetColumnIndex(4);
        if (isEditing && app.inline_editCol == 4) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (app.inline_needFocus) { ImGui::SetKeyboardFocusHere(); app.inline_needFocus = false; }
            ImGui::InputText("##ie4", app.inline_editBuf, sizeof(app.inline_editBuf));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetTooltip("seconds (0 = play to end)");
            if (ImGui::IsItemDeactivated()) {
                try {
                    double d = std::stod(app.inline_editBuf);
                    app.cues.setCueDuration(i, std::max(0.0, d));
                    syncSfFromCues(app);
                } catch (...) {}
                app.inline_editRow = -1;
            }
        } else if (c->type == mcp::CueType::Audio) {
            ImGui::TextDisabled("%s", fmtDuration(app.cues.cueTotalSeconds(i)).c_str());
        } else {
            ImGui::TextDisabled("  \xe2\x80\x94");
        }

        // ---- Col 5: status / playhead progress --------------------------------
        ImGui::TableSetColumnIndex(5);
        if (playing) {
            const double pos   = app.cues.cuePlayheadSeconds(i);
            const double total = app.cues.cueTotalSeconds(i);
            if (total > 0.0) {
                float pct = static_cast<float>(pos / total);
                ImGui::ProgressBar(pct, ImVec2(-1, ImGui::GetTextLineHeight()), "");
            } else {
                ImGui::TextColored(ImVec4(0.3f, 1, 0.4f, 1), "playing");
            }
        } else if (pending) {
            ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), "pending");
        } else if (app.cues.isArmed(i)) {
            ImGui::TextColored(ImVec4(0.9f, 0.55f, 0.1f, 1), "armed");
        } else {
            ImGui::TextDisabled("idle");
        }
    }

    ImGui::EndTable();

    // ---- Apply deferred reorder ---------------------------------------------
    if (reorderSrc >= 0 && reorderDst >= 0 && !app.sf.cueLists.empty()) {
        if (reorderSrc != reorderDst && reorderSrc + 1 != reorderDst) {
            auto& cds = app.sf.cueLists[0].cues;
            mcp::ShowFile::CueData moved = cds[reorderSrc];
            cds.erase(cds.begin() + reorderSrc);
            int dst = reorderDst;
            if (reorderSrc < reorderDst) dst--;
            dst = std::clamp(dst, 0, (int)cds.size());
            cds.insert(cds.begin() + dst, moved);
            std::string err;
            rebuildCueList(app, err);
            app.cues.setSelectedIndex(dst);
            app.insp_lastSel = -2;
            app.inline_editRow = -1;
            app.dirty = true;
        }
    }

    // ---- Apply deferred target-set ------------------------------------------
    if (targetSetRow >= 0 && targetSetSrc >= 0 && !app.sf.cueLists.empty()) {
        auto& cds = app.sf.cueLists[0].cues;
        if (targetSetRow < (int)cds.size()) {
            const mcp::Cue* srcCue = app.cues.cueAt(targetSetSrc);
            const std::string srcNum = srcCue ? srcCue->cueNumber : "";
            cds[targetSetRow].target          = targetSetSrc;
            cds[targetSetRow].targetCueNumber = srcNum;
            std::string err;
            rebuildCueList(app, err);
            app.insp_lastSel = -2;
            app.dirty = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------

static void renderMenuBar(App& app, GLFWwindow* /*win*/) {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Show")) {
            app.cues.panic();
            app.sf       = mcp::ShowFile::empty("Untitled Show");
            app.showPath = "";
            app.baseDir  = "";
            app.dirty    = false;
            std::string err; rebuildCueList(app, err);
            app.insp_lastSel = -2;
        }
        // Set flags here; act on them AFTER EndMainMenuBar (menu popup stack
        // must be closed before calling blocking tinyfd or ImGui::OpenPopup).
        if (ImGui::MenuItem("Open…",    "Cmd+O")) app.wantOpenDlg   = true;
        if (ImGui::MenuItem("Save",     "Cmd+S", false, !app.showPath.empty()))
            saveShow(app);
        if (ImGui::MenuItem("Save As…"))           app.wantSaveAsDlg = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Quit"))
            glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Show")) {
        if (ImGui::MenuItem("Title…")) app.wantTitleDlg = true;
        if (!app.engineOk)
            ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "  ⚠ No audio device");
        ImGui::EndMenu();
    }

    // Right-align status
    {
        const char* marker = app.dirty ? " ●" : "";
        std::string right  = std::string(marker) + "  voices:" +
                             std::to_string(app.engine.activeVoiceCount()) +
                             "  pending:" + std::to_string(app.scheduler.pendingCount());
        float w = ImGui::CalcTextSize(right.c_str()).x + 16;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - w);
        if (app.dirty)
            ImGui::TextColored(ImVec4(1,0.75f,0.2f,1), "%s", right.c_str());
        else
            ImGui::TextDisabled("%s", right.c_str());
    }

    ImGui::EndMainMenuBar();

    // -----------------------------------------------------------------------
    // Deferred actions — executed after the menu popup stack is fully closed.
    // tinyfd calls block the main thread (audio keeps running on its thread).
    // ImGui::OpenPopup must also be called outside any popup context.
    // -----------------------------------------------------------------------

    if (app.wantOpenDlg) {
        app.wantOpenDlg = false;
        const char* path = tinyfd_openFileDialog(
            "Open Show File", app.showPath.c_str(), 0, nullptr, nullptr, 0);
        if (path) {
            std::string err;
            mcp::ShowFile newSf;
            if (newSf.load(path, err)) {
                app.cues.panic();
                app.sf       = std::move(newSf);
                app.showPath = path;
                app.baseDir  = std::filesystem::path(path).parent_path().string();
                app.dirty    = false;
                app.insp_lastSel = -2;
                rebuildCueList(app, err);
            } else {
                tinyfd_messageBox("Open failed", err.c_str(), "ok", "error", 1);
            }
        }
    }

    if (app.wantSaveAsDlg) {
        app.wantSaveAsDlg = false;
        const char* path = tinyfd_saveFileDialog(
            "Save Show As", app.showPath.empty() ? "show.json" : app.showPath.c_str(),
            0, nullptr, nullptr);
        if (path) {
            app.showPath = path;
            app.baseDir  = std::filesystem::path(path).parent_path().string();
            saveShow(app);
        }
    }

    if (app.wantTitleDlg) {
        app.wantTitleDlg = false;
        ImGui::OpenPopup("Edit Show Title");
    }

    // ---- Edit title modal (only one needing a modal — others use tinyfd) ---
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Edit Show Title", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        static char titleBuf[256] = {};
        if (ImGui::IsWindowAppearing())
            std::strncpy(titleBuf, app.sf.show.title.c_str(), sizeof(titleBuf)-1);
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("Title", titleBuf, sizeof(titleBuf));
        if (ImGui::Button("OK", ImVec2(80,0))) {
            app.sf.show.title = titleBuf;
            app.dirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80,0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

static void renderToolbar(App& app) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.70f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.56f, 0.84f, 1.0f));
    if (ImGui::Button("+ Add Cue  ▾"))
        ImGui::OpenPopup("add_cue_popup");
    ImGui::PopStyleColor(2);

    if (ImGui::BeginPopup("add_cue_popup")) {
        if (ImGui::MenuItem("Audio Cue"))  { app.addDlg.type = 0; app.addDlg.open = true; }
        if (ImGui::MenuItem("Start Cue"))  { app.addDlg.type = 1; app.addDlg.open = true; }
        if (ImGui::MenuItem("Stop Cue"))   { app.addDlg.type = 2; app.addDlg.open = true; }
        if (ImGui::MenuItem("Fade Cue"))   { app.addDlg.type = 3; app.addDlg.open = true; }
        if (ImGui::MenuItem("Arm Cue"))    { app.addDlg.type = 4; app.addDlg.open = true; }
        ImGui::EndPopup();
    }

    // Remove selected cue
    ImGui::SameLine();
    const int sel = app.cues.selectedIndex();
    const bool canRemove = (sel >= 0 && sel < app.cues.cueCount());
    if (!canRemove) ImGui::BeginDisabled();
    if (ImGui::Button("Remove Selected")) ImGui::OpenPopup("Confirm Remove");
    if (!canRemove) ImGui::EndDisabled();

    // Confirm dialog
    if (ImGui::BeginPopupModal("Confirm Remove", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove cue [%d]?", sel);
        if (ImGui::Button("Remove", ImVec2(80,0))) {
            // Stop it if running
            app.cues.stop(sel);
            // Remove from ShowFile
            if (!app.sf.cueLists.empty() && sel < (int)app.sf.cueLists[0].cues.size())
                app.sf.cueLists[0].cues.erase(app.sf.cueLists[0].cues.begin() + sel);
            // Rebuild CueList from ShowFile (simplest way to keep indices consistent)
            std::string err;
            rebuildCueList(app, err);
            app.dirty = true;
            app.insp_lastSel = -2;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80,0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Show title + path info on the right
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x -
                         ImGui::CalcTextSize(app.sf.show.title.c_str()).x - 200);
    ImGui::TextDisabled("%s", app.sf.show.title.c_str());
    if (!app.showPath.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  —  %s",
            std::filesystem::path(app.showPath).filename().string().c_str());
    }
}

// ---------------------------------------------------------------------------
// Keyboard handling
// ---------------------------------------------------------------------------

static void handleKeyboard(App& app) {
    // Don't intercept keys while a text field is being edited
    if (ImGui::GetIO().WantTextInput) return;

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        const int64_t origin = app.engine.enginePlayheadFrames();
        app.cues.go(origin);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        app.cues.softPanic(0.5);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
        app.cues.selectNext();
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
        app.cues.selectPrev();
    if (ImGui::IsKeyPressed(ImGuiKey_S, false) && (ImGui::GetIO().KeyMods & ImGuiMod_Super))
        saveShow(app);
    if (ImGui::IsKeyPressed(ImGuiKey_O, false) && (ImGui::GetIO().KeyMods & ImGuiMod_Super))
        app.wantOpenDlg = true;
}

// ---------------------------------------------------------------------------
// GLFW callbacks
// ---------------------------------------------------------------------------

static void glfwErrorCallback(int error, const char* desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, desc);
}

static void dropCallback(GLFWwindow* win, int count, const char** paths) {
    App* app = static_cast<App*>(glfwGetWindowUserPointer(win));
    if (!app) return;

    int added = 0, failed = 0;

    for (int i = 0; i < count; ++i) {
        const std::filesystem::path absPath(paths[i]);

        // Compute path to store in ShowFile: prefer relative to show base dir
        std::string storePath = absPath.string();
        if (!app->baseDir.empty()) {
            std::error_code ec;
            auto rel = std::filesystem::relative(absPath, app->baseDir, ec);
            if (!ec && rel.string().substr(0, 2) != "..")
                storePath = rel.string();
        }

        const std::string name = absPath.stem().string();

        if (!app->cues.addCue(absPath.string(), name)) {
            ++failed;
            continue;
        }

        if (app->sf.cueLists.empty())
            app->sf.cueLists.push_back({"main", "Main", {}});

        mcp::ShowFile::CueData cd;
        cd.type = "audio";
        cd.path = storePath;
        cd.name = name;
        app->sf.cueLists[0].cues.push_back(cd);
        app->dirty = true;
        ++added;
    }

    // Build notification message
    if (added > 0 || failed > 0) {
        std::string msg;
        if (added  > 0) msg += std::to_string(added)  + " cue(s) added";
        if (failed > 0) {
            if (!msg.empty()) msg += "  •  ";
            msg += std::to_string(failed) + " failed (format/rate mismatch?)";
        }
        app->dropMsg       = std::move(msg);
        app->dropMsgExpiry = glfwGetTime() + 3.0;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) { std::fprintf(stderr, "GLFW init failed\n"); return 1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* win = glfwCreateWindow(1024, 720, "MCP — Music Cue Player", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Dark theme, slightly tweaked
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.ItemSpacing       = ImVec2(8, 5);
    style.FramePadding      = ImVec2(6, 3);
    style.Colors[ImGuiCol_Header]        = ImVec4(0.22f, 0.37f, 0.55f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.46f, 0.67f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg]    = ImVec4(0.10f, 0.10f, 0.11f, 1.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.12f, 0.12f, 0.13f, 1.0f);

    // ---- System font + HiDPI -----------------------------------------------
    // Read the OS content scale (2.0 on Retina, 1.0 on standard displays).
    float contentScale = 1.0f;
    glfwGetWindowContentScale(win, &contentScale, nullptr);

    // Try platform-specific system font paths, use first one that exists.
    auto fontExists = [](const char* p) { return std::filesystem::exists(p); };
    const char* fontPath = nullptr;
#ifdef __APPLE__
    if (fontExists("/System/Library/Fonts/SFNS.ttf"))
        fontPath = "/System/Library/Fonts/SFNS.ttf";
    else if (fontExists("/System/Library/Fonts/SFNSText.ttf"))
        fontPath = "/System/Library/Fonts/SFNSText.ttf";
    else if (fontExists("/System/Library/Fonts/Helvetica.ttc"))
        fontPath = "/System/Library/Fonts/Helvetica.ttc";
#elif defined(_WIN32)
    if (fontExists("C:/Windows/Fonts/segoeui.ttf"))
        fontPath = "C:/Windows/Fonts/segoeui.ttf";
    else if (fontExists("C:/Windows/Fonts/arial.ttf"))
        fontPath = "C:/Windows/Fonts/arial.ttf";
#else
    // Common Linux paths
    if (fontExists("/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf"))
        fontPath = "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf";
    else if (fontExists("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"))
        fontPath = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    else if (fontExists("/usr/share/fonts/noto/NotoSans-Regular.ttf"))
        fontPath = "/usr/share/fonts/noto/NotoSans-Regular.ttf";
#endif

    // Load font at physical pixel size so glyphs are sharp on HiDPI.
    // FontGlobalScale maps those physical pixels back to logical pixels,
    // keeping widget layout sizes unchanged (no ScaleAllSizes needed).
    const float fontSize = std::round(13.0f * contentScale);

    // Build glyph range: default Latin + the Unicode symbols used in the UI.
    // Must be static — pointer must stay valid until the font atlas is built
    // (deferred to the first ImGui_ImplOpenGL3_NewFrame call).
    static ImVector<ImWchar> sGlyphRanges;
    {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesDefault());
        b.AddChar(0x2014);  // — EM DASH
        b.AddChar(0x2022);  // • BULLET
        b.AddChar(0x25B6);  // ▶ BLACK RIGHT-POINTING TRIANGLE
        b.AddChar(0x25CF);  // ● BLACK CIRCLE
        b.AddChar(0x25F7);  // ◷ UPPER RIGHT QUADRANT CIRCULAR ARC
        b.BuildRanges(&sGlyphRanges);
    }

    if (fontPath)
        io.Fonts->AddFontFromFileTTF(fontPath, fontSize, nullptr, sGlyphRanges.Data);
    else
        io.Fonts->AddFontDefault();

    if (contentScale > 1.0f)
        io.FontGlobalScale = 1.0f / contentScale;

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // ---- App init ----------------------------------------------------------
    App app;
    app.engineOk = app.engine.initialize();
    if (!app.engineOk)
        app.engineError = app.engine.lastError();

    // Load show file from CLI argument if provided
    if (argc >= 2) {
        std::string err;
        if (app.sf.load(argv[1], err)) {
            app.showPath = argv[1];
            app.baseDir  = std::filesystem::path(argv[1]).parent_path().string();
            rebuildCueList(app, err);
        } else {
            std::fprintf(stderr, "Could not load show: %s\n", err.c_str());
        }
    } else {
        app.sf = mcp::ShowFile::empty("Untitled Show");
    }

    // Register drag-and-drop handler
    glfwSetWindowUserPointer(win, &app);
    glfwSetDropCallback(win, dropCallback);

    // ---- Render loop -------------------------------------------------------
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        handleKeyboard(app);
        app.cues.update();  // reclaim finished stream readers

        // Full-screen dockspace window
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##main", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);
        ImGui::PopStyleVar(3);

        renderMenuBar(app, win);

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
        ImGui::SetCursorPosX(8);
        renderToolbar(app);
        ImGui::Spacing();
        ImGui::Separator();

        // Split: table (top 60%) + inspector (bottom 40%)
        const float avail = ImGui::GetContentRegionAvail().y;
        const float tableH = avail * 0.60f;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
        ImGui::BeginChild("table_region", ImVec2(0, tableH), ImGuiChildFlags_None);
        renderCueTable(app);
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::Separator();
        renderInspector(app);

        ImGui::End(); // ##main

        // Drop notification toast
        if (!app.dropMsg.empty() && glfwGetTime() < app.dropMsgExpiry) {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(
                ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                       vp->WorkPos.y + vp->WorkSize.y - 40.0f),
                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.82f);
            ImGui::Begin("##toast", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "%s", app.dropMsg.c_str());
            ImGui::End();
        } else {
            app.dropMsg.clear();
        }

        renderAddDialog(app);

        // Render
        ImGui::Render();
        int fw, fh;
        glfwGetFramebufferSize(win, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
