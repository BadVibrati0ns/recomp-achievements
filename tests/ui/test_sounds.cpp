#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "recomp/achievements/ui/sounds.h"
#include "test.h"

using namespace recomp::achievements;
using namespace recomp::achievements::ui;

namespace {

struct Range {
  float min_s, max_s;
};

// Expected lengths: unlocks are about a second, ticks well under a quarter.
const std::map<std::string, Range> kLengths = {
    {"xbox360", {1.0f, 1.6f}},     {"ps3_bronze", {0.5f, 1.0f}}, {"ps3_silver", {0.6f, 1.1f}},
    {"ps3_gold", {0.8f, 1.3f}},    {"ps3_platinum", {1.2f, 2.0f}}, {"retro", {0.4f, 0.8f}},
    {"minimal", {0.5f, 1.0f}},     {"classic", {1.25f, 1.35f}},  {"tick", {0.05f, 0.2f}},
    {"retro_tick", {0.05f, 0.2f}}, {"classic_tick", {0.13f, 0.15f}}, {"ps3_tick", {0.05f, 0.25f}},
};

std::filesystem::path TempDir(const char* name) {
  auto dir = std::filesystem::temp_directory_path() / "recomp_ach_ui_tests" / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

void Touch(const std::filesystem::path& p) { std::ofstream(p) << "RIFF"; }

}  // namespace

TEST(synth_every_builtin_is_sane) {
  const auto names = BuiltinSoundNames();
  CHECK_EQ(names.size(), kLengths.size());
  for (const auto& name : names) {
    const Pcm pcm = SynthesizeBuiltin(name);
    const auto range = kLengths.find(name);
    CHECK(range != kLengths.end());
    if (range == kLengths.end()) continue;
    CHECK(!pcm.samples.empty());
    CHECK_EQ(pcm.samples.size() % 2, 0u);
    const float secs = pcm.seconds();
    if (!(secs >= range->second.min_s && secs <= range->second.max_s))
      std::printf("  %s: %.3f s\n", name.c_str(), secs);
    CHECK(secs >= range->second.min_s && secs <= range->second.max_s);

    double sum = 0.0;
    float peak = 0.0f;
    bool finite = true;
    for (float v : pcm.samples) {
      finite &= std::isfinite(v);
      peak = std::max(peak, std::abs(v));
      sum += static_cast<double>(v) * v;
    }
    const double rms = std::sqrt(sum / pcm.samples.size());
    CHECK(finite);
    CHECK(peak <= 0.9f);   // Headroom: no clipping, even summed with game audio.
    CHECK(peak >= 0.3f);   // Not near-silent.
    CHECK(rms > 0.01);
    // Starts and ends at silence, so it never clicks.
    CHECK(std::abs(pcm.samples[0]) < 0.01f && std::abs(pcm.samples[1]) < 0.01f);
    CHECK(std::abs(pcm.samples[pcm.samples.size() - 1]) < 0.01f);
    // Both channels carry sound.
    double l = 0, r = 0;
    for (size_t i = 0; i + 1 < pcm.samples.size(); i += 2) {
      l += std::abs(pcm.samples[i]);
      r += std::abs(pcm.samples[i + 1]);
    }
    CHECK(l > 0.0 && r > 0.0);
  }
}

TEST(synth_is_deterministic_and_distinct) {
  CHECK(SynthesizeBuiltin("xbox360").samples == SynthesizeBuiltin("builtin:xbox360").samples);
  CHECK(SynthesizeBuiltin("retro").samples == SynthesizeBuiltin("retro").samples);
  const auto bronze = SynthesizeBuiltin("ps3_bronze").samples;
  const auto silver = SynthesizeBuiltin("ps3_silver").samples;
  const auto gold = SynthesizeBuiltin("ps3_gold").samples;
  const auto plat = SynthesizeBuiltin("ps3_platinum").samples;
  CHECK(bronze != silver && silver != gold && gold != plat);
  CHECK(plat.size() > bronze.size());  // Platinum rings longest.
  CHECK(SynthesizeBuiltin("not_a_sound").samples.empty());
}

TEST(bank_builtins_are_cached) {
  SoundBank bank;
  auto a = bank.Get("builtin:xbox360");
  auto b = bank.Get("xbox360");
  CHECK(a != nullptr);
  CHECK(a == b);
  CHECK(bank.Get("none") == nullptr);
  CHECK(bank.Get("") == nullptr);
  CHECK(bank.Get("builtin:nope") == nullptr);
  CHECK(bank.Get("/no/such/file.wav") == nullptr);  // No loader: files are silent.
  CHECK(bank.ForEvent(NotificationKind::kUnlock, Tier::kNone, "none") == nullptr);
}

TEST(bank_override_files) {
  const auto game = TempDir("bank_game");
  const auto user = TempDir("bank_user");
  Touch(game / "xbox360.wav");
  Touch(user / "xbox360.wav");
  Touch(user / "unlock_gold.wav");
  Touch(game / "progress.wav");
  Touch(game / "custom.wav");

  SoundBank bank;
  std::vector<std::string> loaded;
  bank.SetWavLoader([&](const std::filesystem::path& p, Pcm& out) {
    loaded.push_back(p.string());
    // Encode which file this was in the length.
    const size_t frames = 100 + p.string().size();
    out.samples.assign(frames * 2, 0.1f);
    return true;
  });
  bank.AddOverrideDir(game);
  bank.AddOverrideDir(user);  // Added last: searched first.

  auto x = bank.Get("builtin:xbox360");
  CHECK(x != nullptr);
  CHECK_EQ(x->frames(), 100 + (user / "xbox360.wav").string().size());

  auto gold = bank.ForEvent(NotificationKind::kUnlock, Tier::kGold, "builtin:ps3_gold");
  CHECK_EQ(gold->frames(), 100 + (user / "unlock_gold.wav").string().size());
  auto bronze = bank.ForEvent(NotificationKind::kUnlock, Tier::kBronze, "builtin:ps3_bronze");
  CHECK(bronze != nullptr);
  CHECK(bronze->samples == SynthesizeBuiltin("ps3_bronze").samples);  // No override for bronze.

  auto tick = bank.ForEvent(NotificationKind::kProgress, Tier::kNone, "builtin:tick");
  CHECK_EQ(tick->frames(), 100 + (game / "progress.wav").string().size());

  auto custom = bank.Get((game / "custom.wav").string());
  CHECK(custom != nullptr);
  CHECK(bank.Get((game / "missing.wav").string()) == nullptr);

  const size_t loads = loaded.size();
  bank.Get("builtin:xbox360");
  bank.Get((game / "custom.wav").string());
  CHECK_EQ(loaded.size(), loads);  // Cached.

  Touch(user / "unlock.wav");
  bank.ClearCache();
  auto any = bank.ForEvent(NotificationKind::kUnlock, Tier::kNone, "builtin:minimal");
  CHECK_EQ(any->frames(), 100 + (user / "unlock.wav").string().size());
}

TEST(bank_failed_loader_falls_back_silent) {
  const auto dir = TempDir("bank_fail");
  Touch(dir / "retro.wav");
  SoundBank bank;
  bank.SetWavLoader([](const std::filesystem::path&, Pcm&) { return false; });
  bank.AddOverrideDir(dir);
  // A broken override doesn't silence the built-in.
  auto r = bank.Get("retro");
  CHECK(r != nullptr);
  CHECK(r->samples == SynthesizeBuiltin("retro").samples);
}

namespace {

std::vector<uint8_t> MakeWav(uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits,
                             const std::vector<uint8_t>& data, bool extensible = false) {
  std::vector<uint8_t> out;
  auto u16 = [&](uint16_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); };
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); };
  const uint32_t fmt_size = extensible ? 40 : 16;
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  u32(4 + 8 + fmt_size + 8 + uint32_t(data.size()));
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(fmt_size);
  u16(extensible ? 0xFFFE : format);
  u16(channels);
  u32(rate);
  u32(rate * channels * bits / 8);
  u16(uint16_t(channels * bits / 8));
  u16(bits);
  if (extensible) {
    u16(22); u16(bits); u32(0);
    u16(format);
    for (int i = 0; i < 14; ++i) out.push_back(0);
  }
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  u32(uint32_t(data.size()));
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

}  // namespace

TEST(builtin_wav_decoder_converts_to_stereo_48k) {
  // 16-bit mono at 24 kHz: two frames, full scale and half scale.
  const std::vector<uint8_t> data = {0xFF, 0x7F, 0x00, 0x40};
  Pcm pcm;
  CHECK(DecodeWav(MakeWav(1, 1, 24000, 16, data), pcm));
  CHECK_EQ(pcm.frames(), 3u);  // 2 frames at half the rate -> 3 output frames.
  CHECK(std::fabs(pcm.samples[0] - 32767 / 32768.0f) < 1e-4f);
  CHECK(pcm.samples[0] == pcm.samples[1]);  // Mono on both sides.
  CHECK(std::fabs(pcm.samples[4] - 0.5f) < 1e-4f);

  // Float stereo at 48 kHz, wrapped in WAVE_FORMAT_EXTENSIBLE.
  std::vector<uint8_t> f(8);
  const float l = -0.25f, r = 0.75f;
  std::memcpy(f.data(), &l, 4);
  std::memcpy(f.data() + 4, &r, 4);
  Pcm fpcm;
  CHECK(DecodeWav(MakeWav(3, 2, 48000, 32, f, true), fpcm));
  CHECK_EQ(fpcm.frames(), 1u);
  CHECK(fpcm.samples[0] == l && fpcm.samples[1] == r);

  Pcm bad;
  CHECK(!DecodeWav(std::vector<uint8_t>{'R', 'I', 'F', 'F'}, bad));
  CHECK(!DecodeWav(MakeWav(2, 1, 8000, 4, {0, 0}), bad));  // ADPCM isn't supported.
}

TEST(sound_bank_loads_wav_overrides_without_sdl) {
  const auto dir = std::filesystem::temp_directory_path() / "recomp_ach_ui_tests" / "wav";
  std::filesystem::create_directories(dir);
  const auto wav = MakeWav(1, 1, 48000, 16, {0x00, 0x40, 0x00, 0x40});
  std::ofstream(dir / "unlock.wav", std::ios::binary).write(reinterpret_cast<const char*>(wav.data()), wav.size());
  SoundBank bank;
  CHECK(bank.has_wav_loader());
  bank.AddOverrideDir(dir);
  auto pcm = bank.ForEvent(NotificationKind::kUnlock, Tier::kNone, "builtin:xbox360");
  CHECK(pcm != nullptr);
  if (pcm) CHECK_EQ(pcm->frames(), 2u);
}
