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
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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
    return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

struct AddDialog {
    bool  open          = false;
    int   type          = 0;     // 0=audio 1=start 2=stop
    char  path[512]     = {};
    char  name[256]     = {};
    float preWait       = 0.0f;
    char  targetCueNum[32] = {}; // user types a cue number here (start/stop)
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
    bool     insp_ac          = false;
    bool     insp_af          = false;
    int      insp_lastSel     = -2;  // force refresh on first frame

    AddDialog addDlg;
    bool wantOpenDlg   = false;
    bool wantSaveAsDlg = false;
    bool wantTitleDlg  = false;

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

    // Pass 1: for start/stop cues, resolve targetCueNumber → target index if needed.
    // Empty cue numbers are intentionally preserved — they are valid but cannot be referenced.
    for (auto& cd : cds) {
        if ((cd.type == "start" || cd.type == "stop") && !cd.targetCueNumber.empty()) {
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
        }
        if (ok) {
            app.cues.setCueCueNumber  (i, cd.cueNumber);
            app.cues.setCueStartTime  (i, cd.startTime);
            app.cues.setCueDuration   (i, cd.duration);
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
        cds[i].autoContinue = c->autoContinue;
        cds[i].autoFollow   = c->autoFollow;
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
        app.insp_ac        = c->autoContinue;
        app.insp_af        = c->autoFollow;
        app.insp_lastSel   = sel;
    }

    const char* typeStr = c->type == mcp::CueType::Audio ? "audio"
                        : c->type == mcp::CueType::Start ? "start" : "stop";

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
    } else {
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

    static const char* types[] = {"Audio", "Start", "Stop"};
    ImGui::SetNextItemWidth(100);
    ImGui::Combo("Type", &app.addDlg.type, types, 3);

    if (app.addDlg.type == 0) {
        ImGui::SetNextItemWidth(380);
        ImGui::InputText("File path", app.addDlg.path, sizeof(app.addDlg.path));
        ImGui::TextDisabled("  Tip: path relative to show file, or absolute");
    } else {
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
        } else {
            // Resolve target cue number → array index
            const std::string tnum(app.addDlg.targetCueNum);
            int tIdx = tnum.empty() ? -1 : findCueByNumber(app.sf, tnum);
            if (!tnum.empty() && tIdx < 0) {
                app.addDlg.errorMsg = "Cue number \"" + tnum + "\" not found";
                goto done;
            }
            cd.type             = (app.addDlg.type == 1) ? "start" : "stop";
            cd.target           = tIdx;
            cd.targetCueNumber  = tnum;
            cd.name             = app.addDlg.name[0] ? app.addDlg.name
                                  : (cd.type + "(Q" + tnum + ")");
            cd.preWait          = app.addDlg.preWait;
            if (cd.type == "start")
                app.cues.addStartCue(cd.target, cd.name, cd.preWait);
            else
                app.cues.addStopCue (cd.target, cd.name, cd.preWait);
            const int newIdx = app.cues.cueCount() - 1;
            app.cues.setCueCueNumber(newIdx, cd.cueNumber);
        }

        if (app.sf.cueLists.empty()) app.sf.cueLists.push_back({"main", "Main", {}});
        app.sf.cueLists[0].cues.push_back(cd);
        app.dirty = true;

        // Reset dialog fields
        std::memset(app.addDlg.path,         0, sizeof(app.addDlg.path));
        std::memset(app.addDlg.name,         0, sizeof(app.addDlg.name));
        std::memset(app.addDlg.targetCueNum, 0, sizeof(app.addDlg.targetCueNum));
        app.addDlg.preWait = 0.0f;
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

    // Deferred mutations — applied after EndTable to avoid modifying the list mid-render.
    int reorderSrc = -1, reorderDst = -1;
    int targetSetRow = -1, targetSetSrc = -1;

    for (int i = 0; i < n; ++i) {
        const mcp::Cue* c = app.cues.cueAt(i);
        if (!c) continue;

        const bool playing = app.cues.isCuePlaying(i);
        const bool pending = app.cues.isCuePending(i);
        const bool isSel   = (i == selIdx);

        ImGui::TableNextRow();

        // Row background tint
        if (playing)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(30, 90, 40, 120));
        else if (pending)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(90, 70, 10, 120));

        // Column 0: cue number
        ImGui::TableSetColumnIndex(0);
        const char* qn = c->cueNumber.empty() ? "-" : c->cueNumber.c_str();
        if (isSel) ImGui::TextColored(ImVec4(1, 0.85f, 0, 1), "▶ %s", qn);
        else        ImGui::TextDisabled("  %s", qn);

        // Column 1: type badge
        ImGui::TableSetColumnIndex(1);
        const std::string tstr = (c->type == mcp::CueType::Audio) ? "audio"
                                : (c->type == mcp::CueType::Start) ? "start" : "stop";
        ImGui::TextColored(typeColor(tstr), "%s", tstr.c_str());

        // Column 2: name — row selection + drag source + reorder drop target
        ImGui::TableSetColumnIndex(2);
        char lbl[64]; std::snprintf(lbl, sizeof(lbl), "##row%d", i);
        if (ImGui::Selectable(lbl, isSel, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 0)))
            app.cues.setSelectedIndex(i);

        // Allow subsequent columns (Target) to receive hover despite SpanAllColumns.
        ImGui::SetItemAllowOverlap();

        // Drag source: grab any row to reorder
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("CUE_IDX", &i, sizeof(int));
            ImGui::Text("Q%s  %s",
                        c->cueNumber.empty() ? "-" : c->cueNumber.c_str(),
                        c->name.c_str());
            ImGui::EndDragDropSource();
        }

        // Drop target: reorder — upper-half = insert before, lower-half = insert after
        if (ImGui::BeginDragDropTarget()) {
            const float midY = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
            const bool  after = ImGui::GetMousePos().y > midY;
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("CUE_IDX")) {
                reorderSrc = *(const int*)p->Data;
                reorderDst = after ? i + 1 : i;
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();
        ImGui::Text("%s", c->name.c_str());

        // Column 3: target — drop zone for start/stop cues
        ImGui::TableSetColumnIndex(3);
        if (c->type != mcp::CueType::Audio) {
            const mcp::Cue* tc = (c->targetIndex >= 0 && c->targetIndex < n)
                                  ? app.cues.cueAt(c->targetIndex) : nullptr;
            const std::string tdisplay = tc
                ? ("Q" + (tc->cueNumber.empty() ? std::to_string(c->targetIndex) : tc->cueNumber))
                : "  —";

            const float cellW = ImGui::GetContentRegionAvail().x;
            const float cellH = ImGui::GetTextLineHeightWithSpacing();
            char tgtId[32]; std::snprintf(tgtId, sizeof(tgtId), "##tgt%d", i);
            ImGui::InvisibleButton(tgtId, ImVec2(cellW > 4 ? cellW : 60, cellH));

            // Draw target label over the invisible button
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
            ImGui::TextDisabled("  —");
        }

        // Column 4: duration
        ImGui::TableSetColumnIndex(4);
        if (c->type == mcp::CueType::Audio)
            ImGui::TextDisabled("%s", fmtDuration(app.cues.cueTotalSeconds(i)).c_str());
        else
            ImGui::TextDisabled("  —");

        // Column 5: status / playhead progress
        ImGui::TableSetColumnIndex(5);
        if (playing) {
            const double pos   = app.cues.cuePlayheadSeconds(i);
            const double total = app.cues.cueTotalSeconds(i);
            if (total > 0.0) {
                float pct = static_cast<float>(pos / total);
                ImGui::ProgressBar(pct, ImVec2(-1, ImGui::GetTextLineHeight()), "");
            } else {
                ImGui::TextColored(ImVec4(0.3f, 1, 0.4f, 1), "▶ playing");
            }
        } else if (pending) {
            ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), "◷ pending");
        } else {
            ImGui::TextDisabled("● idle");
        }
    }

    ImGui::EndTable();

    // ---- Apply deferred reorder ---------------------------------------------
    if (reorderSrc >= 0 && reorderDst >= 0 && !app.sf.cueLists.empty()) {
        // Skip obvious no-ops: same slot or adjacent (moving one step within itself).
        if (reorderSrc != reorderDst && reorderSrc + 1 != reorderDst) {
            auto& cds = app.sf.cueLists[0].cues;
            mcp::ShowFile::CueData moved = cds[reorderSrc];
            cds.erase(cds.begin() + reorderSrc);
            // Adjust for the removal shifting indices above src down by one.
            int dst = reorderDst;
            if (reorderSrc < reorderDst) dst--;
            dst = std::clamp(dst, 0, (int)cds.size());
            cds.insert(cds.begin() + dst, moved);
            std::string err;
            rebuildCueList(app, err);
            app.cues.setSelectedIndex(dst);
            app.insp_lastSel = -2;
            app.dirty = true;
        }
    }

    // ---- Apply deferred target-set ------------------------------------------
    if (targetSetRow >= 0 && targetSetSrc >= 0 && !app.sf.cueLists.empty()) {
        auto& cds = app.sf.cueLists[0].cues;
        if (targetSetRow < (int)cds.size()) {
            const mcp::Cue* srcCue = app.cues.cueAt(targetSetSrc);
            cds[targetSetRow].target          = targetSetSrc;
            cds[targetSetRow].targetCueNumber = srcCue ? srcCue->cueNumber : "";
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
        app.cues.panic();
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
    if (fontPath)
        io.Fonts->AddFontFromFileTTF(fontPath, fontSize);
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
