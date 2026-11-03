#include "recomp/achievements/ui/icon_cache.h"

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

IconResult IconCache::Load(std::string_view ref, std::string_view set_id,
                           const std::filesystem::path& base_dir, Image& out) {
  if (ref.empty()) return IconResult::kMissing;
  std::string_view scheme, body;
  SplitIconRef(ref, scheme, body);

  Provider provider;
  Decoder decoder;
  {
    std::lock_guard lock(mutex_);
    if (!scheme.empty()) {
      auto it = providers_.find(scheme);
      if (it == providers_.end()) return IconResult::kMissing;
      provider = it->second;
    }
    decoder = decoder_;
  }

  IconData data;
  if (provider) {
    IconRequest req{ref, scheme, body, set_id, &base_dir};
    const IconResult r = provider(req, data);
    if (r != IconResult::kOk) return r;
  } else {
    std::filesystem::path path{std::string(body)};
    if (path.is_relative() && !base_dir.empty()) path = base_dir / path;
    if (!ReadFileBytes(path, data.bytes)) return IconResult::kMissing;
  }

  if (data.image.width > 0 && data.image.height > 0 &&
      data.image.rgba.size() >= static_cast<size_t>(data.image.width) * data.image.height * 4) {
    out = std::move(data.image);
    return IconResult::kOk;
  }
  if (data.bytes.empty() || !decoder) return IconResult::kMissing;
  Image img;
  if (!decoder(data.bytes, img) || img.width <= 0 || img.height <= 0) return IconResult::kMissing;
  out = std::move(img);
  return IconResult::kOk;
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

}  // namespace recomp::achievements::ui
