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
#include <optional>
#include <set>
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
    int      insp_continue    = 0;  // 0=none, 1=auto-continue, 2=auto-follow
    int      insp_lastSel     = -2;  // force refresh on first frame

    // Fader label inline editing (double-click dB text to type value)
    int  insp_faderEdit  = -1;    // 0=level, 1=trim; -1=none
    bool insp_faderFocus = false;
    char insp_faderBuf[16] = {};

    // Inspector tab: 0=basic, 1=levels (audio cues only)
    int  insp_tab = 0;

    // Crosspoint cell inline editing state
    int  insp_xpEditS     = -1;  // source channel being edited (-1=none)
    int  insp_xpEditO     = -1;
    char insp_xpEditBuf[16] = {};
    bool insp_xpEditFocus = false;

    // Per-output level inline editing
    int  insp_outLvlEdit  = -1;  // outCh being edited (-1=none)
    bool insp_outLvlFocus = false;
    char insp_outLvlBuf[16] = {};

    // Arm cue inspector
    float insp_armStartTime     = 0.0f;

    // Fade cue inspector buffers
    int   insp_fadeCurve        = 0;     // 0=Linear 1=EqualPower
    bool  insp_fadeStopWhenDone = false;
    char  insp_fadeTargetNum[32] = {};

    // Fade crosspoint cell inline editing state
    int  insp_fxpEditS     = -1;
    int  insp_fxpEditO     = -1;
    char insp_fxpEditBuf[16] = {};
    bool insp_fxpEditFocus = false;

    std::vector<mcp::ShowFile::CueData> clipboardCues;  // Cmd+C copies here (ordered)
    std::set<int>                       multiSel;       // all selected row indices
    int  dropTargetRow = -1;   // audio-cue row under mouse; used by OS file-drop handler
    bool wantOpenDlg   = false;
    bool wantSaveAsDlg = false;
    bool wantTitleDlg  = false;
    bool wantDeviceDlg = false;

    // Device selection dialog state
    std::vector<mcp::DeviceInfo> deviceList;
    int  deviceListSel = -1;

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
    (void)err;
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
            if (!ok) {
                app.cues.addBrokenAudioCue(p.string(), cd.name, cd.preWait);
                ok = true;  // broken placeholder added; indices stay in sync
            }
        } else if (cd.type == "start") {
            app.cues.addStartCue(cd.target, cd.name, cd.preWait);
        } else if (cd.type == "stop") {
            app.cues.addStopCue(cd.target, cd.name, cd.preWait);
        } else if (cd.type == "fade") {
            const mcp::FadeData::Curve curve = (cd.fadeCurve == "equalpower")
                ? mcp::FadeData::Curve::EqualPower : mcp::FadeData::Curve::Linear;
            app.cues.addFadeCue(cd.target, cd.targetCueNumber, curve,
                                cd.fadeStopWhenDone, cd.name, cd.preWait);
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
            if (cd.type == "arm")
                app.cues.setCueArmStartTime(i, cd.armStartTime);
            // Apply routing for audio cues
            if (cd.type == "audio") {
                for (int o = 0; o < (int)cd.outLevelDb.size(); ++o)
                    app.cues.setCueOutLevel(i, o, cd.outLevelDb[o]);
                for (const auto& xe : cd.xpEntries)
                    app.cues.setCueXpoint(i, xe.s, xe.o, xe.db);
            }
            // Apply fade targets
            if (cd.type == "fade") {
                app.cues.setCueFadeMasterTarget(i, cd.fadeMasterEnabled, cd.fadeMasterTarget);
                app.cues.setCueFadeOutTargetCount(i, (int)cd.fadeOutLevels.size());
                for (const auto& fl : cd.fadeOutLevels)
                    app.cues.setCueFadeOutTarget(i, fl.ch, fl.enabled, fl.target);
                for (const auto& fx : cd.fadeXpEntries)
                    app.cues.setCueFadeXpTarget(i, fx.s, fx.o, fx.enabled, fx.target);
            }
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
        if (c->type == mcp::CueType::Arm)
            cds[i].armStartTime = c->armStartTime;
        // Audio routing
        if (c->type == mcp::CueType::Audio) {
            cds[i].outLevelDb = c->routing.outLevelDb;
            // Rebuild xpEntries from xpoint matrix (only enabled cells)
            cds[i].xpEntries.clear();
            for (int s = 0; s < (int)c->routing.xpoint.size(); ++s)
                for (int o = 0; o < (int)c->routing.xpoint[s].size(); ++o)
                    if (c->routing.xpoint[s][o].has_value())
                        cds[i].xpEntries.push_back({s, o, *c->routing.xpoint[s][o]});
        }
        if (c->type == mcp::CueType::Fade && c->fadeData) {
            const auto& fd = *c->fadeData;
            cds[i].targetCueNumber  = fd.targetCueNumber;
            cds[i].target           = fd.resolvedTargetIdx;
            cds[i].fadeStopWhenDone = fd.stopWhenDone;
            cds[i].fadeCurve        = (fd.curve == mcp::FadeData::Curve::EqualPower)
                                      ? "equalpower" : "linear";
            cds[i].fadeMasterEnabled = fd.masterLevel.enabled;
            cds[i].fadeMasterTarget  = fd.masterLevel.targetDb;
            cds[i].fadeOutLevels.clear();
            for (int o = 0; o < (int)fd.outLevels.size(); ++o)
                cds[i].fadeOutLevels.push_back({o, fd.outLevels[o].enabled,
                                                 fd.outLevels[o].targetDb});
            cds[i].fadeXpEntries.clear();
            for (int s = 0; s < (int)fd.xpTargets.size(); ++s)
                for (int o = 0; o < (int)fd.xpTargets[static_cast<size_t>(s)].size(); ++o)
                    cds[i].fadeXpEntries.push_back(
                        {s, o,
                         fd.xpTargets[static_cast<size_t>(s)][static_cast<size_t>(o)].enabled,
                         fd.xpTargets[static_cast<size_t>(s)][static_cast<size_t>(o)].targetDb});
        }
    }
    app.dirty = true;
}

// ---------------------------------------------------------------------------
// Inspector panel
// ---------------------------------------------------------------------------

// Set cue number, rejecting duplicates (sets to "" if another cue already has num).
static void setCueNumberChecked(App& app, int index, const std::string& num) {
    if (!num.empty()) {
        for (int i = 0; i < app.cues.cueCount(); ++i) {
            if (i == index) continue;
            const auto* other = app.cues.cueAt(i);
            if (other && other->cueNumber == num) {
                app.cues.setCueCueNumber(index, "");
                syncSfFromCues(app);
                app.insp_lastSel = -2;
                return;
            }
        }
    }
    app.cues.setCueCueNumber(index, num);
    syncSfFromCues(app);
    app.insp_lastSel = -2;
}

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
        app.insp_continue  = c->autoFollow ? 2 : (c->autoContinue ? 1 : 0);
        app.insp_faderEdit  = -1;
        app.insp_tab        = 0;
        app.insp_xpEditS    = -1;
        app.insp_outLvlEdit = -1;
        app.insp_fxpEditS   = -1;
        if (c->type == mcp::CueType::Fade && c->fadeData) {
            const auto& fd = *c->fadeData;
            std::strncpy(app.insp_fadeTargetNum, fd.targetCueNumber.c_str(),
                         sizeof(app.insp_fadeTargetNum) - 1);
            app.insp_fadeCurve        = (fd.curve == mcp::FadeData::Curve::EqualPower) ? 1 : 0;
            app.insp_fadeStopWhenDone = fd.stopWhenDone;
        }
        if (c->type == mcp::CueType::Arm)
            app.insp_armStartTime = static_cast<float>(c->armStartTime);
        app.insp_lastSel = sel;
    }

    const char* typeStr = c->type == mcp::CueType::Audio ? "audio"
                        : c->type == mcp::CueType::Start ? "start"
                        : c->type == mcp::CueType::Stop  ? "stop"
                        : c->type == mcp::CueType::Arm   ? "arm"
                        : "fade";

    // Multi-select helpers
    const bool isMultiEdit = (app.multiSel.size() > 1);

    // True when every selected cue is an Audio cue.
    const bool allAudio = isMultiEdit && [&]() {
        for (int idx : app.multiSel) {
            const auto* cue = app.cues.cueAt(idx);
            if (!cue || cue->type != mcp::CueType::Audio) return false;
        }
        return true;
    }();

    // Apply a setter to all selected cues, then sync ShowFile.
    auto forEachSel = [&](auto fn) {
        for (int idx : app.multiSel)
            if (idx >= 0 && idx < app.cues.cueCount()) fn(idx);
        syncSfFromCues(app);
    };

    // Returns the common float value across all selected cues, or nullopt if mixed.
    auto sameValueF = [&](auto getter) -> std::optional<float> {
        std::optional<float> ref;
        for (int idx : app.multiSel) {
            const auto* cue = app.cues.cueAt(idx);
            if (!cue) continue;
            float v = getter(cue);
            if (!ref) { ref = v; continue; }
            if (std::abs(*ref - v) > 1e-5f) return std::nullopt;
        }
        return ref;
    };

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

    // Commit a typed dB string for fader mode: 0=level, 1=trim.
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
            }
            syncSfFromCues(app);
        } catch (...) {}
        app.insp_faderEdit = -1;
    };

    // Render a fader group (label above, VSlider, dB text below with double-click edit).
    // editMode: 0=level, 1=trim.  mixed=true shows "(multiple)" instead of dB value.
    auto renderFaderGroup = [&](const char* label, const char* sliderId, const char* textId,
                                float& value, int editMode,
                                const char* tooltip, bool mixed = false) {
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", label);
        if (ImGui::VSliderFloat(sliderId, ImVec2(kFaderW, kFaderH),
                                &value, kFaderMin, kFaderMax, "")) {
            if (isMultiEdit && allAudio) {
                const float v = value;
                if (editMode == 0)
                    forEachSel([&](int idx) { app.cues.setCueLevel(idx, v); });
                else
                    forEachSel([&](int idx) { app.cues.setCueTrim(idx, v); });
            } else {
                if (editMode == 0) { app.cues.setCueLevel(sel, value); }
                else               { app.cues.setCueTrim(sel, value); }
                const int slot = app.cues.cueVoiceSlot(sel);
                if (slot >= 0 && app.engine.isVoiceActive(slot))
                    app.engine.setVoiceGain(slot, levelToGain(app.insp_level + app.insp_trim));
                syncSfFromCues(app);
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nDouble-click to reset to 0 dB", tooltip);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            value = 0.0f;
            if (isMultiEdit && allAudio) {
                if (editMode == 0)
                    forEachSel([&](int idx) { app.cues.setCueLevel(idx, 0.0f); });
                else
                    forEachSel([&](int idx) { app.cues.setCueTrim(idx, 0.0f); });
            } else {
                if (editMode == 0) { app.cues.setCueLevel(sel, 0.0); }
                else               { app.cues.setCueTrim(sel, 0.0); }
                const int slot = app.cues.cueVoiceSlot(sel);
                if (slot >= 0 && app.engine.isVoiceActive(slot))
                    app.engine.setVoiceGain(slot, levelToGain(app.insp_level + app.insp_trim));
                syncSfFromCues(app);
            }
        }
        if (app.insp_faderEdit == editMode) {
            ImGui::SetNextItemWidth(kFaderW + 10);
            if (app.insp_faderFocus) { ImGui::SetKeyboardFocusHere(); app.insp_faderFocus = false; }
            ImGui::InputText(textId, app.insp_faderBuf, sizeof(app.insp_faderBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemDeactivated()) applyFaderText(editMode);
        } else if (mixed) {
            ImGui::TextDisabled("(multiple)");
            // No double-click text edit when mixed
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

    // Pre-fetch fade state (used across multiple tabs for Fade cues).
    const mcp::FadeData* fd_raw = (c->type == mcp::CueType::Fade && c->fadeData)
                                   ? c->fadeData.get() : nullptr;
    const mcp::Cue* fadeTc = nullptr;
    if (fd_raw) {
        const int tIdx = fd_raw->resolvedTargetIdx;
        fadeTc = (tIdx >= 0 && tIdx < app.cues.cueCount()) ? app.cues.cueAt(tIdx) : nullptr;
    }

    // ---- Tab bar (all cue types) ----
    if (ImGui::BeginTabBar("##insp_tabs")) {
        if (ImGui::BeginTabItem("Basic")) {
            ImGui::Spacing();
            ImGui::SetCursorPosX(8);

            if (isMultiEdit) {
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f),
                    "%d cues selected — shared edits apply to all",
                    (int)app.multiSel.size());
                ImGui::Spacing();
                ImGui::SetCursorPosX(8);
            }

            // Cue# | Name | type badge  (single-select only)
            if (!isMultiEdit) {
                ImGui::SetNextItemWidth(60);
                ImGui::InputText("##cuenum", app.insp_cueNum, sizeof(app.insp_cueNum));
                if (ImGui::IsItemDeactivatedAfterEdit())
                    setCueNumberChecked(app, sel, app.insp_cueNum);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cue number");
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
            }

            ImGui::SetNextItemWidth(120);
            {
                const bool preWaitMixed = isMultiEdit &&
                    !sameValueF([](const mcp::Cue* cue) {
                        return static_cast<float>(cue->preWaitSeconds); }).has_value();
                const char* pwFmt = preWaitMixed ? "(multiple)" : "%.3f";
                if (ImGui::DragFloat("Pre-Wait (s)", &app.insp_preWait, 0.01f, 0.0f, 60.0f, pwFmt)) {
                    const float v = app.insp_preWait;
                    forEachSel([&](int idx) { app.cues.setCuePreWait(idx, v); });
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // ---- Type-specific content ----
            if (c->type == mcp::CueType::Audio) {
                ImGui::SetCursorPosX(8);
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

            } else if (c->type == mcp::CueType::Fade) {
                ImGui::SetCursorPosX(8);
                if (fd_raw) {
                    if (fadeTc)
                        ImGui::TextDisabled("Target: Q%s  \"%s\"",
                            fadeTc->cueNumber.c_str(), fadeTc->name.c_str());
                    else if (!fd_raw->targetCueNumber.empty())
                        ImGui::TextDisabled("Target: (unresolved Q%s)",
                            fd_raw->targetCueNumber.c_str());
                    else
                        ImGui::TextDisabled("Target: (none — set via drag-drop in cue list)");
                }
            } else {
                // Start / Stop / Arm
                ImGui::SetCursorPosX(8);
                const mcp::Cue* tc = app.cues.cueAt(c->targetIndex);
                if (tc)
                    ImGui::TextDisabled("→ Q%s  \"%s\"", tc->cueNumber.c_str(), tc->name.c_str());
                else
                    ImGui::TextDisabled("→ (unresolved)");
                // Arm cue: start-time property
                if (c->type == mcp::CueType::Arm) {
                    ImGui::Spacing();
                    ImGui::SetCursorPosX(8);
                    ImGui::SetNextItemWidth(120);
                    if (ImGui::DragFloat("Arm Start (s)", &app.insp_armStartTime,
                                         0.01f, 0.0f, 3600.0f, "%.3f")) {
                        app.cues.setCueArmStartTime(sel, app.insp_armStartTime);
                        syncSfFromCues(app);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Pre-load the target cue from this position when arm fires");
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::SetCursorPosX(8);
            {
                const char* continueItems[] = { "Do Not Continue", "Auto-Continue", "Auto-Follow" };
                ImGui::SetNextItemWidth(160);
                if (ImGui::Combo("Continue", &app.insp_continue, continueItems, 3)) {
                    const bool ac = (app.insp_continue == 1);
                    const bool af = (app.insp_continue == 2);
                    forEachSel([&](int idx) {
                        app.cues.setCueAutoContinue(idx, ac);
                        app.cues.setCueAutoFollow  (idx, af);
                    });
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Auto-Continue: fire next cue immediately\nAuto-Follow: fire next cue when this one ends");
            }

            ImGui::EndTabItem();
        }  // end Basic tab

        // Levels tab — audio cues only
        if (c->type == mcp::CueType::Audio) {
            if (ImGui::BeginTabItem("Levels")) {
                ImGui::Spacing();

                const int outCh = app.engine.channels();
                const int srcCh = c->audioFile.metadata().channels;

                auto fmtDB = [](float db, char* buf, int sz) {
                    if (db <= -59.9f) std::snprintf(buf, sz, "-inf");
                    else              std::snprintf(buf, sz, "%+.1f", (double)db);
                };

                constexpr float kColW  = 40.0f;  // fader width = xpoint cell width
                constexpr float kColSp = 4.0f;
                constexpr float kCellH = 22.0f;

                // ---- Master fader ----
                ImGui::SetCursorPosX(8);
                const bool levelMixed = isMultiEdit && allAudio &&
                    !sameValueF([](const mcp::Cue* cue) {
                        return static_cast<float>(cue->level); }).has_value();
                renderFaderGroup("Master", "##lvl_master", "##fe_master",
                                 app.insp_level, 0, "Master output level", levelMixed);

                // Capture where the master fader group ended so per-output columns
                // can start at the same X as the xpoint columns below.
                const float masterMaxScreenX = ImGui::GetItemRectMax().x;
                const float colStartX = masterMaxScreenX
                                        - ImGui::GetWindowPos().x
                                        + ImGui::GetScrollX() + 14.0f;
                // Row-label width used in the xpoint section to align columns
                const float lblW = std::max(colStartX - 8.0f - kColSp, 30.0f);

                // ---- Per-output level faders ----
                if (outCh > 0) {
                    ImGui::SameLine(0, 0);
                    ImGui::SetCursorPosX(colStartX);
                    ImGui::BeginGroup();

                    for (int o = 0; o < outCh; ++o) {
                        if (o > 0) ImGui::SameLine(0, kColSp);
                        char hdr[16]; std::snprintf(hdr, sizeof(hdr), "Out%d", o + 1);
                        ImVec2 p = ImGui::GetCursorScreenPos();
                        ImGui::Dummy(ImVec2(kColW, ImGui::GetTextLineHeight()));
                        float tw = ImGui::CalcTextSize(hdr).x;
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(p.x + (kColW - tw) * 0.5f, p.y),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), hdr);
                    }
                    for (int o = 0; o < outCh; ++o) {
                        if (o > 0) ImGui::SameLine(0, kColSp);
                        float curDb = (o < (int)c->routing.outLevelDb.size())
                                      ? c->routing.outLevelDb[o] : 0.0f;
                        char sliderId[32], textId[32];
                        std::snprintf(sliderId, sizeof(sliderId), "##olvs%d", o);
                        std::snprintf(textId,   sizeof(textId),   "##olvt%d", o);
                        ImGui::BeginGroup();
                        if (ImGui::VSliderFloat(sliderId, ImVec2(kColW, kFaderH),
                                                &curDb, kFaderMin, kFaderMax, "")) {
                            app.cues.setCueOutLevel(sel, o, curDb);
                            syncSfFromCues(app);
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Out%d level\nDouble-click to reset to 0 dB", o + 1);
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            curDb = 0.0f;
                            app.cues.setCueOutLevel(sel, o, 0.0f);
                            syncSfFromCues(app);
                        }
                        if (app.insp_outLvlEdit == o) {
                            ImGui::SetNextItemWidth(kColW);
                            if (app.insp_outLvlFocus) { ImGui::SetKeyboardFocusHere(); app.insp_outLvlFocus = false; }
                            ImGui::InputText(textId, app.insp_outLvlBuf, sizeof(app.insp_outLvlBuf),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
                            if (ImGui::IsItemDeactivated()) {
                                try {
                                    float v = (std::strncmp(app.insp_outLvlBuf, "-inf", 4) == 0)
                                              ? kFaderMin : std::stof(app.insp_outLvlBuf);
                                    v = std::clamp(v, kFaderMin, kFaderMax);
                                    app.cues.setCueOutLevel(sel, o, v);
                                    syncSfFromCues(app);
                                } catch (...) {}
                                app.insp_outLvlEdit = -1;
                            }
                        } else {
                            char dbBuf[16]; fmtDB(curDb, dbBuf, sizeof(dbBuf));
                            ImVec2 tp = ImGui::GetCursorScreenPos();
                            float  tw = ImGui::CalcTextSize(dbBuf).x;
                            ImGui::Dummy(ImVec2(kColW, ImGui::GetTextLineHeight()));
                            ImGui::GetWindowDrawList()->AddText(
                                ImVec2(tp.x + (kColW - tw) * 0.5f, tp.y),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), dbBuf);
                            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)
                                && app.insp_outLvlEdit < 0) {
                                app.insp_outLvlEdit  = o;
                                app.insp_outLvlFocus = true;
                                fmtDB(curDb, app.insp_outLvlBuf, sizeof(app.insp_outLvlBuf));
                            }
                        }
                        ImGui::EndGroup();
                    }
                    ImGui::EndGroup();
                }

                // ---- Crosspoint matrix ----
                if (srcCh > 0 && outCh > 0) {
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::SetCursorPosX(8);
                    ImGui::TextDisabled("Crosspoint  (rows = file ch, cols = outputs)");
                    ImGui::Spacing();

                    if (c->routing.xpoint.empty())
                        app.cues.initCueRouting(sel, srcCh, outCh);

                    // Header row — blank row-label spacer + column headers
                    ImGui::SetCursorPosX(8);
                    ImGui::Dummy(ImVec2(lblW, ImGui::GetTextLineHeight()));
                    for (int o = 0; o < outCh; ++o) {
                        ImGui::SameLine(0, kColSp);
                        char hdr[16]; std::snprintf(hdr, sizeof(hdr), "Out%d", o + 1);
                        ImVec2 p = ImGui::GetCursorScreenPos();
                        ImGui::Dummy(ImVec2(kColW, ImGui::GetTextLineHeight()));
                        float tw = ImGui::CalcTextSize(hdr).x;
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(p.x + (kColW - tw) * 0.5f, p.y),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), hdr);
                    }
                    ImGui::Spacing();

                    for (int s = 0; s < srcCh; ++s) {
                        ImGui::SetCursorPosX(8);
                        char rowLbl[16]; std::snprintf(rowLbl, sizeof(rowLbl), "Ch%d", s + 1);
                        ImVec2 lblP = ImGui::GetCursorScreenPos();
                        ImGui::Dummy(ImVec2(lblW, kCellH));
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(lblP.x, lblP.y + (kCellH - ImGui::GetTextLineHeight()) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), rowLbl);

                        for (int o = 0; o < outCh; ++o) {
                            ImGui::SameLine(0, kColSp);

                            std::optional<float> cellVal;
                            if (s < (int)c->routing.xpoint.size() &&
                                o < (int)c->routing.xpoint[s].size())
                                cellVal = c->routing.xpoint[s][o];

                            const bool cellEnabled = cellVal.has_value();
                            const bool isEditing   = (app.insp_xpEditS == s &&
                                                       app.insp_xpEditO == o);
                            char cellId[32];
                            std::snprintf(cellId, sizeof(cellId), "##xp%d_%d", s, o);

                            if (isEditing) {
                                ImGui::SetNextItemWidth(kColW);
                                if (app.insp_xpEditFocus) {
                                    ImGui::SetKeyboardFocusHere();
                                    app.insp_xpEditFocus = false;
                                }
                                ImGui::InputText(cellId, app.insp_xpEditBuf,
                                                sizeof(app.insp_xpEditBuf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
                                if (ImGui::IsItemDeactivated()) {
                                    std::string sv = app.insp_xpEditBuf;
                                    bool blank = sv.find_first_not_of(" \t") == std::string::npos;
                                    if (blank) {
                                        app.cues.setCueXpoint(sel, s, o, std::nullopt);
                                    } else {
                                        try {
                                            float v = std::stof(sv);
                                            app.cues.setCueXpoint(sel, s, o,
                                                std::clamp(v, kFaderMin, kFaderMax));
                                        } catch (...) {
                                            app.cues.setCueXpoint(sel, s, o, kFaderMin);
                                        }
                                    }
                                    syncSfFromCues(app);
                                    app.insp_xpEditS = -1;
                                }
                            } else {
                                char btnLbl[24];
                                if (cellEnabled) fmtDB(*cellVal, btnLbl, sizeof(btnLbl));
                                else             std::snprintf(btnLbl, sizeof(btnLbl), " ");

                                if (!cellEnabled) {
                                    ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImVec4(0.18f, 0.18f, 0.20f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                        ImVec4(0.25f, 0.25f, 0.28f, 1.0f));
                                }
                                char btnId[32];
                                std::snprintf(btnId, sizeof(btnId), "%s##b%d_%d", btnLbl, s, o);
                                if (ImGui::Button(btnId, ImVec2(kColW, kCellH))) {
                                    if (cellEnabled) app.cues.setCueXpoint(sel, s, o, std::nullopt);
                                    else             app.cues.setCueXpoint(sel, s, o, 0.0f);
                                    syncSfFromCues(app);
                                }
                                if (cellEnabled && ImGui::IsItemHovered()
                                    && ImGui::IsMouseDoubleClicked(0)) {
                                    app.insp_xpEditS     = s;
                                    app.insp_xpEditO     = o;
                                    app.insp_xpEditFocus = true;
                                    fmtDB(*cellVal, app.insp_xpEditBuf,
                                          sizeof(app.insp_xpEditBuf));
                                }
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip(cellEnabled
                                        ? "Click: disable  |  Double-click: edit dB"
                                        : "Click: enable at 0 dB");
                                if (!cellEnabled) ImGui::PopStyleColor(2);
                            }
                        }
                    }
                }
                ImGui::EndTabItem();
            }
        }  // end Levels tab

        // Trim tab — audio cues only (global scalar trim)
        if (c->type == mcp::CueType::Audio) {
            if (ImGui::BeginTabItem("Trim")) {
                ImGui::Spacing();
                ImGui::SetCursorPosX(8);
                const bool trimMixed = isMultiEdit && allAudio &&
                    !sameValueF([](const mcp::Cue* cue) {
                        return static_cast<float>(cue->trim); }).has_value();
                renderFaderGroup("Trim", "##trim", "##fe_trm", app.insp_trim, 1,
                                 "Global trim (applied on top of Level)", trimMixed);
                ImGui::EndTabItem();
            }
        }  // end Trim tab

        // Levels tab — fade cues (fade target faders)
        if (c->type == mcp::CueType::Fade && fd_raw) {
            if (ImGui::BeginTabItem("Levels")) {
                ImGui::Spacing();

                const int outCh = app.engine.channels();
                const int srcCh = fadeTc ? fadeTc->audioFile.metadata().channels : 0;

                // Ensure storage is sized
                if ((int)fd_raw->outLevels.size() < outCh)
                    app.cues.setCueFadeOutTargetCount(sel, outCh);
                if (srcCh > 0 && outCh > 0)
                    app.cues.setCueFadeXpSize(sel, srcCh, outCh);

                constexpr float kColW  = 40.0f;
                constexpr float kColSp = 4.0f;
                constexpr float kCellH = 22.0f;

                auto fmtDB = [](float db, char* buf, int sz) {
                    if (db <= -59.9f) std::snprintf(buf, sz, "-inf");
                    else              std::snprintf(buf, sz, "%+.1f", (double)db);
                };

                // Each fade-target fader has a clickable label that toggles enabled.
                // When disabled the fader is greyed out.
                auto renderFadeFader = [&](const char* label, const char* id,
                                           bool en, float tv, float w,
                                           auto onToggle, auto onValue) {
                    ImGui::BeginGroup();
                    // Clickable label — toggles enabled
                    ImGui::PushStyleColor(ImGuiCol_Text, en
                        ? ImVec4(0.3f, 0.95f, 0.45f, 1.0f)
                        : ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f,0.25f,0.25f,0.4f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.35f,0.35f,0.35f,0.4f));
                    char btnLblId[48]; std::snprintf(btnLblId, sizeof(btnLblId), "%s##lbl%s", label, id);
                    if (ImGui::Button(btnLblId, ImVec2(w, 0))) onToggle(!en, tv);
                    ImGui::PopStyleColor(4);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Click to %s", en ? "disable" : "enable");
                    // Fader
                    if (!en) ImGui::BeginDisabled();
                    char slId[48]; std::snprintf(slId, sizeof(slId), "##sld%s", id);
                    if (ImGui::VSliderFloat(slId, ImVec2(w, kFaderH),
                                            &tv, kFaderMin, kFaderMax, ""))
                        onValue(tv);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Double-click to reset to 0 dB");
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                        onValue(0.0f);
                    if (!en) ImGui::EndDisabled();
                    char dbBuf[16]; fmtDB(tv, dbBuf, sizeof(dbBuf));
                    ImGui::TextDisabled("%s", dbBuf);
                    ImGui::EndGroup();
                };

                // ---- Master level target ----
                ImGui::SetCursorPosX(8);
                {
                    const bool  masterEn = fd_raw->masterLevel.enabled;
                    const float masterT  = fd_raw->masterLevel.targetDb;
                    renderFadeFader("Master", "fdM", masterEn, masterT, kFaderW,
                        [&](bool en, float tv) {   // label clicked → toggle
                            app.cues.setCueFadeMasterTarget(sel, en, tv);
                            syncSfFromCues(app);
                        },
                        [&](float tv) {             // slider dragged → update value (keep enabled=true)
                            app.cues.setCueFadeMasterTarget(sel, true, tv);
                            syncSfFromCues(app);
                        });
                }

                // ---- Per-output fade targets ----
                if (outCh > 0 && (int)fd_raw->outLevels.size() >= outCh) {
                    ImGui::SameLine(0, 16);
                    ImGui::BeginGroup();

                    // Clickable column headers — toggle enabled state per output
                    for (int o = 0; o < outCh; ++o) {
                        if (o > 0) ImGui::SameLine(0, kColSp);
                        const bool en = fd_raw->outLevels[o].enabled;
                        char hdrId[32]; std::snprintf(hdrId, sizeof(hdrId), "Out%d##fdoh%d", o+1, o);
                        ImGui::PushStyleColor(ImGuiCol_Text, en
                            ? ImVec4(0.3f, 0.95f, 0.45f, 1.0f)
                            : ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f,0.25f,0.25f,0.4f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.35f,0.35f,0.35f,0.4f));
                        if (ImGui::Button(hdrId, ImVec2(kColW, 0))) {
                            app.cues.setCueFadeOutTarget(sel, o, !en, fd_raw->outLevels[o].targetDb);
                            syncSfFromCues(app);
                        }
                        ImGui::PopStyleColor(4);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Click to %s Out%d fade target", en?"disable":"enable", o+1);
                    }

                    // Faders
                    for (int o = 0; o < outCh; ++o) {
                        if (o > 0) ImGui::SameLine(0, kColSp);
                        const bool  en = fd_raw->outLevels[o].enabled;
                        float tvMut = fd_raw->outLevels[o].targetDb;
                        char slId[32]; std::snprintf(slId, sizeof(slId), "##fdOSld%d", o);
                        if (!en) ImGui::BeginDisabled();
                        if (ImGui::VSliderFloat(slId, ImVec2(kColW, kFaderH),
                                                &tvMut, kFaderMin, kFaderMax, "")) {
                            app.cues.setCueFadeOutTarget(sel, o, en, tvMut);
                            syncSfFromCues(app);
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Double-click to reset to 0 dB");
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            app.cues.setCueFadeOutTarget(sel, o, en, 0.0f);
                            syncSfFromCues(app);
                        }
                        if (!en) ImGui::EndDisabled();
                    }

                    // dB labels
                    for (int o = 0; o < outCh; ++o) {
                        if (o > 0) ImGui::SameLine(0, kColSp);
                        float tv = fd_raw->outLevels[o].targetDb;
                        char dbBuf[16]; fmtDB(tv, dbBuf, sizeof(dbBuf));
                        ImVec2 tp = ImGui::GetCursorScreenPos();
                        float  tw = ImGui::CalcTextSize(dbBuf).x;
                        ImGui::Dummy(ImVec2(kColW, ImGui::GetTextLineHeight()));
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(tp.x + (kColW - tw) * 0.5f, tp.y),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), dbBuf);
                    }

                    ImGui::EndGroup();
                }

                // ---- Crosspoint fade targets ----
                if (srcCh > 0 && outCh > 0
                    && (int)fd_raw->xpTargets.size() >= srcCh
                    && !fd_raw->xpTargets.empty()
                    && (int)fd_raw->xpTargets[0].size() >= outCh) {

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::SetCursorPosX(8);
                    ImGui::TextDisabled("Crosspoint fade targets  (rows = file ch, cols = outputs)");
                    ImGui::Spacing();

                    const float lblW = 40.0f;

                    // Header
                    ImGui::SetCursorPosX(8);
                    ImGui::Dummy(ImVec2(lblW, ImGui::GetTextLineHeight()));
                    for (int o = 0; o < outCh; ++o) {
                        ImGui::SameLine(0, kColSp);
                        char hdr[16]; std::snprintf(hdr, sizeof(hdr), "Out%d", o + 1);
                        ImVec2 p = ImGui::GetCursorScreenPos();
                        ImGui::Dummy(ImVec2(kColW, ImGui::GetTextLineHeight()));
                        float tw = ImGui::CalcTextSize(hdr).x;
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(p.x + (kColW - tw) * 0.5f, p.y),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), hdr);
                    }
                    ImGui::Spacing();

                    for (int s = 0; s < srcCh; ++s) {
                        ImGui::SetCursorPosX(8);
                        char rowLbl[16]; std::snprintf(rowLbl, sizeof(rowLbl), "Ch%d", s + 1);
                        ImVec2 lblP = ImGui::GetCursorScreenPos();
                        ImGui::Dummy(ImVec2(lblW, kCellH));
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(lblP.x, lblP.y + (kCellH - ImGui::GetTextLineHeight()) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), rowLbl);

                        for (int o = 0; o < outCh; ++o) {
                            ImGui::SameLine(0, kColSp);
                            const bool cellEn = fd_raw->xpTargets[s][o].enabled;
                            const float cellT  = fd_raw->xpTargets[s][o].targetDb;

                            char btnLbl[24];
                            if (cellEn) fmtDB(cellT, btnLbl, sizeof(btnLbl));
                            else        std::snprintf(btnLbl, sizeof(btnLbl), " ");

                            if (!cellEn) {
                                ImGui::PushStyleColor(ImGuiCol_Button,
                                    ImVec4(0.18f, 0.18f, 0.20f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                    ImVec4(0.25f, 0.25f, 0.28f, 1.0f));
                            }
                            char btnId[32];
                            std::snprintf(btnId, sizeof(btnId), "%s##fxp%d_%d", btnLbl, s, o);
                            if (ImGui::Button(btnId, ImVec2(kColW, kCellH))) {
                                app.cues.setCueFadeXpTarget(sel, s, o, !cellEn, cellT);
                                syncSfFromCues(app);
                            }
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(cellEn
                                    ? "Click: disable  |  Double-click: set target dB"
                                    : "Click: enable at 0 dB");
                            if (cellEn && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                                // inline text edit via InputFloat popup
                                ImGui::OpenPopup("##fxpEdit");
                            }
                            if (!cellEn) ImGui::PopStyleColor(2);
                        }
                    }
                }

                ImGui::EndTabItem();
            }
        }  // end Levels tab (fade)

        // Curve tab — fade cues (duration, curve shape, stop when done)
        if (c->type == mcp::CueType::Fade && fd_raw) {
            if (ImGui::BeginTabItem("Curve")) {
                ImGui::Spacing();
                ImGui::SetCursorPosX(8);
                ImGui::SetNextItemWidth(120);
                if (ImGui::DragFloat("Duration (s)", &app.insp_duration, 0.01f, 0.0f, 3600.0f, "%.3f")) {
                    app.cues.setCueDuration(sel, app.insp_duration);
                    syncSfFromCues(app);
                }

                ImGui::Spacing();
                ImGui::SetCursorPosX(8);
                const char* curveItems[] = { "Linear", "Equal Power" };
                ImGui::SetNextItemWidth(140);
                if (ImGui::Combo("Curve", &app.insp_fadeCurve, curveItems, 2)) {
                    app.cues.setCueFadeCurve(sel,
                        app.insp_fadeCurve == 1 ? mcp::FadeData::Curve::EqualPower
                                                : mcp::FadeData::Curve::Linear);
                    syncSfFromCues(app);
                }

                ImGui::Spacing();
                ImGui::SetCursorPosX(8);
                if (ImGui::Checkbox("Stop target when done", &app.insp_fadeStopWhenDone)) {
                    app.cues.setCueFadeStopWhenDone(sel, app.insp_fadeStopWhenDone);
                    syncSfFromCues(app);
                }

                ImGui::EndTabItem();
            }
        }  // end Curve tab (fade)

        ImGui::EndTabBar();
    }  // end tab bar

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
// Cue helpers
// ---------------------------------------------------------------------------

static bool isCueBroken(const mcp::Cue* c) {
    if (!c) return true;
    switch (c->type) {
    case mcp::CueType::Audio:
        return c->path.empty() || !c->audioFile.isLoaded();
    case mcp::CueType::Start:
    case mcp::CueType::Stop:
    case mcp::CueType::Arm:
        return c->targetIndex < 0;
    case mcp::CueType::Fade: {
        if (!c->fadeData) return true;
        if (c->fadeData->resolvedTargetIdx < 0) return true;
        bool any = c->fadeData->masterLevel.enabled;
        for (const auto& ol : c->fadeData->outLevels) if (ol.enabled) { any = true; break; }
        if (!any)
            for (const auto& row : c->fadeData->xpTargets)
                for (const auto& cell : row) if (cell.enabled) { any = true; break; }
        return !any;
    }
    }
    return true;
}

// Add a new cue of the given type directly, using the selected cue as target.
static void addCueDirectly(App& app, const std::string& type) {
    if (app.sf.cueLists.empty()) app.sf.cueLists.push_back({"main", "Main", {}});
    const std::string cueNum = nextCueNumber(app.sf);
    const int sel    = app.cues.selectedIndex();
    const mcp::Cue* selCue = (sel >= 0 && sel < app.cues.cueCount())
                              ? app.cues.cueAt(sel) : nullptr;
    const std::string selNum = selCue ? selCue->cueNumber : "";
    const int tIdx = selCue ? sel : -1;

    mcp::ShowFile::CueData cd;
    cd.cueNumber = cueNum;
    cd.type      = type;

    if (type == "audio") {
        cd.name = "Audio Cue";
        cd.path = "";
        app.cues.addBrokenAudioCue("", cd.name);
    } else if (type == "start" || type == "stop" || type == "arm") {
        cd.target          = tIdx;
        cd.targetCueNumber = selNum;
        cd.name = type + (selNum.empty() ? "" : "(Q" + selNum + ")");
        if      (type == "start") app.cues.addStartCue(tIdx, cd.name);
        else if (type == "stop")  app.cues.addStopCue (tIdx, cd.name);
        else                      app.cues.addArmCue  (tIdx, cd.name);
    } else if (type == "fade") {
        const bool targetIsAudio = (selCue && selCue->type == mcp::CueType::Audio);
        const int  fadeTIdx = targetIsAudio ? tIdx : -1;
        const std::string fadeTNum = targetIsAudio ? selNum : "";
        cd.target          = fadeTIdx;
        cd.targetCueNumber = fadeTNum;
        cd.name    = fadeTNum.empty() ? "Fade Cue" : ("fade(Q" + fadeTNum + ")");
        cd.duration = 3.0;
        cd.fadeCurve = "linear";
        app.cues.addFadeCue(fadeTIdx, fadeTNum, mcp::FadeData::Curve::Linear,
                            false, cd.name);
        const int ni = app.cues.cueCount() - 1;
        app.cues.setCueDuration(ni, 3.0);
    } else {
        return;
    }

    const int newIdx = app.cues.cueCount() - 1;
    app.cues.setCueCueNumber(newIdx, cueNum);
    app.sf.cueLists[0].cues.push_back(cd);
    app.cues.setSelectedIndex(newIdx);
    app.multiSel.clear();
    app.multiSel.insert(newIdx);
    app.insp_lastSel = -2;
    app.dirty = true;
}

// ---------------------------------------------------------------------------
// Cue table
// ---------------------------------------------------------------------------

static void renderCueTable(App& app) {
    app.dropTargetRow = -1;  // reset each frame; updated below for hovered audio rows
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
    bool deferDeleteMulti = false;
    std::vector<mcp::ShowFile::CueData> deferPasteList;
    int deferPasteAfterRow = -1;

    for (int i = 0; i < n; ++i) {
        const mcp::Cue* c = app.cues.cueAt(i);
        if (!c) continue;

        const bool playing     = app.cues.isCuePlaying(i);
        const bool pending     = app.cues.isCuePending(i);
        const bool isSel       = (i == selIdx);
        const bool isMultiSel  = app.multiSel.count(i) > 0;
        const bool isEditing   = (app.inline_editRow == i);

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
        else if (isMultiSel && !isSel)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(50, 70, 130, 110));

        // ---- Col 0: cue number ------------------------------------------------
        ImGui::TableSetColumnIndex(0);
        if (isEditing && app.inline_editCol == 0) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (app.inline_needFocus) { ImGui::SetKeyboardFocusHere(); app.inline_needFocus = false; }
            ImGui::InputText("##ie0", app.inline_editBuf, sizeof(app.inline_editBuf));
            if (ImGui::IsItemDeactivated()) {
                setCueNumberChecked(app, i, app.inline_editBuf);
                app.inline_editRow = -1;
            }
        } else {
            const char* qn     = c->cueNumber.empty() ? "-" : c->cueNumber.c_str();
            const bool  broken = isCueBroken(c);
            const bool  armed  = app.cues.isArmed(i);
            if (broken) {
                ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f),
                                   "\xe2\x9c\x95 %s", qn);  // ✕
            } else if (armed) {
                ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.1f, 1.0f),
                                   "\xe2\x97\x8f %s", qn);  // ● yellow = armed
            } else if (isSel) {
                ImGui::TextColored(ImVec4(1, 0.85f, 0, 1), "\xe2\x96\xb6 %s", qn);
            } else {
                ImGui::TextDisabled("  %s", qn);
            }
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
            if (ImGui::Selectable(lbl, isMultiSel || isSel, ImGuiSelectableFlags_SpanAllColumns)) {
                const bool cmdHeld   = (ImGui::GetIO().KeyMods & ImGuiMod_Ctrl) != 0;
                const bool shiftHeld = (ImGui::GetIO().KeyMods & ImGuiMod_Shift) != 0;
                if (cmdHeld) {
                    // Toggle this row; keep primary unchanged unless we remove it
                    if (app.multiSel.count(i)) {
                        app.multiSel.erase(i);
                        if (i == app.cues.selectedIndex() && !app.multiSel.empty())
                            app.cues.setSelectedIndex(*app.multiSel.begin());
                    } else {
                        app.multiSel.insert(i);
                    }
                } else if (shiftHeld) {
                    // Range from current primary to clicked row
                    const int lo = std::min(selIdx, i), hi = std::max(selIdx, i);
                    for (int j = lo; j <= hi; ++j) app.multiSel.insert(j);
                    app.cues.setSelectedIndex(i);
                } else {
                    // Single select
                    app.multiSel.clear();
                    app.multiSel.insert(i);
                    app.cues.setSelectedIndex(i);
                }
            }
            ImGui::SetItemAllowOverlap();

            // Track which audio-cue row the mouse is over for OS file-drop replacement.
            if (c->type == mcp::CueType::Audio) {
                const ImVec2 rMin = ImGui::GetItemRectMin();
                const ImVec2 rMax = ImGui::GetItemRectMax();
                const ImVec2 mp   = ImGui::GetMousePos();
                if (mp.y >= rMin.y && mp.y < rMax.y)
                    app.dropTargetRow = i;
            }

            // Right-click context menu — tied to the span-all Selectable so it
            // fires anywhere on the row, not just over the name column.
            {
                char ctxId[32]; std::snprintf(ctxId, sizeof(ctxId), "##ctx%d", i);
                if (ImGui::BeginPopupContextItem(ctxId)) {
                    // Ensure right-clicked row is selected
                    if (!app.multiSel.count(i)) {
                        app.multiSel.clear();
                        app.multiSel.insert(i);
                        app.cues.setSelectedIndex(i);
                    }
                    const int nSel = (int)app.multiSel.size();
                    if (nSel == 1 && app.cues.cueAt(i) &&
                        app.cues.cueAt(i)->type == mcp::CueType::Audio)
                        if (ImGui::MenuItem("Arm"))
                            app.cues.arm(i);
                    ImGui::Separator();
                    char copyLbl[32]; std::snprintf(copyLbl, sizeof(copyLbl),
                        "Copy%s", nSel > 1 ? " (selection)" : "");
                    if (ImGui::MenuItem(copyLbl, "Cmd+C")) {
                        if (!app.sf.cueLists.empty()) {
                            app.clipboardCues.clear();
                            for (int idx : app.multiSel)
                                if (idx < (int)app.sf.cueLists[0].cues.size())
                                    app.clipboardCues.push_back(app.sf.cueLists[0].cues[idx]);
                        }
                    }
                    const bool canPaste = !app.clipboardCues.empty();
                    if (!canPaste) ImGui::BeginDisabled();
                    if (ImGui::MenuItem("Paste", "Cmd+V") && canPaste) {
                        deferPasteList     = app.clipboardCues;
                        deferPasteAfterRow = i;
                    }
                    if (!canPaste) ImGui::EndDisabled();
                    ImGui::Separator();
                    char delLbl[32]; std::snprintf(delLbl, sizeof(delLbl),
                        "Delete%s", nSel > 1 ? " (selection)" : "");
                    if (ImGui::MenuItem(delLbl))
                        deferDeleteMulti = true;
                    ImGui::EndPopup();
                }
            }

            // Double-click → start inline edit for the column under the mouse.
            // Col 3 (Target) and Col 5 (Status) are excluded.
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && app.inline_editRow < 0) {
                app.cues.setSelectedIndex(i);
                app.multiSel.clear();
                app.multiSel.insert(i);
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

    // ---- Apply deferred multi-delete ----------------------------------------
    if (deferDeleteMulti && !app.sf.cueLists.empty() && !app.multiSel.empty()) {
        for (int idx : app.multiSel) app.cues.stop(idx);
        auto& cds = app.sf.cueLists[0].cues;
        // Erase in reverse order so indices stay valid
        for (auto it = app.multiSel.rbegin(); it != app.multiSel.rend(); ++it)
            if (*it < (int)cds.size()) cds.erase(cds.begin() + *it);
        app.multiSel.clear();
        std::string err; rebuildCueList(app, err);
        app.multiSel = {app.cues.selectedIndex()};
        app.dirty = true;
        app.insp_lastSel = -2;
    }

    // ---- Apply deferred paste (multi) ---------------------------------------
    if (!deferPasteList.empty() && !app.sf.cueLists.empty()) {
        auto& cds = app.sf.cueLists[0].cues;
        int insertAt = std::min(deferPasteAfterRow + 1, (int)cds.size());
        for (auto& pd : deferPasteList) {
            pd.cueNumber = nextCueNumber(app.sf);
            cds.insert(cds.begin() + insertAt, pd);
            ++insertAt;
        }
        std::string err; rebuildCueList(app, err);
        const int lastInserted = insertAt - 1;
        app.cues.setSelectedIndex(lastInserted);
        app.multiSel.clear();
        app.multiSel.insert(lastInserted);
        app.insp_lastSel = -2;
        app.dirty = true;
    }

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
            app.multiSel.clear();
            app.multiSel.insert(dst);
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
// Device selection dialog
// ---------------------------------------------------------------------------

static void renderDeviceDialog(App& app) {
    if (app.wantDeviceDlg) {
        app.wantDeviceDlg = false;
        app.deviceList    = mcp::AudioEngine::listOutputDevices();
        app.deviceListSel = -1;
        // Pre-select the currently active device by name
        const std::string& cur = app.sf.engine.deviceName;
        for (int i = 0; i < (int)app.deviceList.size(); ++i) {
            if (app.deviceList[i].name == cur) { app.deviceListSel = i; break; }
        }
        ImGui::OpenPopup("Device Settings");
    }

    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Device Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextDisabled("Select audio output device:");
    ImGui::Spacing();

    if (app.deviceList.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "  No output devices found.");
    } else {
        ImGui::BeginChild("##devlist", ImVec2(0, 200.0f), ImGuiChildFlags_Border);
        for (int i = 0; i < (int)app.deviceList.size(); ++i) {
            const auto& d = app.deviceList[i];
            char lbl[256];
            std::snprintf(lbl, sizeof(lbl), "%s  (%d ch)", d.name.c_str(), d.maxOutputChannels);
            if (ImGui::Selectable(lbl, app.deviceListSel == i))
                app.deviceListSel = i;
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();

    const bool canApply = (app.deviceListSel >= 0 &&
                           app.deviceListSel < (int)app.deviceList.size());
    if (!canApply) ImGui::BeginDisabled();
    if (ImGui::Button("Apply", ImVec2(80, 0))) {
        const auto& d = app.deviceList[app.deviceListSel];
        app.engine.shutdown();
        app.engineOk = app.engine.initialize(48000, 0, d.name);
        if (app.engineOk) {
            app.sf.engine.deviceName = d.name;
            app.sf.engine.channels   = app.engine.channels();
            app.dirty = true;
        } else {
            app.engineError = app.engine.lastError();
        }
        ImGui::CloseCurrentPopup();
    }
    if (!canApply) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0)))
        ImGui::CloseCurrentPopup();

    if (!app.engineOk && !app.engineError.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", app.engineError.c_str());
    }

    ImGui::EndPopup();
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
        if (ImGui::MenuItem("Device Settings…")) app.wantDeviceDlg = true;
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
    // ---- Add-cue icon buttons (one per type) --------------------------------
    struct CueBtn { const char* icon; const char* type; const char* tip; ImVec4 col; };
    static const CueBtn kBtns[] = {
        { "\xe2\x99\xaa", "audio", "Add Audio Cue",  {0.40f, 0.70f, 1.00f, 1.0f} },
        { "\xe2\x96\xb6", "start", "Add Start Cue",  {0.40f, 0.90f, 0.50f, 1.0f} },
        { "\xe2\x96\xa0", "stop",  "Add Stop Cue",   {1.00f, 0.45f, 0.45f, 1.0f} },
        { "~",            "fade",  "Add Fade Cue",   {0.95f, 0.70f, 0.20f, 1.0f} },
        { "\xe2\x97\x8e", "arm",   "Add Arm Cue",    {0.75f, 0.40f, 0.95f, 1.0f} },
    };
    const ImVec2 kBtnSz(28, 22);
    for (const auto& b : kBtns) {
        ImGui::PushStyleColor(ImGuiCol_Text,          b.col);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.34f, 1.0f));
        char bid[32]; std::snprintf(bid, sizeof(bid), "%s##add_%s", b.icon, b.type);
        if (ImGui::Button(bid, kBtnSz)) addCueDirectly(app, b.type);
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", b.tip);
        ImGui::SameLine(0, 2);
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
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
        app.cues.selectNext();
        // Reset multi-selection to the newly focused row
        app.multiSel.clear();
        app.multiSel.insert(app.cues.selectedIndex());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
        app.cues.selectPrev();
        app.multiSel.clear();
        app.multiSel.insert(app.cues.selectedIndex());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S, false) && (ImGui::GetIO().KeyMods & ImGuiMod_Ctrl))
        saveShow(app);
    if (ImGui::IsKeyPressed(ImGuiKey_O, false) && (ImGui::GetIO().KeyMods & ImGuiMod_Ctrl))
        app.wantOpenDlg = true;
    if (ImGui::IsKeyPressed(ImGuiKey_D, false) && (ImGui::GetIO().KeyMods & ImGuiMod_Ctrl)) {
        // Clear cue numbers for all selected cues
        for (int idx : app.multiSel)
            if (idx >= 0 && idx < app.cues.cueCount())
                app.cues.setCueCueNumber(idx, "");
        syncSfFromCues(app);
        app.insp_lastSel = -2;
        app.dirty = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_C, false) && (ImGui::GetIO().KeyMods & ImGuiMod_Ctrl)) {
        // Copy all selected cues in index order
        if (!app.sf.cueLists.empty()) {
            app.clipboardCues.clear();
            for (int idx : app.multiSel)
                if (idx >= 0 && idx < (int)app.sf.cueLists[0].cues.size())
                    app.clipboardCues.push_back(app.sf.cueLists[0].cues[idx]);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_V, false) && (ImGui::GetIO().KeyMods & ImGuiMod_Ctrl)) {
        if (!app.clipboardCues.empty()) {
            if (app.sf.cueLists.empty()) app.sf.cueLists.push_back({"main", "Main", {}});
            auto& cds = app.sf.cueLists[0].cues;
            const int s = app.cues.selectedIndex();
            int insertAt = std::min(s + 1, (int)cds.size());
            for (auto cd : app.clipboardCues) {
                cd.cueNumber = nextCueNumber(app.sf);
                cds.insert(cds.begin() + insertAt, cd);
                ++insertAt;
            }
            std::string err; rebuildCueList(app, err);
            const int lastInserted = insertAt - 1;
            app.cues.setSelectedIndex(lastInserted);
            app.multiSel.clear();
            app.multiSel.insert(lastInserted);
            app.insp_lastSel = -2;
            app.dirty = true;
        }
    }
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

    auto toStorePath = [&](const std::filesystem::path& abs) -> std::string {
        if (!app->baseDir.empty()) {
            std::error_code ec;
            auto rel = std::filesystem::relative(abs, app->baseDir, ec);
            if (!ec && rel.string().substr(0, 2) != "..")
                return rel.string();
        }
        return abs.string();
    };

    // Dropping a single file onto an existing audio-cue row → replace its path.
    if (count == 1 && app->dropTargetRow >= 0
        && !app->sf.cueLists.empty()
        && app->dropTargetRow < (int)app->sf.cueLists[0].cues.size()) {
        auto& cd = app->sf.cueLists[0].cues[app->dropTargetRow];
        if (cd.type == "audio") {
            const std::filesystem::path absPath(paths[0]);
            cd.path = toStorePath(absPath);
            if (cd.name.empty() || cd.name == "Audio Cue")
                cd.name = absPath.stem().string();
            std::string err;
            rebuildCueList(*app, err);
            app->insp_lastSel = -2;
            app->dirty = true;
            app->dropMsg       = "Replaced: " + absPath.filename().string();
            app->dropMsgExpiry = glfwGetTime() + 3.0;
            return;
        }
    }

    // Otherwise add each file as a new audio cue.
    int added = 0, failed = 0;
    for (int i = 0; i < count; ++i) {
        const std::filesystem::path absPath(paths[i]);
        const std::string storePath = toStorePath(absPath);
        const std::string name = absPath.stem().string();

        if (!app->cues.addCue(absPath.string(), name)) {
            ++failed;
            continue;
        }

        if (app->sf.cueLists.empty())
            app->sf.cueLists.push_back({"main", "Main", {}});

        mcp::ShowFile::CueData cd;
        cd.type      = "audio";
        cd.path      = storePath;
        cd.name      = name;
        cd.cueNumber = nextCueNumber(app->sf);
        app->sf.cueLists[0].cues.push_back(cd);
        app->dirty = true;
        ++added;
    }

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
        b.AddChar(0x2715);  // ✕ MULTIPLICATION X  (broken cue indicator)
        b.AddChar(0x266A);  // ♪ EIGHTH NOTE       (audio cue button)
        b.AddChar(0x25A0);  // ■ BLACK SQUARE       (stop cue button)
        b.AddChar(0x25B6);  // ▶ BLACK RIGHT-POINTING TRIANGLE
        b.AddChar(0x25CE);  // ◎ BULLSEYE           (arm cue button)
        b.AddChar(0x25CF);  // ● BLACK CIRCLE
        b.AddChar(0x25F7);  // ◷ UPPER RIGHT QUADRANT CIRCULAR ARC
        b.BuildRanges(&sGlyphRanges);
    }

    if (fontPath)
        io.Fonts->AddFontFromFileTTF(fontPath, fontSize, nullptr, sGlyphRanges.Data);
    else
        io.Fonts->AddFontDefault();

    // Merge a CJK font so Chinese device names and other Unicode text render
    // correctly instead of showing '?' placeholders.  OversampleH/V=1 keeps
    // the atlas from ballooning; PixelSnapH avoids sub-pixel blur on CJK glyphs.
    {
        static const char* kCjkCandidates[] = {
#ifdef __APPLE__
            "/System/Library/Fonts/Hiragino Sans GB.ttc",
            "/System/Library/Fonts/STHeiti Medium.ttc",
            "/System/Library/Fonts/STHeiti Light.ttc",
#elif defined(_WIN32)
            "C:/Windows/Fonts/msyh.ttc",    // Microsoft YaHei
            "C:/Windows/Fonts/simsun.ttc",  // SimSun
#else
            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
#endif
            nullptr
        };
        for (int pi = 0; kCjkCandidates[pi]; ++pi) {
            if (fontExists(kCjkCandidates[pi])) {
                ImFontConfig cfg;
                cfg.MergeMode   = true;
                cfg.OversampleH = 1;
                cfg.OversampleV = 1;
                cfg.PixelSnapH  = true;
                io.Fonts->AddFontFromFileTTF(kCjkCandidates[pi], fontSize, &cfg,
                                              io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                break;
            }
        }
    }

    if (contentScale > 1.0f)
        io.FontGlobalScale = 1.0f / contentScale;

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // ---- App init ----------------------------------------------------------
    App app;

    // Load show file first (if given) so we can read the saved device name.
    if (argc >= 2) {
        std::string err;
        if (app.sf.load(argv[1], err)) {
            app.showPath = argv[1];
            app.baseDir  = std::filesystem::path(argv[1]).parent_path().string();
        } else {
            std::fprintf(stderr, "Could not load show: %s\n", err.c_str());
            app.sf = mcp::ShowFile::empty("Untitled Show");
        }
    } else {
        app.sf = mcp::ShowFile::empty("Untitled Show");
    }

    // Initialize engine with the saved device name (empty = use default).
    app.engineOk = app.engine.initialize(48000, 0, app.sf.engine.deviceName);
    if (!app.engineOk)
        app.engineError = app.engine.lastError();
    app.sf.engine.channels = app.engine.channels();  // sync detected count

    // Build cue list after engine is ready.
    if (!app.showPath.empty()) {
        std::string err;
        rebuildCueList(app, err);
    }
    // Seed multiSel so it always has at least the primary index.
    if (app.cues.cueCount() > 0)
        app.multiSel.insert(app.cues.selectedIndex());

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

        renderDeviceDialog(app);

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
