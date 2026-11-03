// Renders every built-in theme's unlock and progress popups to a raw RGB24
// video stream on stdout, plus a WAV of the matching sounds, without a GPU:
// ImGui draw data is rasterised in software.
//
//   popup_demo <audio.wav> | ffmpeg -f rawvideo -pix_fmt rgb24 -s 960x540 -r 30 -i - \
//       -i audio.wav -c:v libx264 -pix_fmt yuv420p -c:a aac demo.mp4

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "recomp/achievements/ui/icon_cache.h"
#include "recomp/achievements/ui/imgui_renderer.h"
#include "recomp/achievements/ui/notification_center.h"
#include "recomp/achievements/ui/sounds.h"
#include "recomp/achievements/ui/style.h"

using namespace recomp::achievements;
using namespace recomp::achievements::ui;

namespace {

constexpr int kWidth = 960, kHeight = 540, kFps = 30;
constexpr float kSecondsPerTheme = 6.0f;

struct Texture {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
};
std::map<ImU64, Texture> g_textures;

void UploadFontTextures(ImDrawData* dd) {
  if (!dd->Textures) return;
  for (ImTextureData* t : *dd->Textures) {
    if (t->Status != ImTextureStatus_WantCreate && t->Status != ImTextureStatus_WantUpdates) continue;
    const ImU64 id = 1000 + t->UniqueID;
    Texture& x = g_textures[id];
    x.w = t->Width;
    x.h = t->Height;
    const auto* px = static_cast<const uint8_t*>(t->GetPixels());
    x.rgba.clear();
    if (t->Format == ImTextureFormat_Alpha8) {
      for (int i = 0; i < t->Width * t->Height; ++i) x.rgba.insert(x.rgba.end(), {255, 255, 255, px[i]});
    } else {
      x.rgba.assign(px, px + t->Width * t->Height * 4);
    }
    t->SetTexID(static_cast<ImTextureID>(id));
    t->SetStatus(ImTextureStatus_OK);
  }
}

void Rasterize(ImDrawData* dd, std::vector<float>& fb) {
  UploadFontTextures(dd);
  for (ImDrawList* dl : dd->CmdLists) {
    for (const ImDrawCmd& cmd : dl->CmdBuffer) {
      auto it = g_textures.find(static_cast<ImU64>(cmd.GetTexID()));
      const Texture* tex = it == g_textures.end() ? nullptr : &it->second;
      for (unsigned i = 0; i < cmd.ElemCount; i += 3) {
        const ImDrawVert* v[3];
        for (int k = 0; k < 3; ++k) v[k] = &dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + k]];
        const float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) -
                           (v[2]->pos.x - v[0]->pos.x) * (v[1]->pos.y - v[0]->pos.y);
        if (std::fabs(area) < 1e-8f) continue;
        const float minx = std::max(cmd.ClipRect.x, std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}));
        const float maxx = std::min(cmd.ClipRect.z, std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}));
        const float miny = std::max(cmd.ClipRect.y, std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}));
        const float maxy = std::min(cmd.ClipRect.w, std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}));
        for (int y = std::max(0, static_cast<int>(miny)); y <= std::min(kHeight - 1, static_cast<int>(maxy)); ++y) {
          for (int x = std::max(0, static_cast<int>(minx)); x <= std::min(kWidth - 1, static_cast<int>(maxx)); ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float w0 = ((v[1]->pos.x - px) * (v[2]->pos.y - py) - (v[2]->pos.x - px) * (v[1]->pos.y - py)) / area;
            const float w1 = ((v[2]->pos.x - px) * (v[0]->pos.y - py) - (v[0]->pos.x - px) * (v[2]->pos.y - py)) / area;
            const float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float c[4];
            for (int k = 0; k < 4; ++k) {
              auto ch = [&](const ImDrawVert* vv) { return ((vv->col >> (8 * k)) & 0xFF) / 255.0f; };
              c[k] = w0 * ch(v[0]) + w1 * ch(v[1]) + w2 * ch(v[2]);
            }
            if (tex) {
              const float u = w0 * v[0]->uv.x + w1 * v[1]->uv.x + w2 * v[2]->uv.x;
              const float t = w0 * v[0]->uv.y + w1 * v[1]->uv.y + w2 * v[2]->uv.y;
              const int tx = std::clamp(static_cast<int>(u * tex->w), 0, tex->w - 1);
              const int ty = std::clamp(static_cast<int>(t * tex->h), 0, tex->h - 1);
              const uint8_t* s = &tex->rgba[(ty * tex->w + tx) * 4];
              for (int k = 0; k < 4; ++k) c[k] *= s[k] / 255.0f;
            }
            float* d = &fb[(y * kWidth + x) * 3];
            for (int k = 0; k < 3; ++k) d[k] = d[k] * (1 - c[3]) + c[k] * c[3];
          }
        }
      }
    }
  }
}

// Mixes a synthesized sound into the soundtrack at `seconds`.
void MixSound(std::vector<float>& track, const std::string& ref, float seconds, float volume) {
  if (ref.rfind("builtin:", 0) != 0) return;
  const Pcm pcm = SynthesizeBuiltin(ref.substr(8));
  const size_t start = static_cast<size_t>(seconds * kSoundSampleRate) * kSoundChannels;
  if (track.size() < start + pcm.samples.size()) track.resize(start + pcm.samples.size(), 0.0f);
  for (size_t i = 0; i < pcm.samples.size(); ++i) track[start + i] += pcm.samples[i] * volume;
}

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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: popup_demo <audio.wav> > video.rgb\n");
    return 2;
  }
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(kWidth, kHeight);
  io.IniFilename = nullptr;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  const char* regular_ttf = "/usr/share/fonts/dejavu/DejaVuSans.ttf";
  const char* bold_ttf = "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf";
  ImFont* regular = std::ifstream(regular_ttf) ? io.Fonts->AddFontFromFileTTF(regular_ttf, 18) : io.Fonts->AddFontDefault();
  ImFont* bold = std::ifstream(bold_ttf) ? io.Fonts->AddFontFromFileTTF(bold_ttf, 18) : regular;

  // A two-tone placeholder icon.
  Texture icon{16, 16, {}};
  for (int i = 0; i < 256; ++i) {
    const bool a = ((i % 16) / 4 + (i / 64)) % 2;
    icon.rgba.insert(icon.rgba.end(), {uint8_t(a ? 70 : 230), 140, uint8_t(a ? 230 : 90), 255});
  }
  g_textures[7] = icon;
  IconCache icons;
  icons.SetTextureCallbacks([](int, int, const uint8_t*) { return reinterpret_cast<void*>(uintptr_t{7}); });
  icons.AddProvider("demo", [](const IconRequest&, IconData& out) {
    out.image.width = out.image.height = 1;
    out.image.rgba = {1, 1, 1, 1};
    return IconResult::kOk;
  });

  std::vector<float> soundtrack;
  std::vector<uint8_t> frame(kWidth * kHeight * 3);
  float clock = 0.0f;
  for (const std::string& theme : BuiltinThemeNames()) {
    NotificationCenter center;
    ImGuiRenderer renderer(center, &icons);
    renderer.SetFonts(regular, bold);
    auto style = std::make_shared<Style>(BuiltinTheme(theme));

    Notification unlock;
    unlock.kind = NotificationKind::kUnlock;
    unlock.key = "demo:flip_book";
    unlock.name = "Flip Trick Encyclopedia";
    unlock.description = "Land every flip trick at least once.";
    unlock.points = 50;
    unlock.tier = theme == "ps3" ? Tier::kGold : Tier::kNone;
    unlock.icon = "demo:1";
    unlock.set_title = "Theme: " + theme;
    unlock.style = style;
    Notification progress = unlock;
    progress.kind = NotificationKind::kProgress;
    progress.key = "demo:grind_book";
    progress.name = "Grind Encyclopedia";
    progress.item = "Crooked Grind";
    progress.current = 3;
    progress.total = 14;

    const int frames = static_cast<int>(kSecondsPerTheme * kFps);
    for (int f = 0; f < frames; ++f) {
      const float t = static_cast<float>(f) / kFps;
      if (f == kFps / 3) {
        center.Push(progress);
        MixSound(soundtrack, style->sound.progress, clock + t, style->sound.volume);
      }
      if (f == kFps) {
        center.Push(unlock);
        MixSound(soundtrack, style->UnlockSound(unlock.tier), clock + t, style->sound.volume);
      }
      center.Update(1.0f / kFps);
      io.DeltaTime = 1.0f / kFps;
      ImGui::NewFrame();
      ImDrawList* fg = ImGui::GetForegroundDrawList();
      fg->AddText(bold, 22, ImVec2(24, 20), IM_COL32(255, 255, 255, 230), ("theme: " + theme).c_str());
      renderer.Draw(true);
      ImGui::Render();

      std::vector<float> fb(kWidth * kHeight * 3);
      for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
          float* d = &fb[(y * kWidth + x) * 3];
          d[0] = 0.18f + 0.15f * x / kWidth;
          d[1] = 0.26f;
          d[2] = 0.24f + 0.25f * y / kHeight;
        }
      }
      Rasterize(ImGui::GetDrawData(), fb);
      for (size_t i = 0; i < fb.size(); ++i) frame[i] = static_cast<uint8_t>(std::clamp(fb[i] * 255.0f, 0.0f, 255.0f));
      std::fwrite(frame.data(), 1, frame.size(), stdout);
    }
    clock += kSecondsPerTheme;
  }
  soundtrack.resize(static_cast<size_t>(clock * kSoundSampleRate) * kSoundChannels, 0.0f);
  WriteWav(argv[1], soundtrack);
  ImGui::DestroyContext();
  return 0;
}
