// Image and WAV decoding with no SDL3, for games that draw popups and play
// sounds themselves. SDL3 builds may still install SDL's loaders over these.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "recomp/achievements/ui/icon_cache.h"
#include "recomp/achievements/ui/sounds.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include <stb/stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace recomp::achievements::ui {

bool DecodeImage(std::span<const uint8_t> bytes, Image& out) {
  if (bytes.empty() || bytes.size() > static_cast<size_t>(INT32_MAX)) return false;
  int w = 0, h = 0, channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h,
                                          &channels, 4);
  if (!pixels) return false;
  out.width = w;
  out.height = h;
  out.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
  stbi_image_free(pixels);
  return w > 0 && h > 0;
}

namespace {

uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

// One sample in -1..1 from the formats WAV files use in practice.
float Sample(const uint8_t* p, uint16_t format, uint16_t bits) {
  if (format == 3 && bits == 32) {
    float f;
    std::memcpy(&f, p, 4);
    return std::isfinite(f) ? f : 0.0f;
  }
  switch (bits) {
    case 8: return (int(p[0]) - 128) / 128.0f;
    case 16: return int16_t(U16(p)) / 32768.0f;
    case 24: return int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) / 2147483648.0f;
    case 32: return int32_t(U32(p)) / 2147483648.0f;
    default: return 0.0f;
  }
}

}  // namespace

bool DecodeWav(std::span<const uint8_t> bytes, Pcm& out) {
  const uint8_t* data = bytes.data();
  const size_t size = bytes.size();
  if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) return false;

  uint16_t format = 0, channels = 0, bits = 0, block_align = 0;
  uint32_t rate = 0;
  const uint8_t* samples = nullptr;
  size_t sample_bytes = 0;
  for (size_t pos = 12; pos + 8 <= size;) {
    const uint32_t chunk = U32(data + pos + 4);
    const uint8_t* body = data + pos + 8;
    const size_t available = std::min<size_t>(chunk, size - pos - 8);
    if (std::memcmp(data + pos, "fmt ", 4) == 0 && available >= 16) {
      format = U16(body);
      channels = U16(body + 2);
      rate = U32(body + 4);
      block_align = U16(body + 12);
      bits = U16(body + 14);
      // WAVE_FORMAT_EXTENSIBLE: the real format is the sub-format GUID's first word.
      if (format == 0xFFFE && available >= 26) format = U16(body + 24);
    } else if (std::memcmp(data + pos, "data", 4) == 0) {
      samples = body;
      sample_bytes = available;
    }
    pos += 8 + size_t(chunk) + (chunk & 1);
  }
  const bool supported = (format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
                         (format == 3 && bits == 32);
  if (!supported || !samples || channels == 0 || rate == 0 || block_align < channels * (bits / 8)) return false;

  const size_t frames = sample_bytes / block_align;
  if (frames == 0) return false;
  const size_t bytes_per_sample = bits / 8;
  auto frame_sample = [&](size_t frame, int channel) {
    const int c = std::min<int>(channel, channels - 1);  // Mono plays on both sides.
    return Sample(samples + frame * block_align + c * bytes_per_sample, format, bits);
  };

  // Linear resampling to the bank's rate: plenty for short popup sounds.
  const double step = double(rate) / kSoundSampleRate;
  const size_t out_frames = std::max<size_t>(1, size_t(std::floor((frames - 1) / step)) + 1);
  out.samples.resize(out_frames * kSoundChannels);
  for (size_t i = 0; i < out_frames; ++i) {
    const double src = i * step;
    const size_t a = std::min(size_t(src), frames - 1);
    const size_t b = std::min(a + 1, frames - 1);
    const float t = float(src - a);
    for (int c = 0; c < kSoundChannels; ++c) {
      out.samples[i * kSoundChannels + c] = frame_sample(a, c) * (1.0f - t) + frame_sample(b, c) * t;
    }
  }
  return true;
}

bool LoadWavFile(const std::filesystem::path& path, Pcm& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return DecodeWav(bytes, out);
}

}  // namespace recomp::achievements::ui
