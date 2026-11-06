// recomp-achievements: every recomp's achievements in one window, read from
// the shared library (docs/app.md). Windowed with SDL3, or headless: scripted
// input rendered to raw RGB24 frames for a video.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "recomp/achievements/ui/sdl_support.h"
#include "script.h"

using namespace recomp::achievements;
using namespace recomp::achievements::app;

namespace {

struct Args {
  std::vector<std::filesystem::path> roots;
  int width = 1280, height = 800;
  int out_width = 0, out_height = 0;
  float scale = 1.0f;
  bool fullscreen = false;
  bool headless = false;
  bool global_preferences = true;
  std::string script, frames_out;
  float fps = 30.0f;
  float quit_after = 0.0f;  // Windowed: close after this many seconds (smoke tests).
};

int Usage() {
  std::fprintf(stderr,
               "usage: recomp-achievements [options]\n"
               "  --library DIR        show this library folder (repeatable); default: the\n"
               "                       player's library and any in Wine/Proton prefixes\n"
               "  --size WxH           window size (default 1280x800)\n"
               "  --scale X            text and layout scale on top of the window's\n"
               "  --fullscreen         start fullscreen (F11 toggles)\n"
               "  --no-preferences     ignore the player's achievements_style.json\n"
               "  --headless --script FILE --frames-out FILE|-\n"
               "                       run FILE's scripted input without a window and write\n"
               "                       raw RGB24 frames (see apps/achievements/script.h)\n"
               "  --out-size WxH       headless frame size (default: --size); the window\n"
               "                       is centred in it when a script resizes it\n"
               "  --fps N              headless frame rate (default 30)\n"
               "  --quit-after SECONDS close the window after a while (tests)\n");
  return 2;
}

bool ParseSize(const char* s, int& w, int& h) { return std::sscanf(s, "%dx%d", &w, &h) == 2 && w > 0 && h > 0; }

bool ParseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (k == "--library" && (v = value())) a.roots.emplace_back(v);
    else if (k == "--size" && (v = value())) { if (!ParseSize(v, a.width, a.height)) return false; }
    else if (k == "--out-size" && (v = value())) { if (!ParseSize(v, a.out_width, a.out_height)) return false; }
    else if (k == "--scale" && (v = value())) a.scale = static_cast<float>(std::atof(v));
    else if (k == "--fps" && (v = value())) a.fps = static_cast<float>(std::atof(v));
    else if (k == "--script" && (v = value())) a.script = v;
    else if (k == "--frames-out" && (v = value())) a.frames_out = v;
    else if (k == "--quit-after" && (v = value())) a.quit_after = static_cast<float>(std::atof(v));
    else if (k == "--fullscreen") a.fullscreen = true;
    else if (k == "--headless") a.headless = true;
    else if (k == "--no-preferences") a.global_preferences = false;
    else return false;
  }
  return a.scale > 0 && a.fps > 0;
}

AppOptions MakeOptions(const Args& a) {
  AppOptions o;
  o.roots = a.roots;
  o.scale = a.scale;
  o.global_preferences = a.global_preferences;
  return o;
}

// ---- Headless ------------------------------------------------------------------

int RunHeadless(const Args& args) {
  if (args.script.empty() || args.frames_out.empty()) {
    std::fprintf(stderr, "--headless needs --script and --frames-out\n");
    return 2;
  }
  std::ifstream file(args.script, std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "can't read %s\n", args.script.c_str());
    return 1;
  }
  std::stringstream text;
  text << file.rdbuf();
  std::vector<ScriptCommand> script;
  std::string error;
  if (!ParseScript(text.str(), script, &error)) {
    std::fprintf(stderr, "%s: %s\n", args.script.c_str(), error.c_str());
    return 1;
  }
  FILE* out = args.frames_out == "-" ? stdout : std::fopen(args.frames_out.c_str(), "wb");
  if (!out) {
    std::fprintf(stderr, "can't write %s\n", args.frames_out.c_str());
    return 1;
  }
  const int ow = args.out_width ? args.out_width : args.width;
  const int oh = args.out_height ? args.out_height : args.height;
  std::vector<uint8_t> frame(static_cast<size_t>(ow) * oh * 3);

  App app(MakeOptions(args));
  app.SetSize(args.width, args.height);
  uint64_t redraws = 0;
  ScriptTarget target;
  target.frame = [&](float dt, const ui::OverlayInput& in) {
    // Mouse positions are in the window, which sits centred in the frame.
    redraws += app.Frame(dt, in);
    const ui::Image& img = app.image();
    const int x0 = (ow - img.width) / 2, y0 = (oh - img.height) / 2;
    for (int y = 0; y < oh; ++y) {
      uint8_t* d = &frame[static_cast<size_t>(y) * ow * 3];
      for (int x = 0; x < ow; ++x, d += 3) {
        const int sx = x - x0, sy = y - y0;
        if (sx < 0 || sy < 0 || sx >= img.width || sy >= img.height) {
          d[0] = 10, d[1] = 10, d[2] = 12;  // Around a smaller window.
          continue;
        }
        const uint8_t* p = &img.rgba[(static_cast<size_t>(sy) * img.width + sx) * 4];
        d[0] = p[0], d[1] = p[1], d[2] = p[2];
      }
    }
    std::fwrite(frame.data(), 1, frame.size(), out);
  };
  target.resize = [&](int w, int h) { app.SetSize(std::min(w, ow), std::min(h, oh)); };
  target.scale = [&](float s) { app.SetScale(s); };
  target.labels = [&](ui::ButtonLabels l) { app.SetButtonLabels(l); };
  target.poll = [&] { app.Poll(); };
  const int frames = RunScript(script, args.fps, target);
  if (out != stdout) std::fclose(out);
  else std::fflush(stdout);
  std::fprintf(stderr, "%d frames at %dx%d, %llu redraws\n", frames, ow, oh, static_cast<unsigned long long>(redraws));
  return 0;
}

// ---- Windowed --------------------------------------------------------------------

int RunWindowed(const Args& args) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (args.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
  SDL_Window* window = SDL_CreateWindow("Achievements", args.width, args.height, flags);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (!renderer) {
    std::fprintf(stderr, "SDL window: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);
  std::fprintf(stderr, "video driver %s, renderer %s\n", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(renderer));

  App app(MakeOptions(args));
  SDL_Texture* texture = nullptr;
  int tw = 0, th = 0;
  SDL_Gamepad* pad = nullptr;
  bool running = true, redraw = true, animating = false;
  float wheel = 0;
  uint64_t last = SDL_GetTicksNS();
  const uint64_t start = last;
  uint64_t frames = 0, uploads = 0;

  while (running) {
    auto handle = [&](const SDL_Event& e) {
      switch (e.type) {
        case SDL_EVENT_QUIT: running = false; break;
        case SDL_EVENT_MOUSE_WHEEL: wheel += e.wheel.y; break;
        case SDL_EVENT_GAMEPAD_ADDED:
          if (!pad) pad = SDL_OpenGamepad(e.gdevice.which);
          app.SetButtonLabels(ui::ButtonLabelsFor(pad));
          break;
        case SDL_EVENT_GAMEPAD_REMOVED:
          if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) {
            SDL_CloseGamepad(pad);
            pad = nullptr;
          }
          break;
        case SDL_EVENT_WINDOW_EXPOSED: redraw = true; break;
        default: break;
      }
    };
    SDL_Event e;
    // Nothing moving: sleep until something happens, or for a tenth of a
    // second (the library is polled about once a second). While the last frame
    // changed something (an animation, scrolling), keep going at vsync.
    if (!redraw && !animating && SDL_WaitEventTimeout(&e, 100)) handle(e);
    while (SDL_PollEvent(&e)) handle(e);
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    if (pw != tw || ph != th) {
      if (texture) SDL_DestroyTexture(texture);
      texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, pw, ph);
      tw = pw, th = ph;
      app.SetSize(pw, ph);
      redraw = true;
    }

    ui::OverlayInput in;
    ui::FillOverlayInput(in, SDL_GetKeyboardState(nullptr), pad);
    float mx = 0, my = 0;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
    const float density = SDL_GetWindowPixelDensity(window);
    in.mouse_present = SDL_GetMouseFocus() == window;
    in.mouse_x = mx * density;
    in.mouse_y = my * density;
    in.mouse_left = (buttons & SDL_BUTTON_LMASK) != 0;
    in.mouse_wheel = wheel;
    wheel = 0;

    const uint64_t now = SDL_GetTicksNS();
    const float dt = std::min(0.1f, static_cast<float>(now - last) / 1e9f);
    last = now;
    ++frames;
    if (args.quit_after > 0 && (now - start) / 1e9 >= args.quit_after) running = false;
    animating = app.Frame(dt, in);
    if (animating) {
      const ui::PixelRect r = app.dirty_rect();
      const SDL_Rect rect{r.x0, r.y0, r.width(), r.height()};
      const ui::Image& img = app.image();
      SDL_UpdateTexture(texture, &rect, img.rgba.data() + (static_cast<size_t>(r.y0) * img.width + r.x0) * 4,
                        img.width * 4);
      ++uploads;
      redraw = true;
    } else if (!redraw) {
      continue;
    }
    if (app.TakeFullscreenToggle())
      SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
    redraw = false;
  }
  std::fprintf(stderr, "%llu frames, %llu uploads\n", static_cast<unsigned long long>(frames),
               static_cast<unsigned long long>(uploads));
  if (pad) SDL_CloseGamepad(pad);
  if (texture) SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, args)) return Usage();
  return args.headless ? RunHeadless(args) : RunWindowed(args);
}
