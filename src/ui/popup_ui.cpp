#include "recomp/achievements/ui/popup_ui.h"

#include "recomp/achievements/ui/sdl_support.h"

namespace recomp::achievements::ui {

PopupUi::PopupUi(System& system, PopupUiOptions options)
    : system_(system),
      options_(std::move(options)),
      renderer_(center_, &icons_),
      list_(system, &icons_) {
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
  if (options_.sounds) sound_.Attach(center_);

  icons_.SetTextureCallbacks(options_.create_texture, options_.destroy_texture);
  icons_.SetFallbackIcon(options_.fallback_icon);
  InstallSdlImageDecoder(icons_);

  center_.Attach(system_);
}

PopupUi::~PopupUi() {
  center_.Detach();
  center_.SetSoundHandler(nullptr);
}

bool PopupUi::ReloadStyles(std::string* error) {
  const bool ok = styles_.Reload(error);
  sound_.bank().ClearCache();
  return ok;
}

void PopupUi::Draw(bool* list_open) {
  renderer_.Draw();
  if (list_open && *list_open) list_.Draw(list_open);
}

}  // namespace recomp::achievements::ui
