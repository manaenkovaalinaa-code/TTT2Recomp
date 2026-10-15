// Experimental mods in the Mods tab: the warning before enabling one (which backs up the saves) and
// the "Save backups" section (list, Back up now, Restore at next start). Same ImGui navigation as the
// other overlay dialogs; the host supplies controller B. Used by the overlay and offscreen tests.
#pragma once
#include "../mods/packages.h"
#include "../mods/save_backup.h"
#include "imgui.h"
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace overlay::experimental {
inline const ImVec4 kBadgeColour(1.0f, 0.62f, 0.2f, 1.0f);
inline constexpr const char* kBadge = "Experimental";
inline void badge() {
    ImGui::SameLine();
    ImGui::TextColored(kBadgeColour, "%s", kBadge);
}

// ---- warning before enabling
struct Confirm {
    std::string id, name;
    std::vector<std::pair<std::string, std::string>> mods;  // experimental packages this enable turns on
    bool open_now = false;
    std::function<void()> next;  // the normal enable flow, after the backup
};
inline Confirm confirm;

// Called before every enable flow of a package (checkbox, Set up). Returns true when it opened the
// warning instead; `next` then runs after the player confirmed and the backup succeeded.
inline bool gate(const mods::packages::View& mod, std::function<void()> next) {
    auto mods = mods::packages::unacknowledged_experimental(mod.id);
    if (mods.empty()) return false;
    confirm = {mod.id, mod.name, std::move(mods), true, std::move(next)};
    return true;
}

enum class Answer { None, Accepted, Cancelled, Failed };
inline std::string warning_text(const Confirm& c) {
    std::string names;
    for (size_t i = 0; i < c.mods.size(); i++)
        names += (i == 0 ? "" : i + 1 == c.mods.size() ? " and " : ", ") + c.mods[i].second;
    return names + (c.mods.size() == 1 ? " is" : " are") +
           " experimental: untested, and it can cause game-breaking problems such as crashes, stuck events or lost "
           "progress. Saving the game while it is active can damage your save so that it no longer loads or "
           "plays correctly.";
}
inline constexpr const char* kBackupText =
    "Before it is turned on, your saves are backed up. You can restore them at any time in Mods > Save backups.";

// error receives the reason when the backup fails (the mod then stays off).
inline Answer dialog(bool back, std::string& error) {
    const char* title = "Experimental mod##experimental_confirm";
    if (confirm.open_now) { ImGui::OpenPopup(title); confirm.open_now = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return Answer::None;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
    ImGui::TextColored(kBadgeColour, "%s", kBadge);
    ImGui::TextWrapped("%s", warning_text(confirm).c_str());
    ImGui::TextWrapped("%s", kBackupText);
    if (confirm.mods.size() > 1 || confirm.mods[0].first != confirm.id)
        ImGui::TextDisabled("Enabling %s also enables these packages.", confirm.name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    bool accept = ImGui::Button("Back up saves and enable");
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel", ImVec2(120, 0));
    ImGui::SetItemDefaultFocus();  // keyboard and controller start on Cancel
    if (back || ImGui::IsKeyPressed(ImGuiKey_Escape)) { cancel = true; accept = false; }
    auto answer = Answer::None;
    if (accept) {
        if (mods::packages::acknowledge_experimental(confirm.id, error)) {
            answer = Answer::Accepted;
            if (confirm.next) confirm.next();
        } else answer = Answer::Failed;
    } else if (cancel) answer = Answer::Cancelled;
    if (answer != Answer::None) {
        confirm = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return answer;
}

// ---- Save backups section
struct Backups {
    std::string message;        // last action's result
    std::string restore_choice; // backup name awaiting the restore confirmation
    bool open_restore = false;
    std::string delete_choice;  // backup name awaiting the delete confirmation
    bool open_delete = false;
};
inline Backups backups;

// Description of the staged restore for the restart notice, or empty.
inline std::string staged_restore() {
    std::lock_guard guard(mods::saves::mutex());
    auto& store = mods::saves::store();
    auto name = store.pending();
    if (name.empty()) return {};
    for (const auto& info : store.list())
        if (info.name == name) return info.when() + " (" + info.description() + ")";
    return name;
}

// Returns true when controller B was used by the restore dialog.
inline bool backups_section(bool back) {
    using namespace mods::saves;
    std::lock_guard guard(mutex());
    auto& s = store();
    bool b_used = false;
    if (!startup_notice().empty()) {
        ImGui::TextWrapped("%s", startup_notice().c_str());
        if (ImGui::Button("Dismiss##restore_notice")) startup_notice().clear();
    }
    if (!s.configured()) {
        ImGui::TextDisabled("Save backups are unavailable: the save folder is unknown.");
        return false;
    }
    ImGui::TextDisabled("Enabling an experimental mod backs up your saves first. The newest %zu automatic backups are "
                        "kept; backups made with Back up now are never deleted.", kKeepAutomatic);
    ImGui::TextDisabled("Backups folder: %s", s.root().string().c_str());
    if (ImGui::Button("Back up now")) {
        auto result = s.create("manual");
        backups.message = result.ok ? (result.created ? result.message : std::string("No save data yet; nothing to back up."))
                                    : result.error;
    }
    if (!backups.message.empty()) ImGui::TextWrapped("%s", backups.message.c_str());
    auto staged = s.pending();
    const auto& list = s.list();
    if (!staged.empty()) {
        std::string what = staged;
        for (const auto& info : list) if (info.name == staged) what = info.when() + " (" + info.description() + ")";
        ImGui::TextColored(kBadgeColour, "Restore staged: %s. It is applied at the next start, before the game loads "
                           "your saves.", what.c_str());
        if (ImGui::Button("Cancel restore")) {
            std::string error;
            backups.message = s.cancel_restore(error) ? "Restore cancelled." : error;
        }
    }
    if (list.empty()) ImGui::TextDisabled("No save backups yet.");
    else if (ImGui::BeginTable("save_backups", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("When");
        ImGui::TableSetupColumn("Reason");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const auto& info : list) {
            ImGui::PushID(info.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(info.when().c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted((info.description() + (info.automatic ? "" : " (kept)")).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(size_text(info.bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(info.name == staged);
            if (ImGui::SmallButton("Restore")) { backups.restore_choice = info.name; backups.open_restore = true; }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(info.name == staged);
            if (ImGui::SmallButton("Delete")) { backups.delete_choice = info.name; backups.open_delete = true; }
            ImGui::EndDisabled();
            if (info.name == staged && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Staged for restore. Cancel the restore to delete it.");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // restore confirmation
    const char* title = "Restore save backup##restore_confirm";
    if (backups.open_restore) { ImGui::OpenPopup(title); backups.open_restore = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        std::string what = backups.restore_choice;
        for (const auto& info : list) if (info.name == backups.restore_choice) what = info.when() + " (" + info.description() + ")";
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextWrapped("Restore your saves from the backup of %s?", what.c_str());
        ImGui::TextWrapped("The game has its save files open while it runs, so the restore happens at the next start, "
                           "before the game loads any save. Anything you save until then is replaced.");
        ImGui::TextWrapped("Your current saves are backed up first (\"Before a restore\"), so you can undo this.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        bool accept = ImGui::Button("Restore at next start");
        ImGui::SameLine();
        bool cancel = ImGui::Button("Cancel", ImVec2(120, 0));
        ImGui::SetItemDefaultFocus();
        if (back || ImGui::IsKeyPressed(ImGuiKey_Escape)) { cancel = true; accept = false; b_used = back; }
        if (accept) {
            std::string error;
            backups.message = s.restore(backups.restore_choice, error)
                                  ? "Restore staged. Restart the game to apply it."
                                  : error;
        }
        if (accept || cancel) { backups.restore_choice.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    // delete confirmation
    const char* delete_title = "Delete save backup##delete_confirm";
    if (backups.open_delete) { ImGui::OpenPopup(delete_title); backups.open_delete = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(delete_title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        std::string what = backups.delete_choice;
        for (const auto& info : s.list()) if (info.name == backups.delete_choice) what = info.when() + " (" + info.description() + ")";
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextWrapped("Backup of %s", what.c_str());
        ImGui::TextWrapped("Delete this backup? This can't be undone.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        bool accept = ImGui::Button("Delete", ImVec2(120, 0));
        ImGui::SameLine();
        bool cancel = ImGui::Button("Cancel", ImVec2(120, 0));
        ImGui::SetItemDefaultFocus();  // keyboard and controller start on Cancel
        if (back || ImGui::IsKeyPressed(ImGuiKey_Escape)) { cancel = true; accept = false; b_used = b_used || back; }
        if (accept) {
            std::string error;
            backups.message = s.remove(backups.delete_choice, error) ? "Backup deleted." : error;
        }
        if (accept || cancel) { backups.delete_choice.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    return b_used;
}
}  // namespace overlay::experimental
