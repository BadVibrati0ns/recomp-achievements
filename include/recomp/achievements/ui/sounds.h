#pragma once

// Built-in popup sounds and the bank that resolves sound refs to PCM. The
// built-ins are our own procedural synthesis, written to evoke each platform's
// feel without using any of its recordings. Audio-backend free: SoundPlayer
// (sound_player.h) plays them through SDL3, or a game can take the PCM itself.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "recomp/achievements/types.h"
#include "recomp/achievements/ui/notification_center.h"

namespace recomp::achievements {
class System;
}

namespace recomp::achievements::ui {

// The player's popup sound choice for this game, kept in the System's
// settings ("popup_sound": {"enabled": true, "volume": 1.0}). The overlay and
// PopupUi apply it themselves: volume multiplies the style's, and nothing
// plays when off. Games that mix popup sounds another way read it here.
struct PopupSoundSettings {
  bool enabled = true;
  float volume = 1.0f;  // 0..1.
  bool operator==(const PopupSoundSettings&) const = default;
};
inline constexpr std::string_view kPopupSoundSetting = "popup_sound";
PopupSoundSettings GetPopupSoundSettings(const System& system);
void SetPopupSoundSettings(System& system, const PopupSoundSettings& settings);
// The gain to play a popup sound at: the style's volume times the player's,
// 0 when sound is off.
float PopupSoundGain(const PopupSoundSettings& settings, float style_volume);

constexpr int kSoundSampleRate = 48000;
constexpr int kSoundChannels = 2;

// Interleaved stereo float at kSoundSampleRate.
struct Pcm {
  std::vector<float> samples;
  size_t frames() const { return samples.size() / kSoundChannels; }
  float seconds() const { return static_cast<float>(frames()) / kSoundSampleRate; }
};

// Names usable as "builtin:<name>":
//   xbox360, ps3_bronze, ps3_silver, ps3_gold, ps3_platinum, retro, minimal,
//   classic (unlocks) and tick, retro_tick, classic_tick, ps3_tick (progress).
std::vector<std::string> BuiltinSoundNames();
// Empty Pcm for unknown names. Deterministic; peaks stay below 0.9.
Pcm SynthesizeBuiltin(std::string_view name);

// Built-in WAV decoding (PCM 8/16/24/32-bit and float, any channel count and
// rate), converted to kSoundSampleRate stereo. No SDL needed.
bool DecodeWav(std::span<const uint8_t> bytes, Pcm& out);
bool LoadWavFile(const std::filesystem::path& path, Pcm& out);

class SoundBank {
 public:
  // Decodes a WAV file to our format. Defaults to LoadWavFile; SoundPlayer
  // installs SDL_LoadWAV instead. A null loader leaves only built-ins.
  using WavLoader = std::function<bool(const std::filesystem::path& path, Pcm& out)>;

  void SetWavLoader(WavLoader loader);
  bool has_wav_loader() const;
  // Folders searched (last added first) for replacement files:
  //   <name>.wav        replaces builtin:<name> wherever a style uses it
  //   unlock.wav        replaces every unlock sound
  //   unlock_<tier>.wav replaces unlock sounds for that tier (bronze/silver/gold/platinum)
  //   progress.wav      replaces every progress sound
  void AddOverrideDir(std::filesystem::path dir);
  void ClearOverrideDirs();

  // The PCM to play for a popup, honouring override files. Null = silent.
  std::shared_ptr<const Pcm> ForEvent(NotificationKind kind, Tier tier, std::string_view ref);
  // A ref alone: "builtin:<name>", a bare builtin name, a WAV path, or ""/"none".
  std::shared_ptr<const Pcm> Get(std::string_view ref);
  void ClearCache();

 private:
  std::shared_ptr<const Pcm> LoadFile(const std::filesystem::path& path);
  std::shared_ptr<const Pcm> FindOverride(std::string_view stem);

  mutable std::mutex mutex_;
  WavLoader loader_ = LoadWavFile;
  std::vector<std::filesystem::path> override_dirs_;
  // Missing files are cached as null so they aren't stat'ed every popup.
  std::map<std::string, std::shared_ptr<const Pcm>, std::less<>> cache_;
};

}  // namespace recomp::achievements::ui
