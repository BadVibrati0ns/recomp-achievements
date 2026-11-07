#include "recomp/achievements/ui/sounds.h"

#include <algorithm>
#include <cmath>

#include "recomp/achievements/system.h"

namespace recomp::achievements::ui {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRate = static_cast<float>(kSoundSampleRate);

// A stereo buffer with a few voices we build every sound from.
struct Synth {
  std::vector<float> l, r;
  explicit Synth(float seconds)
      : l(static_cast<size_t>(seconds * kRate), 0.0f), r(l.size(), 0.0f) {}
  size_t frames() const { return l.size(); }

  void Add(size_t i, float v, float pan) {
    // Constant-power pan, pan in -1..1.
    const float a = (pan + 1.0f) * 0.25f * kPi;
    l[i] += v * std::cos(a);
    r[i] += v * std::sin(a);
  }

  // A struck bell: inharmonic partials, each with its own decay.
  struct Partial {
    float ratio, amp, decay_scale;
  };
  void Bell(float start, float freq, float amp, float decay, float pan,
            std::initializer_list<Partial> partials, float attack = 0.004f, float detune = 0.0f) {
    const size_t s = static_cast<size_t>(start * kRate);
    for (size_t i = s; i < frames(); ++i) {
      const float t = static_cast<float>(i - s) / kRate;
      const float env_a = std::min(1.0f, t / attack);
      float v = 0.0f;
      for (const auto& p : partials) {
        const float env = env_a * std::exp(-t / (decay * p.decay_scale));
        if (env < 1e-5f) continue;
        v += p.amp * env * std::sin(2.0f * kPi * (freq * p.ratio + detune) * t);
      }
      if (v == 0.0f && t > decay * 4.0f) break;
      Add(i, amp * v, pan);
    }
  }

  // Sine whose pitch glides exponentially from f0 to f1 over `glide` seconds.
  void Glide(float start, float len, float f0, float f1, float glide, float amp, float attack,
             float decay, float pan) {
    const size_t s = static_cast<size_t>(start * kRate);
    const size_t e = std::min(frames(), s + static_cast<size_t>(len * kRate));
    float phase = 0.0f;
    for (size_t i = s; i < e; ++i) {
      const float t = static_cast<float>(i - s) / kRate;
      const float k = std::min(1.0f, t / glide);
      const float f = f0 * std::pow(f1 / f0, k);
      phase += 2.0f * kPi * f / kRate;
      const float env = std::min(1.0f, t / attack) * std::exp(-t / decay);
      Add(i, amp * env * std::sin(phase), pan);
    }
  }

  // Band-limited pulse from odd harmonics (no aliasing fizz), for chip tones.
  void Square(float start, float len, float freq, float amp, float decay, float pan) {
    const size_t s = static_cast<size_t>(start * kRate);
    const size_t e = std::min(frames(), s + static_cast<size_t>(len * kRate));
    const int harmonics = std::max(1, static_cast<int>(9000.0f / freq));
    for (size_t i = s; i < e; ++i) {
      const float t = static_cast<float>(i - s) / kRate;
      const float release = std::min(1.0f, static_cast<float>(e - i) / (0.004f * kRate));
      const float env = std::min(1.0f, t / 0.002f) * std::exp(-t / decay) * release;
      float v = 0.0f;
      for (int h = 1; h <= harmonics; h += 2) v += std::sin(2.0f * kPi * freq * h * t) / h;
      Add(i, amp * env * v, pan);
    }
  }

  void Noise(float start, float len, float amp, float pan, uint32_t seed, float lp_hz,
             bool swell) {
    const size_t s = static_cast<size_t>(start * kRate);
    const size_t e = std::min(frames(), s + static_cast<size_t>(len * kRate));
    const float wc = 2.0f * kPi * lp_hz / kRate;
    const float alpha = wc / (1.0f + wc);
    float lp = 0.0f;
    for (size_t i = s; i < e; ++i) {
      seed = seed * 1664525u + 1013904223u;
      const float n = static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f;
      lp += alpha * (n - lp);
      const float k = static_cast<float>(i - s) / static_cast<float>(e - s);
      const float env = swell ? std::sin(k * kPi) : (1.0f - k);
      Add(i, amp * env * lp, pan);
    }
  }

  // Normalises to `peak`, with short fades so nothing clicks.
  Pcm Finish(float peak) {
    float max = 0.0f;
    for (size_t i = 0; i < frames(); ++i) max = std::max({max, std::abs(l[i]), std::abs(r[i])});
    const float gain = max > 0.0f ? peak / max : 0.0f;
    const size_t fade_in = static_cast<size_t>(0.001f * kRate);
    const size_t fade_out = std::min(frames(), static_cast<size_t>(0.02f * kRate));
    Pcm out;
    out.samples.resize(frames() * 2);
    for (size_t i = 0; i < frames(); ++i) {
      float g = gain;
      if (i < fade_in) g *= static_cast<float>(i) / fade_in;
      if (i >= frames() - fade_out) g *= static_cast<float>(frames() - i) / fade_out;
      out.samples[i * 2] = l[i] * g;
      out.samples[i * 2 + 1] = r[i] * g;
    }
    return out;
  }
};

// Glassy FM-ish bell partials.
constexpr std::initializer_list<Synth::Partial> kGlass = {
    {1.0f, 1.0f, 1.0f}, {2.0f, 0.35f, 0.6f}, {2.76f, 0.18f, 0.4f}, {5.4f, 0.06f, 0.2f}};
// Softer, rounder bell (fewer upper partials).
constexpr std::initializer_list<Synth::Partial> kSoft = {
    {1.0f, 1.0f, 1.0f}, {2.0f, 0.2f, 0.5f}, {3.0f, 0.06f, 0.3f}};

// Xbox 360-like: a soft rising "bloop" into a bright two-note chime.
Pcm Xbox360() {
  Synth s(1.35f);
  s.Noise(0.0f, 0.16f, 0.12f, 0.0f, 1234567u, 2500.0f, true);
  s.Glide(0.0f, 0.22f, 190.0f, 470.0f, 0.12f, 0.55f, 0.01f, 0.08f, 0.0f);
  s.Bell(0.11f, 987.77f, 0.45f, 0.55f, -0.25f, kGlass, 0.003f);   // B5
  s.Bell(0.16f, 1318.51f, 0.38f, 0.65f, 0.25f, kGlass, 0.003f);   // E6
  s.Bell(0.16f, 659.25f, 0.18f, 0.5f, 0.0f, kSoft, 0.006f);       // E5 body
  return s.Finish(0.85f);
}

// PS3-like trophy "ping": one clear tone; higher tiers are brighter and richer.
Pcm Ps3(int tier) {
  switch (tier) {
    case 0: {  // Bronze
      Synth s(0.75f);
      s.Bell(0.0f, 783.99f, 0.6f, 0.30f, 0.0f, kSoft, 0.008f);  // G5
      return s.Finish(0.75f);
    }
    case 1: {  // Silver
      Synth s(0.9f);
      s.Bell(0.0f, 880.0f, 0.6f, 0.36f, 0.0f, kSoft, 0.008f);   // A5
      s.Bell(0.0f, 1760.0f, 0.12f, 0.25f, 0.3f, kSoft, 0.01f);
      return s.Finish(0.78f);
    }
    case 2: {  // Gold
      Synth s(1.1f);
      s.Bell(0.0f, 1046.5f, 0.5f, 0.35f, -0.15f, kGlass, 0.006f);  // C6
      s.Bell(0.08f, 1567.98f, 0.45f, 0.45f, 0.15f, kGlass, 0.006f);  // G6
      return s.Finish(0.8f);
    }
    default: {  // Platinum
      Synth s(1.6f);
      const float notes[] = {1046.5f, 1318.51f, 1567.98f, 2093.0f};  // C6 E6 G6 C7
      for (int i = 0; i < 4; ++i) {
        const float pan = -0.4f + 0.27f * i;
        s.Bell(0.065f * i, notes[i], 0.4f, 0.55f, pan, kGlass, 0.005f);
        s.Bell(0.065f * i, notes[i], 0.12f, 0.6f, -pan, kGlass, 0.005f, 2.5f);  // Chorus.
      }
      return s.Finish(0.85f);
    }
  }
}

// RetroAchievements-like: a quick chip arpeggio.
Pcm Retro() {
  Synth s(0.55f);
  const float notes[] = {523.25f, 659.25f, 783.99f};  // C5 E5 G5
  for (int i = 0; i < 3; ++i) s.Square(0.05f * i, 0.05f, notes[i], 0.25f, 1.0f, 0.0f);
  s.Square(0.15f, 0.38f, 1046.5f, 0.28f, 0.14f, 0.0f);  // C6, held
  return s.Finish(0.7f);
}

Pcm Minimal() {
  Synth s(0.8f);
  s.Bell(0.0f, 659.25f, 0.5f, 0.28f, -0.1f, kSoft, 0.006f);  // E5
  s.Bell(0.07f, 987.77f, 0.45f, 0.35f, 0.1f, kSoft, 0.006f);  // B5
  return s.Finish(0.7f);
}

Pcm Tick() {
  Synth s(0.12f);
  s.Bell(0.0f, 1244.5f, 0.6f, 0.025f, 0.0f, kSoft, 0.001f);
  s.Noise(0.0f, 0.006f, 0.25f, 0.0f, 42u, 6000.0f, false);
  return s.Finish(0.5f);
}

Pcm RetroTick() {
  Synth s(0.11f);
  s.Square(0.0f, 0.04f, 880.0f, 0.25f, 1.0f, 0.0f);
  s.Square(0.04f, 0.06f, 1318.51f, 0.25f, 0.05f, 0.0f);
  return s.Finish(0.5f);
}

Pcm Ps3Tick() {
  Synth s(0.18f);
  s.Bell(0.0f, 1760.0f, 0.5f, 0.04f, 0.0f, kSoft, 0.003f);
  return s.Finish(0.45f);
}

// ---- The classic theme's sounds ---------------------------------------------

float WhiteNoise(uint32_t& seed) {
  seed = seed * 1664525u + 1013904223u;
  return static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f;
}

// Board landing thump, a score whoosh and a warm low chord, lowpassed below 1.1 kHz.
Pcm Classic() {
  const int total = static_cast<int>(kRate * 1.30f);
  Synth s(1.30f);
  uint32_t seed = 987654321u;
  const float wc = 2.0f * kPi * 1100.0f / kRate;
  const float a = wc / (1.0f + wc);
  float lp_l = 0.0f, lp_r = 0.0f;
  struct Tone {
    float freq, amp, decay, pan_l, pan_r;
  };
  static const Tone kChord[] = {
      {261.63f, 0.42f, 0.95f, 0.95f, 0.90f}, {329.63f, 0.38f, 0.85f, 0.85f, 0.95f},
      {392.00f, 0.35f, 0.75f, 1.00f, 0.85f}, {523.25f, 0.28f, 0.65f, 0.88f, 0.98f},
      {659.25f, 0.18f, 0.50f, 0.95f, 0.92f},
  };
  for (int i = 0; i < total; ++i) {
    const float t = static_cast<float>(i) / kRate;
    float l = 0.0f, r = 0.0f;
    if (t < 0.08f) {
      const float thump = 0.65f * std::exp(-t / 0.026f) *
                          std::sin(2.0f * kPi * (42.0f + 63.0f * std::exp(-t / 0.018f)) * t);
      const float wood = 0.42f * std::exp(-t / 0.038f) * std::sin(2.0f * kPi * 195.0f * t);
      const float wheel =
          0.28f * std::exp(-t / 0.022f) * WhiteNoise(seed) * std::sin(2.0f * kPi * 360.0f * t);
      l += thump * 0.95f + wood + wheel * 0.9f;
      r += thump * 0.95f + wood * 0.9f + wheel;
    }
    if (t >= 0.025f && t < 0.16f) {
      const float wt = (t - 0.025f) / 0.135f;
      const float w = 0.26f * std::sin(wt * kPi) *
                      (std::sin(2.0f * kPi * (190.0f + 250.0f * wt * wt) * t) + WhiteNoise(seed) * 0.15f);
      l += w * (1.05f - 0.4f * wt);
      r += w * (0.65f + 0.4f * wt);
    }
    if (t >= 0.08f) {
      const float bt = t - 0.08f;
      const float attack = std::min(1.0f, bt / 0.014f);
      for (const auto& c : kChord) {
        const float env = attack * std::exp(-bt / c.decay);
        const float pl = 2.0f * kPi * (c.freq - 0.25f) * bt;
        const float pr = 2.0f * kPi * (c.freq + 0.25f) * bt;
        l += env * c.amp * (std::sin(pl) + 0.15f * std::sin(2.0f * pl)) * c.pan_l;
        r += env * c.amp * (std::sin(pr) + 0.15f * std::sin(2.0f * pr)) * c.pan_r;
      }
    }
    lp_l += a * (l - lp_l);
    lp_r += a * (r - lp_r);
    s.l[i] = std::tanh(lp_l * 1.05f);
    s.r[i] = std::tanh(lp_r * 1.05f);
  }
  return s.Finish(0.78f);
}

// A soft wooden "thoc" with a marimba tick.
Pcm ClassicTick() {
  const int total = static_cast<int>(kRate * 0.14f);
  Synth s(0.14f);
  const float wc = 2.0f * kPi * 950.0f / kRate;
  const float a = wc / (1.0f + wc);
  float lp_l = 0.0f, lp_r = 0.0f;
  for (int i = 0; i < total; ++i) {
    const float t = static_cast<float>(i) / kRate;
    float l = 0.0f, r = 0.0f;
    if (t < 0.028f) {
      const float f = t / 0.028f;
      const float pop = 0.45f * (1.0f - f) * std::sin(2.0f * kPi * (340.0f - 180.0f * f) * t);
      l += pop * 0.95f;
      r += pop * 0.95f;
    }
    if (t >= 0.006f) {
      const float bt = t - 0.006f;
      const float attack = std::min(1.0f, bt / 0.004f);
      const float w1 = 0.30f * attack * std::exp(-bt / 0.048f) * std::sin(2.0f * kPi * 440.0f * bt);
      const float w2 = 0.15f * attack * std::exp(-bt / 0.032f) * std::sin(2.0f * kPi * 659.25f * bt);
      l += w1 * 0.95f + w2 * 0.85f;
      r += w1 * 0.85f + w2 * 0.95f;
    }
    lp_l += a * (l - lp_l);
    lp_r += a * (r - lp_r);
    s.l[i] = std::tanh(lp_l * 1.10f);
    s.r[i] = std::tanh(lp_r * 1.10f);
  }
  return s.Finish(0.65f);
}

std::string_view StripBuiltin(std::string_view ref) {
  constexpr std::string_view kPrefix = "builtin:";
  return ref.substr(0, kPrefix.size()) == kPrefix ? ref.substr(kPrefix.size()) : ref;
}

bool IsPath(std::string_view ref) {
  return ref.substr(0, 8) != "builtin:" && ref.find_first_of("/\\.") != std::string_view::npos;
}

const char* TierStem(Tier tier) {
  switch (tier) {
    case Tier::kBronze: return "unlock_bronze";
    case Tier::kSilver: return "unlock_silver";
    case Tier::kGold: return "unlock_gold";
    case Tier::kPlatinum: return "unlock_platinum";
    default: return nullptr;
  }
}

}  // namespace

std::vector<std::string> BuiltinSoundNames() {
  return {"xbox360", "ps3_bronze", "ps3_silver", "ps3_gold", "ps3_platinum", "retro", "minimal",
          "classic", "tick",    "retro_tick", "classic_tick", "ps3_tick"};
}

Pcm SynthesizeBuiltin(std::string_view name) {
  name = StripBuiltin(name);
  if (name == "xbox360") return Xbox360();
  if (name == "ps3_bronze" || name == "ps3") return Ps3(0);
  if (name == "ps3_silver") return Ps3(1);
  if (name == "ps3_gold") return Ps3(2);
  if (name == "ps3_platinum") return Ps3(3);
  if (name == "retro") return Retro();
  if (name == "minimal") return Minimal();
  if (name == "classic") return Classic();
  if (name == "tick") return Tick();
  if (name == "retro_tick") return RetroTick();
  if (name == "classic_tick") return ClassicTick();
  if (name == "ps3_tick") return Ps3Tick();
  return {};
}

// ---- SoundBank -------------------------------------------------------------------

void SoundBank::SetWavLoader(WavLoader loader) {
  std::lock_guard lock(mutex_);
  loader_ = std::move(loader);
  cache_.clear();
}

bool SoundBank::has_wav_loader() const {
  std::lock_guard lock(mutex_);
  return static_cast<bool>(loader_);
}

void SoundBank::AddOverrideDir(std::filesystem::path dir) {
  std::lock_guard lock(mutex_);
  override_dirs_.push_back(std::move(dir));
  cache_.clear();
}

void SoundBank::ClearOverrideDirs() {
  std::lock_guard lock(mutex_);
  override_dirs_.clear();
  cache_.clear();
}

void SoundBank::ClearCache() {
  std::lock_guard lock(mutex_);
  cache_.clear();
}

std::shared_ptr<const Pcm> SoundBank::LoadFile(const std::filesystem::path& path) {
  const std::string key = "file:" + path.string();
  if (auto it = cache_.find(key); it != cache_.end()) return it->second;
  std::shared_ptr<const Pcm> result;
  std::error_code ec;
  if (loader_ && std::filesystem::is_regular_file(path, ec)) {
    auto pcm = std::make_shared<Pcm>();
    if (loader_(path, *pcm) && !pcm->samples.empty()) result = std::move(pcm);
  }
  cache_.emplace(key, result);
  return result;
}

std::shared_ptr<const Pcm> SoundBank::FindOverride(std::string_view stem) {
  if (!loader_) return nullptr;
  const std::string key = "override:" + std::string(stem);
  if (auto it = cache_.find(key); it != cache_.end()) return it->second;
  std::shared_ptr<const Pcm> result;
  for (auto it = override_dirs_.rbegin(); it != override_dirs_.rend() && !result; ++it)
    result = LoadFile(*it / (std::string(stem) + ".wav"));
  cache_.emplace(key, result);
  return result;
}

std::shared_ptr<const Pcm> SoundBank::Get(std::string_view ref) {
  std::lock_guard lock(mutex_);
  if (ref.empty() || ref == "none") return nullptr;
  if (IsPath(ref)) return LoadFile(std::filesystem::path(std::string(ref)));
  const std::string_view name = StripBuiltin(ref);
  if (auto o = FindOverride(name)) return o;
  const std::string key = "builtin:" + std::string(name);
  if (auto it = cache_.find(key); it != cache_.end()) return it->second;
  Pcm pcm = SynthesizeBuiltin(name);
  std::shared_ptr<const Pcm> result;
  if (!pcm.samples.empty()) result = std::make_shared<const Pcm>(std::move(pcm));
  cache_.emplace(key, result);
  return result;
}

std::shared_ptr<const Pcm> SoundBank::ForEvent(NotificationKind kind, Tier tier, std::string_view ref) {
  if (ref.empty() || ref == "none") return nullptr;  // The style asked for silence.
  {
    std::lock_guard lock(mutex_);
    if (kind == NotificationKind::kUnlock) {
      if (const char* stem = TierStem(tier))
        if (auto o = FindOverride(stem)) return o;
      if (auto o = FindOverride("unlock")) return o;
    } else if (auto o = FindOverride("progress")) {
      return o;
    }
  }
  return Get(ref);
}

PopupSoundSettings GetPopupSoundSettings(const System& system) {
  PopupSoundSettings out;
  const nlohmann::json j = system.GetSetting(kPopupSoundSetting);
  if (!j.is_object()) return out;
  if (const auto it = j.find("enabled"); it != j.end() && it->is_boolean()) out.enabled = it->get<bool>();
  if (const auto it = j.find("volume"); it != j.end() && it->is_number())
    out.volume = std::clamp(it->get<float>(), 0.0f, 1.0f);
  return out;
}

void SetPopupSoundSettings(System& system, const PopupSoundSettings& settings) {
  // Two decimals: repeated 10% steps shouldn't save as 0.30000001.
  const double volume = std::round(std::clamp(settings.volume, 0.0f, 1.0f) * 100.0) / 100.0;
  system.SetSetting(kPopupSoundSetting, {{"enabled", settings.enabled}, {"volume", volume}});
}

float PopupSoundGain(const PopupSoundSettings& settings, float style_volume) {
  return settings.enabled ? std::clamp(style_volume, 0.0f, 1.0f) * std::clamp(settings.volume, 0.0f, 1.0f) : 0.0f;
}

}  // namespace recomp::achievements::ui
