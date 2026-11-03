#include "recomp/achievements/ui/popup_ui.h"

#include "recomp/achievements/ui/sdl_support.h"

namespace recomp::achievements::ui {

PopupUi::PopupUi(System& system, PopupUiOptions options)
    : system_(system),
      options_(std::move(options)),
      renderer_(center_, &icons_),
      list_(system, &icons_) {
  if (!options_.game_style_file.empty()) styles_.LoadGameStyleFile(options_.game_style_file);
  if (!options_.user_dir.empty()) styles_.LoadUserStyleFile(options_.user_dir / "achievements_style.json");
  center_.UseStyles(styles_);

  for (const auto& dir : options_.sound_dirs) sound_.bank().AddOverrideDir(dir);
  // Added last so the user's files win over the game's.
  if (!options_.user_dir.empty()) sound_.bank().AddOverrideDir(options_.user_dir / "sounds");
  if (options_.sounds) sound_.Attach(center_);

  icons_.SetTextureCallbacks(options_.create_texture, options_.destroy_texture);
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
