#include "library_util.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>

#include "recomp/achievements/library.h"

namespace recomp::achievements {

namespace fs = std::filesystem;
using nlohmann::json;

namespace detail {

namespace {

constexpr std::array<uint32_t, 64> kSha256K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void Sha256Block(std::array<uint32_t, 8>& h, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = k + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
    const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

const std::array<uint32_t, 256>& CrcTable() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  return table;
}

}  // namespace

std::string Sha256Hex(std::span<const uint8_t> data) {
  std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t i = 0;
  for (; i + 64 <= data.size(); i += 64) Sha256Block(h, data.data() + i);
  uint8_t tail[128] = {};
  const size_t rest = data.size() - i;
  for (size_t j = 0; j < rest; ++j) tail[j] = data[i + j];
  tail[rest] = 0x80;
  const size_t tail_len = rest + 9 <= 64 ? 64 : 128;
  const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
  for (int j = 0; j < 8; ++j) tail[tail_len - 1 - j] = static_cast<uint8_t>(bits >> (8 * j));
  for (size_t j = 0; j < tail_len; j += 64) Sha256Block(h, tail + j);
  std::string out;
  char buf[9];
  for (const uint32_t v : h) {
    std::snprintf(buf, sizeof(buf), "%08x", v);
    out += buf;
  }
  return out;
}

std::string Sha256Hex(std::string_view text) {
  return Sha256Hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

uint32_t Crc32(std::span<const uint8_t> data, uint32_t crc) {
  const auto& table = CrcTable();
  crc = ~crc;
  for (const uint8_t b : data) crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
  return ~crc;
}

std::string NewUuid() {
  std::random_device rd;
  std::mt19937_64 rng((static_cast<uint64_t>(rd()) << 32) ^ rd() ^
                      static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
  uint8_t b[16];
  for (int i = 0; i < 16; i += 8) {
    const uint64_t v = rng();
    for (int j = 0; j < 8; ++j) b[i + j] = static_cast<uint8_t>(v >> (8 * j));
  }
  b[6] = static_cast<uint8_t>((b[6] & 0x0f) | 0x40);  // Version 4.
  b[8] = static_cast<uint8_t>((b[8] & 0x3f) | 0x80);  // RFC 4122 variant.
  std::string out;
  char buf[3];
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    std::snprintf(buf, sizeof(buf), "%02x", b[i]);
    out += buf;
  }
  return out;
}

uint64_t UnixMillis() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::string SafeName(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '_' || c == '-';
    out.push_back(ok ? c : '_');
  }
  if (out.empty() || out == "." || out == "..") out = "_" + out;
  return out;
}

bool IsPng(std::span<const uint8_t> bytes) {
  static constexpr uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  if (bytes.size() < 8) return false;
  for (int i = 0; i < 8; ++i)
    if (bytes[i] != kSig[i]) return false;
  return true;
}

bool WriteFileAtomic(const fs::path& path, std::span<const uint8_t> bytes) {
  try {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    // Unique per writer: two processes may write the same file at once.
    fs::path tmp = path;
    tmp += ".tmp-" + NewUuid().substr(0, 8);
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) return false;
      out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      out.flush();
      if (!out.good()) {
        out.close();
        fs::remove(tmp, ec);
        return false;
      }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
      fs::remove(tmp, ec);
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool WriteJsonAtomic(const fs::path& path, const json& doc) {
  const std::string text = doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
  return WriteFileAtomic(path, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

bool WriteJsonAtomic(const fs::path& path, const nlohmann::ordered_json& doc) {
  const std::string text = doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
  return WriteFileAtomic(path, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

json ReadJsonFile(const fs::path& path) {
  try {
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    json doc = json::parse(in, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    return doc.is_object() ? doc : json(nullptr);
  } catch (...) {
    return nullptr;
  }
}

}  // namespace detail

std::vector<uint8_t> EncodePng(int width, int height, std::span<const uint8_t> rgba) {
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
      rgba.size() < static_cast<size_t>(width) * height * 4) {
    return {};
  }
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  const auto put32 = [](std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
  };
  const auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, detail::Crc32(std::span<const uint8_t>(out.data() + start, out.size() - start)));
  };

  std::vector<uint8_t> ihdr;
  put32(ihdr, static_cast<uint32_t>(width));
  put32(ihdr, static_cast<uint32_t>(height));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, no interlace.
  chunk("IHDR", ihdr);

  // Scanlines with filter 0, in stored (uncompressed) deflate blocks: icons
  // are small, and this needs no zlib.
  const size_t row = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> raw;
  raw.reserve((row + 1) * height);
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + y * row, rgba.begin() + (y + 1) * row);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    z.push_back(pos + n == raw.size() ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n));
    z.push_back(static_cast<uint8_t>(n >> 8));
    z.push_back(static_cast<uint8_t>(~n));
    z.push_back(static_cast<uint8_t>(~n >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  } while (pos < raw.size());
  uint32_t a = 1, b = 0;
  for (const uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put32(z, (b << 16) | a);
  chunk("IDAT", z);
  chunk("IEND", {});
  return out;
}

}  // namespace recomp::achievements
