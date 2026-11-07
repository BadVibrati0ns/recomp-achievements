// Headless ImGui frames through the real renderer, list window and PopupUi,
// plus the SDL3 loaders when built with SDL3. Built only with an imgui target.

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>

#include "imgui.h"
#include "imgui_internal.h"
#include "recomp/achievements/system.h"
#include "recomp/achievements/ui/achievement_list.h"
#include "recomp/achievements/ui/imgui_renderer.h"
#include "recomp/achievements/ui/popup_ui.h"
#include "recomp/achievements/ui/sdl_support.h"
#include "recomp/achievements/ui/sound_player.h"
#include "test.h"

#if RECOMP_ACH_HAS_SDL3
#include <SDL3/SDL.h>
#endif

using namespace recomp::achievements;
using namespace recomp::achievements::ui;

namespace {

struct Context {
  Context() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920, 1080);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
#if IMGUI_VERSION_NUM >= 19200
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
#else
    unsigned char* px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
#endif
  }
  ~Context() { ImGui::DestroyContext(); }
};

int Frame(const std::function<void()>& body) {
  ImGui::NewFrame();
  body();
  ImGui::Render();
  return ImGui::GetDrawData()->TotalVtxCount;
}

AchievementSet MakeSet(const char* id, Platform platform) {
  AchievementSet s;
  s.id = id;
  s.title = std::string("Set ") + id;
  s.platform = platform;
  s.credits = {{"Someone", "author", "https://example.invalid"}};
  for (int i = 0; i < 4; ++i) {
    Achievement a;
    a.local_id = "a" + std::to_string(i);
    a.name = "Achievement number " + std::to_string(i) + " with a fairly long name to force truncation";
    a.description = "A description long enough that it has to wrap onto a second line in the popup card, "
                    "and then some more so it must be cut with an ellipsis.";
    a.points = 10 * i;
    a.icon = "fake:" + std::to_string(i);
    a.category = i < 2 ? "Story" : "Tricks";
    if (platform == Platform::kPlayStation3) a.tier = static_cast<Tier>(1 + i);
    if (i == 3) {
      a.secret = true;
      a.locked_description = "Hidden.";
    }
    if (i == 2) {
      a.trigger.type = "checklist";
      a.trigger.event = "trick";
      a.checklist = {"Kickflip", "Heelflip", "Varial"};
      a.credits = {{"Helper", "trigger", ""}};
    }
    s.achievements.push_back(a);
  }
  return s;
}

}  // namespace

TEST(imgui_every_theme_draws) {
  Context ctx;
  int created = 0;
  IconCache icons;
  icons.SetTextureCallbacks([&](int, int, const uint8_t*) { return reinterpret_cast<void*>(uintptr_t(++created)); });
  icons.AddProvider("fake", [](const IconRequest& r, IconData& out) {
    if (r.body == "1") return IconResult::kMissing;  // Exercise the fallback badge.
    out.image.width = out.image.height = 8;
    out.image.rgba.assign(8 * 8 * 4, 200);
    return IconResult::kOk;
  });

  for (const auto& theme : BuiltinThemeNames()) {
    NotificationCenter center;
    StyleManager styles;
    styles.SetThemeOverride(theme);
    center.UseStyles(styles);
    ImGuiRenderer renderer(center, &icons);
    auto set = MakeSet("s", Platform::kPlayStation3);
    for (size_t i = 0; i < set.achievements.size(); ++i) {
      auto& a = set.achievements[i];
      a.key = "s:" + a.local_id;
      a.set_id = "s";
      UnlockEvent ue;
      ue.view.achievement = &a;
      ue.view.set = &set;
      center.OnUnlock(ue);
      ProgressEvent pe;
      pe.view.achievement = &a;
      pe.view.set = &set;
      pe.view.progress_current = static_cast<uint32_t>(i + 1);
      pe.view.progress_total = 14;
      pe.item = "Kickflip";
      center.OnProgress(pe);
    }
    int max_vtx = 0;
    bool inside = true;
    for (int f = 0; f < 60 * 30; ++f) {
      const int vtx = Frame([&] {
        renderer.Draw();
        for (const auto& p : center.Active()) {
          const PopupLayout L = renderer.Layout(p, ImVec2(0, 0), ImVec2(1920, 1080));
          if (p.phase == Phase::kHold)
            inside &= L.min.x >= 0 && L.min.y >= 0 && L.max.x <= 1920 && L.max.y <= 1080 &&
                      L.max.x > L.min.x && L.max.y > L.min.y;
        }
      });
      max_vtx = std::max(max_vtx, vtx);
    }
    if (max_vtx == 0) std::printf("  theme %s drew nothing\n", theme.c_str());
    CHECK(max_vtx > 0);
    CHECK(inside);
    CHECK(!center.Busy());  // Everything played out within 30 s.
  }
  CHECK(created > 0);
}

TEST(imgui_layout_follows_anchor_and_scale) {
  Context ctx;
  NotificationCenter center;
  auto style = std::make_shared<Style>(BuiltinTheme("minimal"));
  style->unlock.animation = Animation::kNone;
  ImGuiRenderer renderer(center, nullptr);
  const Anchor anchors[] = {Anchor::kTopLeft, Anchor::kTopRight, Anchor::kBottomCenter, Anchor::kMiddleRight};
  for (Anchor anchor : anchors) {
    style->unlock.anchor = anchor;
    style->unlock.offset_x = 20;
    style->unlock.offset_y = 30;
    Notification n;
    n.key = "k";
    n.name = "Name";
    n.style = style;
    center.Clear();
    center.Push(n);
    center.Update(0.0f);
    const auto act = center.Active();
    CHECK_EQ(act.size(), 1u);
    if (act.empty()) continue;
    ImGui::NewFrame();
    const PopupLayout L = renderer.Layout(act[0], ImVec2(0, 0), ImVec2(3840, 2160));  // Scale 2.
    ImGui::EndFrame();
    CHECK_NEAR(L.scale, 2.0, 1e-5);
    CHECK_NEAR(L.max.x - L.min.x, style->unlock.width * 2, 1e-3);
    switch (anchor) {
      case Anchor::kTopLeft:
        CHECK_NEAR(L.min.x, 40, 1e-3);
        CHECK_NEAR(L.min.y, 60, 1e-3);
        break;
      case Anchor::kTopRight:
        CHECK_NEAR(L.max.x, 3840 - 40, 1e-3);
        CHECK_NEAR(L.min.y, 60, 1e-3);
        break;
      case Anchor::kBottomCenter:
        CHECK_NEAR((L.min.x + L.max.x) * 0.5f, 1920 + 40, 1e-3);
        CHECK_NEAR(L.max.y, 2160 - 60, 1e-3);
        break;
      default:
        CHECK_NEAR(L.max.x, 3840 - 40, 1e-3);
        CHECK_NEAR((L.min.y + L.max.y) * 0.5f, 1080 + 60, 1e-3);
        break;
    }
  }
}

TEST(imgui_custom_draw_replaces_default) {
  Context ctx;
  NotificationCenter center;
  ImGuiRenderer renderer(center, nullptr);
  int calls = 0;
  renderer.SetCustomDraw([&](ImDrawList*, const ActivePopup& p, const PopupLayout&) {
    ++calls;
    return p.n.key == "mine";  // Others fall back to the built-in look.
  });
  auto style = std::make_shared<Style>(BuiltinTheme("retro"));
  Notification a;
  a.key = "mine";
  a.style = style;
  center.Push(a);
  center.Update(0.0f);
  center.Update(0.3f);
  const int custom_vtx = Frame([&] { renderer.Draw(false); });
  CHECK_EQ(calls, 1);
  CHECK_EQ(custom_vtx, 0);  // Nothing drawn by us.
  Notification b;
  b.key = "theirs";
  b.style = style;
  center.Push(b);
  center.Update(0.0f);
  center.Update(0.3f);  // Past its fade-in start.
  const int vtx = Frame([&] { renderer.Draw(false); });
  CHECK(vtx > 0);
}

TEST(imgui_list_window_and_popup_ui) {
  Context ctx;
  System system;
  system.AddSet(MakeSet("x", Platform::kXbox360));
  system.AddSet(MakeSet("p", Platform::kPlayStation3));
  system.ReportEvent("trick", "Kickflip");
  system.Unlock("x:a0");

  const auto dir = std::filesystem::temp_directory_path() / "recomp_ach_ui_tests" / "popup_ui";
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "achievements_style.json") << R"({"theme": "classic", "sound": {"enabled": false}})";

  PopupUiOptions opt;
  opt.user_dir = dir;
  opt.create_texture = [](int, int, const uint8_t*) { return reinterpret_cast<void*>(uintptr_t(1)); };
  PopupUi ui(system, opt);
  ui.list().options().show_test_button = true;
  CHECK(ui.styles().Resolve("x", Platform::kXbox360)->theme == "classic");

  system.Unlock("p:a1");
  system.ReportEvent("trick", "Heelflip");
  bool open = true;
  int vtx = 0;
  for (int f = 0; f < 30; ++f) vtx = std::max(vtx, Frame([&] { ui.Draw(&open); }));
  CHECK(vtx > 0);
  CHECK(ui.center().Busy());

  // Every filter and the secret toggle render without tripping ImGui asserts.
  AchievementListWindow list(system, &ui.icons());
  for (int f = 0; f < 3; ++f) Frame([&] { list.Draw(); });
}

namespace {

// Moves the mouse over the window named `window` until the item with `id` is
// hovered, then clicks it. ImGui has no item lookup by label, so this sweeps.
bool Click(const std::function<void()>& body, ImGuiID id, const char* window = "w") {
  ImGuiIO& io = ImGui::GetIO();
  const ImGuiWindow* w = ImGui::FindWindowByName(window);
  if (!w) return false;
  const ImRect r = w->Rect();
  for (float y = r.Min.y + 2; y < r.Max.y; y += 3) {
    for (float x = r.Min.x + 12; x < std::min(r.Max.x, r.Min.x + 400); x += 24) {
      io.AddMousePosEvent(x, y);
      Frame(body);
      if (GImGui->HoveredId != id) continue;
      io.AddMouseButtonEvent(0, true);
      Frame(body);
      io.AddMouseButtonEvent(0, false);
      Frame(body);
      io.AddMousePosEvent(-1, -1);
      Frame(body);
      return true;
    }
  }
  return false;
}

ImGuiID SettingsId(std::initializer_list<const char*> path) {
  ImGuiID id = ImHashStr("w");  // The test window.
  id = ImHashStr("recomp_ach_settings", 0, id);
  for (const char* p : path) id = ImHashStr(p, 0, id);
  return id;
}

constexpr const char* kModal = "Reset progress?##recomp_ach_reset";

ImGuiID ModalId(const char* label) { return ImHashStr(label, 0, ImHashStr(kModal)); }

}  // namespace

TEST(imgui_settings_sets_sound_and_resets) {
  Context ctx;
  System system;
  system.AddSet(MakeSet("x", Platform::kXbox360));
  system.AddSet(MakeSet("p", Platform::kPlayStation3));
  system.Unlock("x:a0");
  system.Unlock("p:a1");
  AchievementListWindow list(system);
  const auto body = [&] {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(900, 1000));
    ImGui::Begin("w", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
    list.DrawSettings();
    ImGui::End();
  };
  for (int f = 0; f < 3; ++f) Frame(body);

  // A set's checkbox turns it off; the list and totals follow.
  CHECK(Click(body, SettingsId({"p", "Set p"})));
  CHECK(!system.IsSetEnabled("p"));
  CHECK_EQ(system.GetTotals().count, 4u);
  CHECK(Click(body, SettingsId({"p", "Set p"})));
  CHECK(system.IsSetEnabled("p"));

  // Sound off.
  CHECK(Click(body, SettingsId({"Sound"})));
  CHECK(!GetPopupSoundSettings(system).enabled);

  // A reset asks first: one click only opens the confirmation.
  CHECK(Click(body, SettingsId({"Reset all progress..."})));
  CHECK(system.IsUnlocked("x:a0"));
  CHECK(Click(body, ModalId("Cancel"), kModal));
  CHECK(system.IsUnlocked("x:a0"));
  // One set, confirmed.
  CHECK(Click(body, SettingsId({"x", "Reset Set x..."})));
  CHECK(system.IsUnlocked("x:a0"));
  CHECK(Click(body, ModalId("Reset"), kModal));
  CHECK(!system.IsUnlocked("x:a0"));
  CHECK(system.IsUnlocked("p:a1"));
  // Everything.
  CHECK(Click(body, SettingsId({"Reset all progress..."})));
  CHECK(Click(body, ModalId("Reset"), kModal));
  CHECK(!system.IsUnlocked("p:a1"));

  // Reset mode hands over to the list, with a Reset button per row.
  system.Unlock("x:a1");
  CHECK(Click(body, SettingsId({"Reset chosen achievements..."})));
  CHECK(list.reset_mode());
  const auto contents = [&] {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(900, 1000));
    ImGui::Begin("w", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
    list.DrawContents();
    ImGui::End();
  };
  for (int f = 0; f < 3; ++f) Frame(contents);
  // Sets that are off leave the list window; with all off it says so.
  system.SetSetEnabled("x", false);
  system.SetSetEnabled("p", false);
  for (int f = 0; f < 3; ++f) Frame(contents);
  list.SetSettingsOpen(true);
  for (int f = 0; f < 3; ++f) Frame(contents);
  CHECK(list.settings_open());
}

#if RECOMP_ACH_HAS_SDL3

namespace {

// 16-bit mono WAV, a sine at `rate`.
void WriteWav(const std::filesystem::path& path, int rate, float seconds) {
  const uint32_t frames = static_cast<uint32_t>(rate * seconds);
  std::vector<int16_t> data(frames);
  for (uint32_t i = 0; i < frames; ++i) data[i] = static_cast<int16_t>(12000 * std::sin(i * 0.05));
  std::ofstream f(path, std::ios::binary);
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  f.write("RIFF", 4);
  u32(36 + frames * 2);
  f.write("WAVEfmt ", 8);
  u32(16);
  u16(1);
  u16(1);
  u32(rate);
  u32(rate * 2);
  u16(2);
  u16(16);
  f.write("data", 4);
  u32(frames * 2);
  f.write(reinterpret_cast<const char*>(data.data()), frames * 2);
}

}  // namespace

TEST(sdl_wav_override_and_playback) {
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  const auto dir = std::filesystem::temp_directory_path() / "recomp_ach_ui_tests" / "sdl_wav";
  std::filesystem::create_directories(dir);
  WriteWav(dir / "xbox360.wav", 22050, 0.5f);

  SoundPlayer player;
  CHECK(SoundPlayer::available());
  CHECK(player.bank().has_wav_loader());
  player.bank().AddOverrideDir(dir);
  auto pcm = player.bank().Get("builtin:xbox360");
  CHECK(pcm != nullptr);
  if (pcm) CHECK_NEAR(pcm->seconds(), 0.5, 0.01);  // Resampled to 48 kHz stereo.
  CHECK(player.Play("builtin:xbox360", 0.5f));
  CHECK(player.Play("builtin:ps3_gold", 0.5f, NotificationKind::kProgress));
  player.SetMuted(true);
  CHECK(!player.Play("builtin:tick"));
}

TEST(sdl_image_decoder) {
  IconCache cache;
  CHECK(InstallSdlImageDecoder(cache));
  SDL_Surface* s = SDL_CreateSurface(3, 2, SDL_PIXELFORMAT_RGBA32);
  CHECK(s != nullptr);
  if (!s) return;
  static_cast<uint8_t*>(s->pixels)[0] = 0xAB;  // R of pixel (0, 0)
  static_cast<uint8_t*>(s->pixels)[3] = 0xFF;
  int created = 0;
  int w = 0, h = 0;
  uint8_t first = 0;
  cache.SetTextureCallbacks([&](int tw, int th, const uint8_t* rgba) {
    ++created;
    w = tw;
    h = th;
    first = rgba[0];
    return reinterpret_cast<void*>(uintptr_t(created));
  });
  auto save = [&](bool png, std::vector<uint8_t>& out) {
    SDL_IOStream* io = SDL_IOFromDynamicMem();
#if SDL_VERSION_ATLEAST(3, 4, 0)
    const bool ok = png ? SDL_SavePNG_IO(s, io, false) : SDL_SaveBMP_IO(s, io, false);
#else
    const bool ok = png ? false : SDL_SaveBMP_IO(s, io, false);
#endif
    const Sint64 size = SDL_GetIOSize(io);
    out.resize(static_cast<size_t>(std::max<Sint64>(0, size)));
    SDL_SeekIO(io, 0, SDL_IO_SEEK_SET);
    SDL_ReadIO(io, out.data(), out.size());
    SDL_CloseIO(io);
    return ok;
  };
  for (bool png : {true, false}) {
    std::vector<uint8_t> bytes;
    if (!save(png, bytes)) {
      std::printf("  skipping %s: SDL can't write it\n", png ? "PNG" : "BMP");
      continue;
    }
    const std::string scheme = png ? "png" : "bmp";
    cache.AddProvider(scheme, [bytes](const IconRequest&, IconData& out) {
      out.bytes = bytes;
      return IconResult::kOk;
    });
    w = h = 0;
    CHECK(cache.Get(scheme + ":x") != nullptr);
    CHECK_EQ(w, 3);
    CHECK_EQ(h, 2);
    CHECK_EQ(first, 0xAB);
  }
  SDL_DestroySurface(s);
}

#endif

// ---- PopupUi's list toggle: the same rules as the software overlay ----------------

namespace {

struct ToggleRig {
  Context ctx;
  System system;
  std::unique_ptr<PopupUi> ui;
  ToggleRig() {
    system.AddSet(MakeSet("t", Platform::kCustom));
    PopupUiOptions o;
    o.global_preferences = false;
    o.sounds = false;
    ui = std::make_unique<PopupUi>(system, o);
  }
  // One frame: input, then the ImGui frame with the list.
  OverlayInput Step(const OverlayInput& in = {}) {
    ui->HandleInput(1.0f / 60, in);
    Frame([&] { ui->Draw(); });
    return ui->GameInput(in);
  }
};

OverlayInput Keys(const std::function<void(OverlayInput&)>& set) {
  OverlayInput in;
  set(in);
  return in;
}

}  // namespace

TEST(popup_ui_f8_opens_and_closes_the_list) {
  ToggleRig r;
  CHECK(!r.ui->list_open());
  const auto f8 = Keys([](OverlayInput& in) { in.f[8] = true; });
  OverlayInput game = r.Step(f8);
  CHECK(r.ui->list_open());
  CHECK(!game.f[8]);
  CHECK(r.ui->wants_input());
  r.Step();
  // Keys reach the list, not the game.
  game = r.Step(Keys([](OverlayInput& in) { in.down = true; }));
  CHECK(!game.down);
  r.Step(f8);
  CHECK(!r.ui->list_open());
  // Still latched while F8 is held after closing; free once released.
  CHECK(r.ui->wants_input());
  r.Step();
  CHECK(!r.ui->wants_input());
  game = r.Step(Keys([](OverlayInput& in) { in.down = true; }));
  CHECK(game.down);
}

TEST(popup_ui_chord_and_grace) {
  ToggleRig r;
  // LB alone is held back from the game for the grace time, then let through.
  OverlayInput lb = Keys([](OverlayInput& in) { in.pad_lb = true; });
  OverlayInput game = r.Step(lb);
  CHECK(!game.pad_lb);
  for (int i = 0; i < 12; ++i) game = r.Step(lb);
  CHECK(game.pad_lb);
  r.Step();
  // LB then Start: the chord opens the list, and the game never saw LB.
  game = r.Step(lb);
  CHECK(!game.pad_lb);
  r.Step(Keys([](OverlayInput& in) { in.pad_lb = in.pad_start = true; }));
  CHECK(r.ui->list_open());
  // B closes it, as in the overlay.
  r.Step();
  r.Step(Keys([](OverlayInput& in) { in.pad_b = true; }));
  CHECK(!r.ui->list_open());
  r.Step();
  // A quick tap of LB (shorter than the grace) reaches the game as a tap.
  r.Step(lb);
  game = r.Step();
  CHECK(game.pad_lb);
}

TEST(popup_ui_player_rebinds_the_toggle) {
  ToggleRig r;
  r.ui->styles().SetGlobalStyle(nlohmann::json::parse(R"({"viewer": {"key": "F9", "pad": "back+rb"}})"));
  r.Step(Keys([](OverlayInput& in) { in.f[8] = true; }));
  CHECK(!r.ui->list_open());
  r.Step();
  r.Step(Keys([](OverlayInput& in) { in.f[9] = true; }));
  CHECK(r.ui->list_open());
  r.Step();
  r.Step(Keys([](OverlayInput& in) { in.pad_back = in.pad_rb = true; }));
  CHECK(!r.ui->list_open());
}

TEST(popup_ui_close_button_closes_and_latches) {
  ToggleRig r;
  r.ui->SetListOpen(true);
  r.Step();
  CHECK(r.ui->list_open());
  r.ui->SetListOpen(false);
  CHECK(!r.ui->list_open());
  // A host's own close (or the window's X) latches like the toggle.
  OverlayInput held = Keys([](OverlayInput& in) { in.enter = true; });
  CHECK(!r.Step(held).enter);
  r.Step();
  CHECK(r.Step(held).enter);
}
