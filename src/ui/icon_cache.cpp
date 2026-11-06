#include "recomp/achievements/ui/icon_cache.h"

#include <algorithm>
#include <chrono>
#include <fstream>

namespace recomp::achievements::ui {

namespace {

double Now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool SchemeChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-';
}

}  // namespace

void SplitIconRef(std::string_view ref, std::string_view& scheme, std::string_view& body) {
  scheme = {};
  body = ref;
  const size_t colon = ref.find(':');
  // One letter before ':' is a Windows drive, not a scheme.
  if (colon == std::string_view::npos || colon < 2) return;
  for (size_t i = 0; i < colon; ++i)
    if (!SchemeChar(ref[i])) return;
  scheme = ref.substr(0, colon);
  body = ref.substr(colon + 1);
  if (scheme == "file") scheme = {};
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return false;
  const std::streamoff size = in.tellg();
  if (size < 0) return false;
  out.resize(static_cast<size_t>(size));
  in.seekg(0);
  return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size)) || size == 0;
}

IconCache::IconCache() = default;

IconCache::~IconCache() { Clear(); }

void IconCache::SetTextureCallbacks(CreateTexture create, DestroyTexture destroy) {
  std::lock_guard lock(mutex_);
  create_ = std::move(create);
  destroy_ = std::move(destroy);
}

void IconCache::SetDecoder(Decoder decoder) {
  std::lock_guard lock(mutex_);
  decoder_ = std::move(decoder);
  // Earlier failures may have been "no decoder".
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (!it->second.texture) it = entries_.erase(it);
    else ++it;
  }
}

bool IconCache::has_decoder() const {
  std::lock_guard lock(mutex_);
  return static_cast<bool>(decoder_);
}

void IconCache::AddProvider(std::string scheme, Provider provider) {
  std::lock_guard lock(mutex_);
  providers_[std::move(scheme)] = std::move(provider);
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (!it->second.texture) it = entries_.erase(it);
    else ++it;
  }
}

void IconCache::RemoveProvider(std::string_view scheme) {
  std::lock_guard lock(mutex_);
  if (auto it = providers_.find(scheme); it != providers_.end()) providers_.erase(it);
}

void IconCache::SetRetrySeconds(double seconds) {
  std::lock_guard lock(mutex_);
  retry_seconds_ = seconds;
}

IconResult IconCache::Fetch(std::string_view ref, std::string_view set_id,
                            const std::filesystem::path& base_dir, IconData& out) {
  if (ref.empty()) return IconResult::kMissing;
  std::string_view scheme, body;
  SplitIconRef(ref, scheme, body);

  Provider provider;
  {
    std::lock_guard lock(mutex_);
    if (!scheme.empty()) {
      auto it = providers_.find(scheme);
      if (it == providers_.end()) return IconResult::kMissing;
      provider = it->second;
    }
  }

  if (provider) {
    IconRequest req{ref, scheme, body, set_id, &base_dir};
    return provider(req, out);
  }
  std::filesystem::path path{std::string(body)};
  if (path.is_relative() && !base_dir.empty()) path = base_dir / path;
  return ReadFileBytes(path, out.bytes) ? IconResult::kOk : IconResult::kMissing;
}

bool IconCache::Decode(std::span<const uint8_t> bytes, Image& out) const {
  Decoder decoder;
  {
    std::lock_guard lock(mutex_);
    decoder = decoder_;
  }
  Image img;
  if (bytes.empty() || !decoder || !decoder(bytes, img) || img.width <= 0 || img.height <= 0) return false;
  out = std::move(img);
  return true;
}

IconResult IconCache::Load(std::string_view ref, std::string_view set_id,
                           const std::filesystem::path& base_dir, Image& out) {
  IconData data;
  const IconResult r = Fetch(ref, set_id, base_dir, data);
  if (r != IconResult::kOk) return r;
  if (data.image.width > 0 && data.image.height > 0 &&
      data.image.rgba.size() >= static_cast<size_t>(data.image.width) * data.image.height * 4) {
    out = std::move(data.image);
    return IconResult::kOk;
  }
  return Decode(data.bytes, out) ? IconResult::kOk : IconResult::kMissing;
}

void* IconCache::Get(std::string_view ref, std::string_view set_id, const std::filesystem::path& base_dir) {
  if (ref.empty()) return nullptr;
  std::string key(set_id);
  key += '|';
  key += ref;
  CreateTexture create;
  {
    std::lock_guard lock(mutex_);
    if (!create_) return nullptr;  // Not cached: callbacks may arrive later.
    if (auto it = entries_.find(key); it != entries_.end()) {
      const Entry& e = it->second;
      if (e.texture || e.failed) return e.texture;
      if (Now() < e.retry_at) return nullptr;
    }
    create = create_;
  }

  // Unlocked: providers and texture uploads can be slow or re-enter the cache.
  Image img;
  const IconResult r = Load(ref, set_id, base_dir, img);
  Entry e;
  if (r == IconResult::kOk) {
    e.texture = create(img.width, img.height, img.rgba.data());
    e.width = img.width;
    e.height = img.height;
    e.failed = e.texture == nullptr;
  } else if (r == IconResult::kPending) {
    std::lock_guard lock(mutex_);
    e.retry_at = Now() + retry_seconds_;
  } else {
    e.failed = true;
  }

  std::lock_guard lock(mutex_);
  auto [it, inserted] = entries_.try_emplace(key, e);
  if (!inserted) {
    if (it->second.texture && e.texture && destroy_) destroy_(e.texture);  // Lost a race.
    else it->second = e;
  }
  return it->second.texture;
}

void IconCache::SetFallbackIcon(std::string ref, std::filesystem::path base_dir) {
  std::lock_guard lock(mutex_);
  fallback_ref_ = std::move(ref);
  fallback_base_dir_ = std::move(base_dir);
}

void* IconCache::GetOrFallback(std::string_view ref, std::string_view set_id,
                               const std::filesystem::path& base_dir) {
  if (!ref.empty()) {
    if (void* texture = Get(ref, set_id, base_dir)) return texture;
  }
  std::string fallback;
  std::filesystem::path fallback_base;
  {
    std::lock_guard lock(mutex_);
    fallback = fallback_ref_;
    fallback_base = fallback_base_dir_;
  }
  return fallback.empty() ? nullptr : Get(fallback, {}, fallback_base);
}

void IconCache::Clear() {
  std::map<std::string, Entry, std::less<>> entries;
  DestroyTexture destroy;
  {
    std::lock_guard lock(mutex_);
    entries.swap(entries_);
    destroy = destroy_;
  }
  if (!destroy) return;
  for (auto& [key, e] : entries)
    if (e.texture) destroy(e.texture);
}

size_t IconCache::size() const {
  std::lock_guard lock(mutex_);
  return entries_.size();
}

LibraryIconSource LibraryIconSourceFor(IconCache& icons) {
  return [&icons](std::string_view ref, std::string_view set_id, const std::filesystem::path& base_dir,
                  LibraryIcon& out) {
    IconData data;
    if (icons.Fetch(ref, set_id, base_dir, data) != IconResult::kOk) return false;
    const Image& img = data.image;
    if (img.width > 0 && img.height > 0 && img.rgba.size() >= static_cast<size_t>(img.width) * img.height * 4) {
      out.width = img.width;
      out.height = img.height;
      out.rgba = img.rgba;
      return true;
    }
    static constexpr uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (data.bytes.size() >= 8 && std::equal(kPng, kPng + 8, data.bytes.begin())) {
      out.png = std::move(data.bytes);
      return true;
    }
    Image decoded;
    if (!icons.Decode(data.bytes, decoded)) return false;
    out.width = decoded.width;
    out.height = decoded.height;
    out.rgba = std::move(decoded.rgba);
    return true;
  };
}

std::unique_ptr<Library> MakeLibrary(System& system, LibraryOptions options, IconCache& icons,
                                     const std::string& fallback_icon) {
  if (!options.enabled || options.game_id.empty()) return nullptr;
  if (!options.icon_source) options.icon_source = LibraryIconSourceFor(icons);
  if (options.icon.empty()) options.icon = fallback_icon;
  return std::make_unique<Library>(system, std::move(options));
}

}  // namespace recomp::achievements::ui
