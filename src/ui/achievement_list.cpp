#include "recomp/achievements/ui/achievement_list.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <vector>

#include "recomp/achievements/ui/imgui_renderer.h"
#include "recomp/achievements/ui/sounds.h"
#include "recomp/achievements/ui/style.h"

namespace recomp::achievements::ui {

namespace {

ImVec4 ToVec(const Color& c) { return ImVec4(c.r, c.g, c.b, c.a); }

const char* TierLabel(Tier t) {
  switch (t) {
    case Tier::kBronze: return "Bronze";
    case Tier::kSilver: return "Silver";
    case Tier::kGold: return "Gold";
    case Tier::kPlatinum: return "Platinum";
    default: return "";
  }
}

std::string Lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

std::string CreditsLine(const std::vector<Credit>& credits) {
  std::string out;
  for (const auto& c : credits) {
    if (!out.empty()) out += ", ";
    out += c.name;
    if (!c.role.empty()) out += " (" + c.role + ")";
  }
  return out;
}

std::string FormatTime(uint64_t unix_seconds) {
  if (!unix_seconds) return {};
  const std::time_t t = static_cast<std::time_t>(unix_seconds);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
  return buf;
}

void DrawCredits(const std::vector<Credit>& credits) {
  for (const auto& c : credits) {
    ImGui::BulletText("%s%s%s", c.name.c_str(), c.role.empty() ? "" : " - ", c.role.c_str());
    if (!c.url.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", c.url.c_str());
    }
  }
}

}  // namespace

AchievementListWindow::AchievementListWindow(System& system, IconCache* icons) : system_(system), icons_(icons) {}

void AchievementListWindow::Draw(bool* open) {
  ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
  if (ImGui::Begin(options_.window_title, open)) DrawContents();
  ImGui::End();
}

void AchievementListWindow::DrawSettings() {
  ImGui::PushID("recomp_ach_settings");
  DrawSettingsBody();
  DrawResetConfirmation();
  ImGui::PopID();
}

void AchievementListWindow::AskReset(std::string label, std::vector<std::string> keys, std::string set_id, bool all) {
  pending_ = {std::move(label), std::move(keys), std::move(set_id), all, true};
}

void AchievementListWindow::DrawResetConfirmation() {
  const char* id = "Reset progress?##recomp_ach_reset";
  if (pending_.open) {
    ImGui::OpenPopup(id);
    pending_.open = false;
  }
  if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::Text("Reset %s?", pending_.label.c_str());
  ImGui::TextDisabled("Unlocks and progress are cleared for this profile. This can't be undone.");
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.17f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.26f, 0.22f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.30f, 0.25f, 1.0f));
  const bool confirm = ImGui::Button("Reset", ImVec2(120, 0));
  ImGui::PopStyleColor(3);
  ImGui::SameLine();
  // Focus starts on Cancel, so Enter alone never confirms.
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  const bool cancel = ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (confirm) {
    if (pending_.all) system_.Reset();
    else if (!pending_.set_id.empty()) system_.ResetSet(pending_.set_id);
    else system_.Reset(pending_.keys);
  }
  if (confirm || cancel) {
    pending_ = {};
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void AchievementListWindow::DrawSettingsBody() {
  if (options_.settings_sound) {
    ImGui::SeparatorText("Popup sound");
    PopupSoundSettings sound = GetPopupSoundSettings(system_);
    if (ImGui::Checkbox("Sound", &sound.enabled)) SetPopupSoundSettings(system_, sound);
    ImGui::BeginDisabled(!sound.enabled);
    float percent = volume_edit_ >= 0.0f ? volume_edit_ : sound.volume * 100.0f;
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderFloat("Volume", &percent, 0.0f, 100.0f, "%.0f%%")) volume_edit_ = percent;
    // Saved on release, not on every step of a drag.
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      sound.volume = std::clamp(percent, 0.0f, 100.0f) / 100.0f;
      SetPopupSoundSettings(system_, sound);
      volume_edit_ = -1.0f;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Test")) system_.TestPopup();
    ImGui::EndDisabled();
  }

  const auto all = system_.sets();
  if (options_.settings_sets && !all.empty()) {
    ImGui::SeparatorText("Achievement sets");
    ImGui::TextDisabled("Sets that are off hide their points and can't unlock. Their progress is kept.");
    for (const AchievementSet* set : all) {
      ImGui::PushID(set->id.c_str());
      bool on = system_.IsSetEnabled(set->id);
      if (ImGui::Checkbox(set->title.empty() ? set->id.c_str() : set->title.c_str(), &on))
        system_.SetSetEnabled(set->id, on);
      const auto t = system_.GetTotals(set->id);
      ImGui::SameLine();
      if (t.points) ImGui::TextDisabled("%u / %u unlocked, %u / %u points", t.unlocked, t.count, t.unlocked_points, t.points);
      else ImGui::TextDisabled("%u / %u unlocked", t.unlocked, t.count);
      ImGui::PopID();
    }
  }

  if (options_.settings_reset && !all.empty()) {
    ImGui::SeparatorText("Reset progress");
    if (ImGui::Button("Reset chosen achievements...")) {
      reset_mode_ = true;
      settings_open_ = false;
    }
    if (all.size() > 1) {
      for (const AchievementSet* set : all) {
        ImGui::PushID(set->id.c_str());
        const std::string title = set->title.empty() ? set->id : set->title;
        if (ImGui::Button(("Reset " + title + "...").c_str())) AskReset(title, {}, set->id, false);
        ImGui::PopID();
      }
    }
    if (ImGui::Button("Reset all progress...")) AskReset("all progress", {}, {}, true);
  }
}

void AchievementListWindow::DrawContents() {
  const TierStyle tier_colors;
  ImGui::PushID("recomp_ach_list");
  const bool has_settings =
      options_.show_settings && (options_.settings_sound || options_.settings_sets || options_.settings_reset);
  if (!has_settings) settings_open_ = false;
  if (settings_open_) {
    if (ImGui::Button("< Achievements")) settings_open_ = false;
    DrawSettingsBody();
    DrawResetConfirmation();
    ImGui::PopID();
    return;
  }
  // Sets that are off leave the list, the set picker and the totals.
  const auto sets = system_.enabled_sets();
  if (!selected_set_.empty() &&
      std::none_of(sets.begin(), sets.end(), [&](const AchievementSet* s) { return s->id == selected_set_; }))
    selected_set_.clear();

  // ---- Toolbar -------------------------------------------------------------
  if (sets.size() > 1) {
    const AchievementSet* cur = nullptr;
    for (auto* s : sets)
      if (s->id == selected_set_) cur = s;
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##set", cur ? cur->title.c_str() : "All sets")) {
      if (ImGui::Selectable("All sets", selected_set_.empty())) selected_set_.clear();
      for (auto* s : sets) {
        ImGui::PushID(s->id.c_str());
        if (ImGui::Selectable(s->title.empty() ? s->id.c_str() : s->title.c_str(), s->id == selected_set_))
          selected_set_ = s->id;
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
  }
  int filter = static_cast<int>(filter_);
  ImGui::RadioButton("All", &filter, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Unlocked", &filter, 1);
  ImGui::SameLine();
  ImGui::RadioButton("Locked", &filter, 2);
  filter_ = static_cast<ListFilter>(filter);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(160);
  ImGui::InputTextWithHint("##search", "Search", search_, sizeof(search_));
  if (options_.allow_reveal_secrets) {
    ImGui::SameLine();
    ImGui::Checkbox("Show secrets", &reveal_secrets_);
  }
  if (has_settings) {
    ImGui::SameLine();
    if (ImGui::Button("Settings")) {
      settings_open_ = true;
      reset_mode_ = false;
    }
  }
  if (reset_mode_) {
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "Reset mode: pick an achievement to reset.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Done")) reset_mode_ = false;
  }

  const auto totals = system_.GetTotals(selected_set_);
  ImGui::Text("%u / %u unlocked", totals.unlocked, totals.count);
  if (totals.points > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("%u / %u points", totals.unlocked_points, totals.points);
  }
  ImGui::ProgressBar(totals.count ? static_cast<float>(totals.unlocked) / totals.count : 0.0f, ImVec2(-1, 0), "");
  ImGui::Separator();

  // ---- List ------------------------------------------------------------------
  const std::string needle = Lower(search_);
  const auto views = system_.Snapshot();
  ImGui::BeginChild("##achievement_list");
  if (sets.empty() && !system_.sets().empty())
    ImGui::TextDisabled(has_settings ? "Every achievement set is off. Turn one on in Settings."
                                     : "Every achievement set is off.");
  for (const AchievementSet* set : sets) {
    if (!selected_set_.empty() && set->id != selected_set_) continue;
    ImGui::PushID(set->id.c_str());
    if (selected_set_.empty() && sets.size() > 1) {
      ImGui::SeparatorText(set->title.empty() ? set->id.c_str() : set->title.c_str());
    }
    if (options_.show_credits && !set->credits.empty()) {
      if (ImGui::TreeNode("##set_credits", "Credits (%zu)", set->credits.size())) {
        DrawCredits(set->credits);
        ImGui::TreePop();
      }
    }

    // Categories in first-seen order.
    std::vector<std::string> categories;
    std::map<std::string, std::vector<const AchievementView*>> by_category;
    for (const auto& v : views) {
      if (!v.achievement || v.achievement->set_id != set->id) continue;
      const bool unlocked = v.progress.unlocked;
      if (filter_ == ListFilter::kUnlocked && !unlocked) continue;
      if (filter_ == ListFilter::kLocked && unlocked) continue;
      const Achievement& a = *v.achievement;
      const bool hidden = a.secret && !unlocked && !reveal_secrets_;
      if (!needle.empty()) {
        const std::string hay = hidden ? Lower(options_.secret_name) : Lower(a.name + " " + a.description);
        if (hay.find(needle) == std::string::npos) continue;
      }
      if (!by_category.count(a.category)) categories.push_back(a.category);
      by_category[a.category].push_back(&v);
    }

    for (const auto& cat : categories) {
      const auto& rows = by_category[cat];
      const uint32_t done = static_cast<uint32_t>(
          std::count_if(rows.begin(), rows.end(), [](const AchievementView* v) { return v->progress.unlocked; }));
      bool open = true;
      if (categories.size() > 1 || !cat.empty()) {
        char label[256];
        std::snprintf(label, sizeof(label), "%s (%u/%zu)###cat_%s", cat.empty() ? "General" : cat.c_str(), done,
                      rows.size(), cat.c_str());
        open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
      }
      if (!open) continue;

      for (const AchievementView* v : rows) {
        const Achievement& a = *v->achievement;
        const bool unlocked = v->progress.unlocked;
        const bool hidden = a.secret && !unlocked && !reveal_secrets_;
        ImGui::PushID(a.key.c_str());
        ImGui::BeginGroup();

        // Icon (locked icon, or the normal one dimmed).
        const float isz = options_.icon_size * ImGui::GetFontSize() / 13.0f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + isz, p0.y + isz);
        void* tex = nullptr;
        bool dim = false;
        // Achievements without an icon show the fallback (the game's icon).
        if (icons_ && !hidden) {
          if (!unlocked && !a.locked_icon.empty()) tex = icons_->Get(a.locked_icon, a.set_id, set->base_dir);
          if (!tex) {
            tex = icons_->GetOrFallback(a.icon, a.set_id, set->base_dir);
            dim = !unlocked;
          }
        } else if (icons_ && hidden) {
          tex = icons_->GetOrFallback(a.locked_icon, a.set_id, set->base_dir);
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (tex) {
          dl->AddImage(ToImTextureID(tex), p0, p1, ImVec2(0, 0), ImVec2(1, 1),
                       dim ? IM_COL32(110, 110, 110, 255) : IM_COL32_WHITE);
        } else {
          // No icon and no fallback icon: the trophy, dimmed while locked.
          dl->AddRectFilled(p0, p1, ImGui::GetColorU32(unlocked ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg), 4.0f);
          DrawTrophy(dl, ImVec2(p0.x + isz * 0.5f, p0.y + isz * 0.5f), isz * 0.5f,
                     ImGui::GetColorU32(ImGuiCol_Text, unlocked ? 1.0f : 0.45f));
        }
        ImGui::Dummy(ImVec2(isz, isz));
        ImGui::SameLine();

        ImGui::BeginGroup();
        const std::string& name = hidden ? options_.secret_name : a.name;
        const std::string& desc = hidden ? (a.locked_description.empty() ? options_.secret_description
                                                                          : a.locked_description)
                                         : (!unlocked && !a.locked_description.empty() && a.secret
                                                ? a.locked_description
                                                : a.description);
        if (unlocked) ImGui::TextUnformatted(name.c_str());
        else ImGui::TextDisabled("%s", name.c_str());

        // Tier or points, to the right of the name.
        if (a.tier != Tier::kNone) {
          ImGui::SameLine();
          const Color& tc = a.tier == Tier::kBronze ? tier_colors.bronze
                            : a.tier == Tier::kSilver ? tier_colors.silver
                            : a.tier == Tier::kGold   ? tier_colors.gold
                                                      : tier_colors.platinum;
          ImGui::TextColored(ToVec(tc), "[%s]", TierLabel(a.tier));
        }
        if (a.points > 0) {
          ImGui::SameLine();
          ImGui::TextDisabled("%u pts", a.points);
        }
        if (unlocked && v->progress.unlock_time) {
          ImGui::SameLine();
          ImGui::TextDisabled("- %s", FormatTime(v->progress.unlock_time).c_str());
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();

        if (!unlocked && v->progress_total > 0) {
          char overlay[48];
          std::snprintf(overlay, sizeof(overlay), "%u / %u", v->progress_current, v->progress_total);
          ImGui::ProgressBar(static_cast<float>(v->progress_current) / v->progress_total,
                             ImVec2(std::min(260.0f, ImGui::GetContentRegionAvail().x), 0), overlay);
        }
        // Item names would spoil a hidden secret.
        if (!a.checklist.empty() && !hidden) {
          size_t got = 0;
          for (const auto& item : a.checklist)
            if (unlocked || std::find(v->progress.completed.begin(), v->progress.completed.end(), item) !=
                                v->progress.completed.end())
              ++got;
          if (ImGui::TreeNode("##checklist", "Checklist (%zu/%zu)", got, a.checklist.size())) {
            for (const auto& item : a.checklist) {
              const bool ok = unlocked || std::find(v->progress.completed.begin(), v->progress.completed.end(),
                                                    item) != v->progress.completed.end();
              if (ok) ImGui::BulletText("%s", item.c_str());
              else ImGui::TextDisabled("    %s", item.c_str());
            }
            ImGui::TreePop();
          }
        }
        if (options_.show_credits && !a.credits.empty() && !hidden)
          ImGui::TextDisabled("Credits: %s", CreditsLine(a.credits).c_str());
        if (options_.show_test_button && ImGui::SmallButton("Test popup")) system_.TestPopup(a.key);
        if (reset_mode_) {
          if (options_.show_test_button) ImGui::SameLine();
          if (ImGui::SmallButton("Reset")) AskReset("\"" + name + "\"", {a.key}, {}, false);
        }
        ImGui::EndGroup();
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::PopID();
      }
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  DrawResetConfirmation();
  ImGui::PopID();
}

}  // namespace recomp::achievements::ui
