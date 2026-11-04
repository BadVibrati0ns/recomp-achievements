// Drives ui::Overlay with a scripted input sequence over a stand-in game
// frame, without a GPU or Dear ImGui, and writes raw RGB24 frames to stdout
// plus a WAV of the popup sounds:
//
//   overlay_demo audio.wav > video.rgb
//   ffmpeg -f rawvideo -pix_fmt rgb24 -s 1280x720 -r 30 -i video.rgb
//       -i audio.wav -c:v libx264 -pix_fmt yuv420p -c:a aac overlay.mp4
//   (the WAV is written last, so not as one pipe; overlay_demo audio.wav
//   <width> <height> for another size)
//
// The script browses the list, then the settings page: volume, a set turned
// off and on, a reset cancelled, and one achievement reset by holding A.
//
//   overlay_demo --bench    times list and popup frames at 1920x1080

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "recomp/achievements/system.h"
#include "recomp/achievements/ui/overlay.h"
#include "recomp/achievements/ui/sounds.h"

using namespace recomp::achievements;
using namespace recomp::achievements::ui;

namespace {

constexpr int kFps = 30;

// A 64x64 badge: a two-colour gradient disc with a ring, per index.
Image MakeIcon(int n) {
  Image img;
  img.width = img.height = 64;
  img.rgba.resize(64 * 64 * 4);
  const float hue = n * 0.61803f;
  auto channel = [&](float off) { return 0.5f + 0.5f * std::cos(6.2832f * (hue + off)); };
  const float r = channel(0.0f), g = channel(0.33f), b = channel(0.67f);
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      uint8_t* p = &img.rgba[(y * 64 + x) * 4];
      const float dx = x - 31.5f, dy = y - 31.5f, d = std::sqrt(dx * dx + dy * dy);
      const float t = (x + y) / 126.0f;
      const bool ring = d > 20 && d < 25;
      const bool star = std::fabs(dx) + std::fabs(dy) < 13 + 4 * std::sin(n + 0.5f);
      float k = ring ? 1.0f : star ? 0.95f : 0.35f + 0.4f * t;
      p[0] = static_cast<uint8_t>(std::clamp((r * k + (star ? 0.4f : 0)) * 255.0f, 0.0f, 255.0f));
      p[1] = static_cast<uint8_t>(std::clamp((g * k + (star ? 0.4f : 0)) * 255.0f, 0.0f, 255.0f));
      p[2] = static_cast<uint8_t>(std::clamp((b * k + (star ? 0.4f : 0)) * 255.0f, 0.0f, 255.0f));
      p[3] = 255;
    }
  }
  return img;
}

Achievement Make(const char* id, const char* name, const char* desc, uint32_t points, const char* category = "") {
  Achievement a;
  a.local_id = id;
  a.name = name;
  a.description = desc;
  a.points = points;
  a.category = category;
  return a;
}

void AddSets(System& system) {
  AchievementSet xbox;
  xbox.id = "skater";
  xbox.title = "Skater (Xbox 360)";
  xbox.platform = Platform::kXbox360;
  xbox.credits = {{"The recomp-achievements authors", "demo set", "https://example.invalid/demo"}};
  const char* names[] = {"Street Legend", "Rail Rider", "Big Air", "Combo Breaker", "Wall Crawler", "Night Owl",
                         "Street Legend", "Park Rat", "Bail Master", "Photo Op", "Sponsor Me", "Hall of Fame"};
  const char* descs[] = {"Finish the story.",
                         "Grind 100 m in one go.",
                         "Land a jump over 10 m high.",
                         "Chain 20 tricks into one combo without bailing.",
                         "Wallride across the plaza from one end to the other.",
                         "Skate every spot in the city after dark.",
                         "Own every spot in the city by beating its challenge.",
                         "Spend an hour in the skate park.",
                         "Bail 100 times. It happens.",
                         "Make the cover of the magazine.",
                         "Get sponsored by a board company.",
                         "Reach the Hall of Fame."};
  for (int i = 0; i < 12; ++i) {
    Achievement a = Make(("a" + std::to_string(i)).c_str(), names[i], descs[i], 10 + 5 * (i % 4),
                         i < 6 ? "Career" : "Challenges");
    a.icon = "gen:" + std::to_string(i);
    a.platform_id = static_cast<uint32_t>(i + 1);
    xbox.achievements.push_back(a);
  }
  // A secret, and a checklist in progress.
  xbox.achievements[5].secret = true;
  xbox.achievements[5].locked_description = "Something happens after midnight.";
  {
    Achievement& a = xbox.achievements[3];
    a.name = "Flip Trick Encyclopedia";
    a.description = "Land every flip trick at least once.";
    a.trigger.type = "checklist";
    a.trigger.event = "trick";
    a.checklist = {"Kickflip", "Heelflip", "Varial Kickflip", "360 Flip", "Hardflip", "Inward Heelflip",
                   "Laser Flip", "Impossible", "Casper Flip"};
    a.credits = {{"The recomp-achievements authors", "trigger", ""}};
  }
  system.AddSet(xbox);

  AchievementSet ps;
  ps.id = "trophies";
  ps.title = "Trophies (PS3)";
  ps.platform = Platform::kPlayStation3;
  const Tier tiers[] = {Tier::kPlatinum, Tier::kGold, Tier::kSilver, Tier::kSilver, Tier::kBronze, Tier::kBronze};
  const char* pnames[] = {"Untouchable", "Gold Standard", "Silver Lining", "Second Wind", "First Steps", "Tourist"};
  for (int i = 0; i < 6; ++i) {
    Achievement a = Make(("t" + std::to_string(i)).c_str(), pnames[i], "A trophy from the PS3 list.", 0);
    a.tier = tiers[i];
    a.icon = "gen:" + std::to_string(20 + i);
    a.platform_id = static_cast<uint32_t>(i);
    ps.achievements.push_back(a);
  }
  system.AddSet(ps);

  // A mod pack with no icons: the trophy is drawn.
  AchievementSet mod;
  mod.id = "mod_pack";
  mod.title = "Community Pack";
  mod.credits = {{"The recomp-achievements authors", "pack", ""}};
  mod.achievements = {
      Make("tour", "City Tour", "Visit all five districts in one session.", 10, "Exploration"),
      Make("lost", "Off the Map", "Find the hidden spot behind the stadium.", 15, "Exploration"),
      Make("grind", "Grind Encyclopedia", "Grind every rail type.", 25, "Tricks"),
      Make("manual", "Manual Labour", "Hold a manual for 30 seconds.", 20, "Tricks"),
      Make("secret", "The Long Way Home", "Skate from the docks to the old school without stopping.", 50,
           "Tricks"),
  };
  mod.achievements[4].secret = true;
  {
    Achievement& a = mod.achievements[2];
    a.trigger.type = "checklist";
    a.trigger.event = "grind";
    a.checklist = {"50-50", "5-0", "Boardslide", "Lipslide", "Crooked", "Smith", "Feeble", "Nosegrind"};
  }
  system.AddSet(mod);
}

// One scripted press: which button, when, and for how long.
struct Press {
  float at, hold;
  std::function<void(OverlayInput&)> set;
};

void WriteWav(const char* path, const std::vector<float>& track) {
  std::ofstream f(path, std::ios::binary);
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  const uint32_t bytes = static_cast<uint32_t>(track.size() * 2);
  f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(kSoundChannels);
  u32(kSoundSampleRate); u32(kSoundSampleRate * kSoundChannels * 2); u16(kSoundChannels * 2); u16(16);
  f.write("data", 4); u32(bytes);
  for (float s : track) {
    const auto v = static_cast<int16_t>(std::clamp(s, -1.0f, 1.0f) * 32767);
    f.write(reinterpret_cast<const char*>(&v), 2);
  }
}

// The stand-in game frame: a dusk gradient with a few buildings.
void GameFrame(std::vector<float>& fb, int w, int h, float t) {
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float* d = &fb[(static_cast<size_t>(y) * w + x) * 3];
      const float v = static_cast<float>(y) / h;
      d[0] = 0.20f + 0.25f * v;
      d[1] = 0.22f + 0.10f * v;
      d[2] = 0.34f - 0.10f * v;
      const float bx = std::fmod(x + t * 40.0f, 160.0f);
      const float top = h * (0.55f + 0.15f * std::sin(std::floor((x + t * 40.0f) / 160.0f) * 1.7f));
      if (y > top && bx < 120) d[0] = d[1] = d[2] = 0.10f + 0.03f * (bx < 6);
      if (y > h * 0.86f) d[0] = d[1] = d[2] = 0.16f;
    }
  }
}

int Bench() {
  System system;
  AddSets(system);
  for (const char* t : {"Kickflip", "Heelflip", "360 Flip"}) system.ReportEvent("trick", t);
  system.Unlock("skater:a0");
  system.Unlock("skater:a1");
  OverlayOptions opt;
  opt.global_preferences = false;
  Overlay overlay(system, opt);
  overlay.icons().AddProvider("gen", [](const IconRequest& r, IconData& out) {
    out.image = MakeIcon(std::atoi(std::string(r.body).c_str()));
    return IconResult::kOk;
  });
  overlay.SetSize(1920, 1080);
  OverlayInput in;
  overlay.Frame(0.0f, in);
  overlay.Open();
  for (int i = 0; i < 20; ++i) overlay.Frame(0.05f, in);  // Finish the open animation.
  // Moving the selection redraws the whole viewer.
  double list_ms = 0;
  const int n = 60;
  for (int i = 0; i < n; ++i) {
    in.pad_down = i % 2 == 0;
    const auto t0 = std::chrono::steady_clock::now();
    overlay.Frame(1.0f / 60, in);
    (void)overlay.image();  // Includes the straight-alpha conversion.
    list_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  in = {};
  int idle_redraws = 0;
  double idle_ms = 0;
  for (int i = 0; i < 300; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    idle_redraws += overlay.Frame(1.0f / 60, in);
    idle_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  overlay.Close();
  for (int i = 0; i < 20; ++i) overlay.Frame(0.05f, in);
  // Popups only.
  system.TestPopup("skater:a2");
  double popup_ms = 0;
  int popup_frames = 0, redraws = 0;
  for (int i = 0; i < 60 * 7; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool drew = overlay.Frame(1.0f / 60, in);
    if (drew) (void)overlay.image();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (drew) popup_ms += ms, ++redraws;
    ++popup_frames;
  }
  double closed_ms = 0;
  int closed_redraws = 0;
  for (int i = 0; i < 1000; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    closed_redraws += overlay.Frame(1.0f / 60, in);
    closed_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  std::printf("1920x1080 list frame (selection moves): %.2f ms avg over %d\n", list_ms / n, n);
  std::printf("list open, nothing changing: %d redraws in 300 frames, %.4f ms avg\n", idle_redraws, idle_ms / 300);
  std::printf("popup frames: %d of %d redrew, %.3f ms avg per redraw\n", redraws, popup_frames,
              redraws ? popup_ms / redraws : 0.0);
  std::printf("closed, no popups: %d redraws in 1000 frames, %.4f ms avg\n", closed_redraws, closed_ms / 1000);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--bench") == 0) return Bench();
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: overlay_demo <audio.wav> [width height [seconds]] > video.rgb   |   overlay_demo --bench\n"
                 "  with seconds, only the frame at that time is written\n");
    return 2;
  }
  const int w = argc >= 4 ? std::atoi(argv[2]) : 1280;
  const int h = argc >= 4 ? std::atoi(argv[3]) : 720;
  const float still = argc >= 5 ? static_cast<float>(std::atof(argv[4])) : -1.0f;

  System system;
  AddSets(system);
  system.ReportEvent("trick", "Kickflip");
  system.ReportEvent("trick", "Heelflip");
  system.Unlock("skater:a0");
  system.Unlock("skater:a2");
  system.Unlock("trophies:t4");
  system.Unlock("mod_pack:tour");

  std::vector<float> soundtrack;
  float now = 0.0f;
  OverlayOptions opt;
  opt.global_preferences = false;  // Reproducible: ignore this machine's preferences.
  opt.play_sound = [&](std::shared_ptr<const Pcm> pcm, float volume, NotificationKind) {
    const size_t start = static_cast<size_t>(now * kSoundSampleRate) * kSoundChannels;
    if (soundtrack.size() < start + pcm->samples.size()) soundtrack.resize(start + pcm->samples.size(), 0.0f);
    for (size_t i = 0; i < pcm->samples.size(); ++i) soundtrack[start + i] += pcm->samples[i] * volume;
  };
  Overlay overlay(system, opt);
  overlay.icons().AddProvider("gen", [](const IconRequest& r, IconData& out) {
    out.image = MakeIcon(std::atoi(std::string(r.body).c_str()));
    return IconResult::kOk;
  });
  overlay.SetSize(w, h);

  auto pad = [](bool OverlayInput::*b) { return [b](OverlayInput& in) { in.*b = true; }; };
  std::vector<Press> script = {
      // Start held, then LB: the chord opens the viewer.
      {7.0f, 0.9f, pad(&OverlayInput::pad_start)},
      {7.35f, 0.5f, pad(&OverlayInput::pad_lb)},
      {8.6f, 0.1f, pad(&OverlayInput::pad_down)},
      {9.0f, 0.1f, pad(&OverlayInput::pad_down)},
      {9.4f, 0.1f, pad(&OverlayInput::pad_down)},
      {10.2f, 0.1f, pad(&OverlayInput::pad_rb)},  // Skater set.
      {11.2f, 0.1f, pad(&OverlayInput::pad_x)},   // Unlocked.
      {12.0f, 0.1f, pad(&OverlayInput::pad_x)},   // Locked.
      {12.8f, 0.1f, pad(&OverlayInput::pad_x)},   // All; the checklist row stays selected.
      {13.8f, 0.1f, pad(&OverlayInput::pad_a)},   // Expand the checklist.
      {16.4f, 1.1f, pad(&OverlayInput::pad_down)},  // Held: repeats and scrolls.
      {18.0f, 0.6f, [](OverlayInput& in) { in.pad_ry = -0.8f; }},
      {19.0f, 0.1f, pad(&OverlayInput::pad_y)},  // Show secrets.
      {20.0f, 0.1f, pad(&OverlayInput::pad_back)},  // Credits.
      {21.8f, 0.1f, pad(&OverlayInput::pad_b)},
      {22.5f, 0.1f, [](OverlayInput& in) { in.tab = true; }},  // Keyboard: hints switch.
      {23.2f, 0.1f, [](OverlayInput& in) { in.right = true; }},
      {24.0f, 0.1f, [](OverlayInput& in) { in.right = true; }},
      {25.0f, 0.1f, pad(&OverlayInput::pad_rb)},
      // LB first, then Start: closes without leaving the set changed.
      {27.0f, 0.6f, pad(&OverlayInput::pad_lb)},
      {27.25f, 0.4f, pad(&OverlayInput::pad_start)},

      // Settings: Start on the open viewer.
      {29.0f, 0.7f, pad(&OverlayInput::pad_start)},
      {29.3f, 0.4f, pad(&OverlayInput::pad_lb)},
      {30.6f, 0.1f, pad(&OverlayInput::pad_start)},
      {31.8f, 0.1f, pad(&OverlayInput::pad_down)},   // Volume.
      {32.6f, 0.1f, pad(&OverlayInput::pad_left)},   // 90%, with a preview tick.
      {33.2f, 0.1f, pad(&OverlayInput::pad_left)},   // 80%.
      {33.8f, 0.1f, [](OverlayInput& in) { in.left = true; }},  // 70%, from the keyboard.
      {34.8f, 0.1f, pad(&OverlayInput::pad_down)},
      {35.2f, 0.1f, pad(&OverlayInput::pad_down)},
      {35.6f, 0.1f, pad(&OverlayInput::pad_down)},   // Community Pack.
      {36.6f, 0.1f, pad(&OverlayInput::pad_a)},      // Off.
      {38.2f, 0.1f, pad(&OverlayInput::pad_b)},      // The list: its tab and points are gone.
      {39.6f, 0.1f, pad(&OverlayInput::pad_rb)},
      {40.4f, 0.1f, pad(&OverlayInput::pad_lb)},
      {41.4f, 0.1f, pad(&OverlayInput::pad_start)},
      {42.4f, 0.1f, pad(&OverlayInput::pad_a)},      // On again, progress intact.
      {43.4f, 0.1f, pad(&OverlayInput::pad_down)},   // Reset chosen achievements...
      {43.8f, 0.1f, pad(&OverlayInput::pad_down)},
      {44.2f, 0.1f, pad(&OverlayInput::pad_down)},
      {44.6f, 0.1f, pad(&OverlayInput::pad_down)},   // Reset Community Pack.
      {45.4f, 0.1f, pad(&OverlayInput::pad_a)},      // A tap does nothing...
      {46.2f, 0.7f, pad(&OverlayInput::pad_a)},      // ...and letting go early cancels.
      {47.6f, 0.1f, pad(&OverlayInput::pad_up)},
      {48.0f, 0.1f, pad(&OverlayInput::pad_up)},
      {48.4f, 0.1f, pad(&OverlayInput::pad_up)},
      {49.2f, 0.1f, pad(&OverlayInput::pad_a)},      // Reset mode.
      {50.2f, 1.6f, pad(&OverlayInput::pad_a)},      // Held to the end: Street Legend is reset.
      {53.0f, 0.1f, pad(&OverlayInput::pad_b)},      // Back to the settings.
      {54.2f, 0.1f, pad(&OverlayInput::pad_b)},
      {55.0f, 0.1f, pad(&OverlayInput::pad_b)},
  };
  struct Event {
    float at;
    std::function<void()> fn;
  };
  std::vector<Event> events = {
      {1.0f, [&] { system.ReportEvent("trick", "Varial Kickflip"); }},
      {2.2f, [&] { system.Unlock("skater:a1"); }},
      {4.4f, [&] { system.Unlock("mod_pack:manual"); }},
      {5.0f, [&] { system.Unlock("trophies:t2"); }},  // PS3 look, top right.
      {23.6f, [&] { system.Unlock("mod_pack:lost"); }},  // While the viewer is open.
      {37.4f, [&] { system.Unlock("mod_pack:grind"); }},  // Its set is off: no popup, no unlock.
  };
  const float seconds = 56.5f;
  const int frames = static_cast<int>(seconds * kFps);
  std::vector<float> fb(static_cast<size_t>(w) * h * 3);
  std::vector<uint8_t> out(fb.size());
  int uploads = 0;
  for (int f = 0; f < frames; ++f) {
    now = static_cast<float>(f) / kFps;
    for (auto& e : events)
      if (e.fn && now >= e.at) {
        e.fn();
        e.fn = nullptr;
      }
    OverlayInput in;
    for (const auto& p : script)
      if (now >= p.at && now < p.at + p.hold) p.set(in);
    uploads += overlay.Frame(1.0f / kFps, in);

    if (still >= 0 && std::fabs(now - still) > 0.5f / kFps) continue;
    GameFrame(fb, w, h, now);
    // What a game's blend does: straight alpha over the frame.
    const Image& img = overlay.image();
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
      const uint8_t* s = &img.rgba[i * 4];
      const float a = s[3] / 255.0f;
      for (int c = 0; c < 3; ++c) {
        const float v = fb[i * 3 + c] * (1 - a) + s[c] / 255.0f * a;
        out[i * 3 + c] = static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
      }
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
  }
  soundtrack.resize(static_cast<size_t>(seconds * kSoundSampleRate) * kSoundChannels, 0.0f);
  WriteWav(argv[1], soundtrack);
  const auto st = overlay.stats();
  std::fprintf(stderr, "%d frames, %d uploads, %llu renders, last %.2f ms\n", frames, uploads,
               static_cast<unsigned long long>(st.renders), st.last_render_ms);
  return 0;
}
