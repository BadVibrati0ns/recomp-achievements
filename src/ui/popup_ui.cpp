#include "recomp/achievements/ui/popup_ui.h"

#include <imgui.h>

#include "recomp/achievements/ui/sdl_support.h"

namespace recomp::achievements::ui {

PopupUi::PopupUi(System& system, PopupUiOptions options)
    : system_(system),
      options_(std::move(options)),
      renderer_(center_, &icons_),
      list_(system, &icons_),
      toggle_({options_.toggle_key, options_.toggle_pad, options_.chord_grace}) {
  std::filesystem::path global_dir;
  if (options_.global_preferences) {
    global_dir = options_.create_global_preferences ? EnsureGlobalConfigDir() : GlobalConfigDir();
  }
  if (!options_.game_style_file.empty()) styles_.LoadGameStyleFile(options_.game_style_file);
  if (!global_dir.empty()) styles_.LoadGlobalStyleFile(global_dir / "achievements_style.json");
  if (!options_.user_dir.empty()) styles_.LoadUserStyleFile(options_.user_dir / "achievements_style.json");
  center_.UseStyles(styles_);

  // Later folders win: the game's, then the player's global ones, then this game's user folder.
  for (const auto& dir : options_.sound_dirs) sound_.bank().AddOverrideDir(dir);
  if (!global_dir.empty()) sound_.bank().AddOverrideDir(global_dir / "sounds");
  if (!options_.user_dir.empty()) sound_.bank().AddOverrideDir(options_.user_dir / "sounds");
  if (options_.sounds) {
    // The player's popup sound choice (the settings views) over the style's volume.
    center_.SetSoundHandler([this](const NotificationCenter::SoundRequest& r) {
      const float gain = PopupSoundGain(GetPopupSoundSettings(system_), r.volume);
      if (gain <= 0.0f) return;
      NotificationCenter::SoundRequest scaled = r;
      scaled.volume = gain;
      sound_.Play(scaled);
    });
  }

  icons_.SetTextureCallbacks(options_.create_texture, options_.destroy_texture);
  icons_.SetFallbackIcon(options_.fallback_icon);
  InstallSdlImageDecoder(icons_);

  center_.Attach(system_);
  library_ = MakeLibrary(system_, options_.library, icons_, options_.fallback_icon);
}

PopupUi::~PopupUi() {
  library_.reset();
  center_.Detach();
  center_.SetSoundHandler(nullptr);
}

bool PopupUi::ReloadStyles(std::string* error) {
  const bool ok = styles_.Reload(error);
  sound_.bank().ClearCache();
  return ok;
}

void PopupUi::ApplyAppearance() {
  const uint64_t gen = system_.generation();
  if (gen == appearance_generation_) return;
  appearance_generation_ = gen;
  styles_.SetPlayerAppearance(GetAppearanceSettings(system_));
}

bool PopupUi::HandleInput(float dt, const OverlayInput& input) {
  ApplyAppearance();
  // The player's "viewer" keys, re-read when the styles change.
  if (styles_.generation() != bindings_generation_) {
    bindings_generation_ = styles_.generation();
    toggle_.ApplyStyle(styles_.Resolve(nullptr)->viewer);
  }
  const bool toggled = toggle_.Update(dt, input, list_open_);
  if (toggled) SetListOpen(!list_open_);
  // B / Esc closes it too, as in the overlay, unless one of its dialogs is
  // open (ImGui closes that first).
  else if (list_open_ && (input.pad_b || input.escape) && !back_held_ && !list_.settings_open() &&
           !(ImGui::GetCurrentContext() && ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)))
    SetListOpen(false);
  back_held_ = input.pad_b || input.escape;
  return toggled;
}

OverlayInput PopupUi::GameInput(const OverlayInput& raw) const { return toggle_.GameInput(raw, list_open_); }

void PopupUi::SetListOpen(bool open) {
  if (open == list_open_) return;
  list_open_ = open;
  if (!open) toggle_.Closed();
}

void PopupUi::Draw() {
  ApplyAppearance();
  renderer_.Draw();
  if (!list_open_) return;
  bool open = true;
  list_.Draw(&open);
  if (!open) SetListOpen(false);  // Its close button.
}

void PopupUi::Draw(bool* list_open) {
  ApplyAppearance();
  renderer_.Draw();
  if (list_open && *list_open) list_.Draw(list_open);
}

}  // namespace recomp::achievements::ui
