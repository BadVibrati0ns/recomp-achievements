#include "recomp/achievements/sources/xdbf.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <tuple>
#include <unordered_set>

namespace recomp::achievements::xbox {
namespace {

constexpr uint32_t Fourcc(const char (&s)[5]) {
  return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
         (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}
constexpr uint32_t kMagicXdbf = Fourcc("XDBF");
constexpr uint32_t kMagicXach = Fourcc("XACH");
constexpr uint32_t kMagicXstr = Fourcc("XSTR");
constexpr uint32_t kMagicXstc = Fourcc("XSTC");
constexpr uint32_t kMagicXthd = Fourcc("XTHD");

constexpr size_t kHeaderSize = 24;
constexpr size_t kEntrySize = 18;
constexpr size_t kFreeSize = 8;
constexpr size_t kSectionHeaderSize = 14;  // magic, version, size, u16 count
constexpr size_t kAchievementSize = 0x24;

uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }

void SetError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out, std::string* error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    SetError(error, "cannot open " + path.string());
    return false;
  }
  const std::streamsize size = f.tellg();
  if (size < 0) {
    SetError(error, "cannot read " + path.string());
    return false;
  }
  out.resize(size_t(size));
  f.seekg(0);
  if (size > 0 && !f.read(reinterpret_cast<char*>(out.data()), size)) {
    SetError(error, "cannot read " + path.string());
    return false;
  }
  return true;
}

std::string HexTitleId(uint32_t id) {
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08X", id);
  return buf;
}

}  // namespace

std::string TitleInfo::version_string() const {
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(build) + "." +
         std::to_string(revision);
}

bool XdbfDatabase::Load(std::span<const uint8_t> data, std::string* error) {
  return Load(std::vector<uint8_t>(data.begin(), data.end()), error);
}

bool XdbfDatabase::Load(std::vector<uint8_t>&& data, std::string* error) {
  *this = XdbfDatabase();
  data_ = std::move(data);
  if (data_.size() < kHeaderSize || Be32(data_.data()) != kMagicXdbf) {
    SetError(error, "not an XDBF database (missing XDBF magic)");
    return false;
  }
  const uint8_t* h = data_.data();
  const uint64_t entry_count = Be32(h + 8);
  const uint64_t entry_used = std::min<uint64_t>(Be32(h + 12), entry_count);
  const uint64_t free_count = Be32(h + 16);
  const uint64_t content = kHeaderSize + entry_count * kEntrySize + free_count * kFreeSize;
  if (content > data_.size()) {
    SetError(error, "XDBF header claims more entries than the file holds");
    return false;
  }
  content_offset_ = size_t(content);
  const size_t content_size = data_.size() - content_offset_;
  entries_.reserve(size_t(entry_used));
  for (uint64_t i = 0; i < entry_used; ++i) {
    const uint8_t* e = h + kHeaderSize + i * kEntrySize;
    Entry entry{Be16(e), Be64(e + 2), Be32(e + 10), Be32(e + 14)};
    // Drop entries pointing outside the file rather than rejecting the file.
    if (uint64_t(entry.offset) + entry.size > content_size) continue;
    entries_.push_back(entry);
  }
  valid_ = true;
  return true;
}

bool XdbfDatabase::LoadFile(const std::filesystem::path& path, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!ReadFile(path, bytes, error)) {
    *this = XdbfDatabase();
    return false;
  }
  return Load(std::move(bytes), error);
}

std::span<const uint8_t> XdbfDatabase::GetEntry(Section section, uint64_t id) const {
  for (const Entry& e : entries_) {
    if (e.section == uint16_t(section) && e.id == id) {
      return {data_.data() + content_offset_ + e.offset, e.size};
    }
  }
  return {};
}

Language XdbfDatabase::default_language() const {
  auto block = GetEntry(Section::kMetadata, kMagicXstc);
  if (block.size() < 16 || Be32(block.data()) != kMagicXstc) return Language::kEnglish;
  const uint32_t lang = Be32(block.data() + 12);
  return lang == 0 ? Language::kEnglish : Language(lang);
}

std::vector<Language> XdbfDatabase::languages() const {
  std::vector<Language> out;
  for (const Entry& e : entries_) {
    if (e.section == uint16_t(Section::kStringTable)) out.push_back(Language(uint32_t(e.id)));
  }
  return out;
}

bool XdbfDatabase::HasLanguage(Language language) const {
  return !GetEntry(Section::kStringTable, uint64_t(language)).empty();
}

std::optional<std::string> XdbfDatabase::FindString(Language language, uint16_t string_id) const {
  auto block = GetEntry(Section::kStringTable, uint64_t(language));
  if (block.size() < kSectionHeaderSize || Be32(block.data()) != kMagicXstr) return std::nullopt;
  const uint16_t count = Be16(block.data() + 12);
  size_t pos = kSectionHeaderSize;
  for (uint16_t i = 0; i < count && pos + 4 <= block.size(); ++i) {
    const uint16_t id = Be16(block.data() + pos);
    const uint16_t len = Be16(block.data() + pos + 2);
    pos += 4;
    if (pos + len > block.size()) break;
    if (id == string_id) return std::string(reinterpret_cast<const char*>(block.data() + pos), len);
    pos += len;
  }
  return std::nullopt;
}

std::string XdbfDatabase::GetString(uint16_t string_id, Language preferred) const {
  for (Language lang : {preferred, Language::kEnglish, default_language()}) {
    if (auto s = FindString(lang, string_id)) return *s;
  }
  for (Language lang : languages()) {
    if (auto s = FindString(lang, string_id)) return *s;
  }
  return {};
}

std::vector<XdbfAchievement> XdbfDatabase::achievements() const {
  std::vector<XdbfAchievement> out;
  auto block = GetEntry(Section::kMetadata, kMagicXach);
  if (block.size() < kSectionHeaderSize || Be32(block.data()) != kMagicXach) return out;
  const uint16_t count = Be16(block.data() + 12);
  const size_t fits = (block.size() - kSectionHeaderSize) / kAchievementSize;
  out.reserve(std::min<size_t>(count, fits));
  for (size_t i = 0; i < count && i < fits; ++i) {
    const uint8_t* p = block.data() + kSectionHeaderSize + i * kAchievementSize;
    XdbfAchievement a;
    a.id = Be16(p + 0);
    a.label_id = Be16(p + 2);
    a.description_id = Be16(p + 4);
    a.unachieved_id = Be16(p + 6);
    a.image_id = Be32(p + 8);
    a.gamerscore = Be16(p + 12);
    a.flags = Be32(p + 16);
    out.push_back(a);
  }
  return out;
}

std::optional<TitleInfo> XdbfDatabase::title_info() const {
  auto block = GetEntry(Section::kMetadata, kMagicXthd);
  // magic, version, size, then title id, type and four u16 version parts.
  if (block.size() < 28 || Be32(block.data()) != kMagicXthd) return std::nullopt;
  const uint8_t* p = block.data() + 12;
  TitleInfo info;
  info.title_id = Be32(p);
  info.title_type = Be32(p + 4);
  info.major = Be16(p + 8);
  info.minor = Be16(p + 10);
  info.build = Be16(p + 12);
  info.revision = Be16(p + 14);
  return info;
}

// ---- XdbfTitle ------------------------------------------------------------

std::string XdbfTitle::GetString(uint16_t string_id, Language preferred) const {
  // Per language, TU before base, so a TU only in English doesn't hide the
  // base's translation in the preferred language.
  std::vector<Language> order = {preferred, Language::kEnglish};
  if (update_.valid()) order.push_back(update_.default_language());
  if (base_.valid()) order.push_back(base_.default_language());
  for (Language lang : order) {
    if (update_.valid())
      if (auto s = update_.FindString(lang, string_id)) return *s;
    if (base_.valid())
      if (auto s = base_.FindString(lang, string_id)) return *s;
  }
  if (update_.valid()) {
    if (auto s = update_.GetString(string_id, preferred); !s.empty()) return s;
  }
  return base_.valid() ? base_.GetString(string_id, preferred) : std::string();
}

std::vector<XdbfAchievement> XdbfTitle::achievements() const {
  std::vector<XdbfAchievement> out = base_.valid() ? base_.achievements() : std::vector<XdbfAchievement>{};
  if (!update_.valid()) return out;
  for (const XdbfAchievement& tu : update_.achievements()) {
    auto it = std::find_if(out.begin(), out.end(), [&](const auto& a) { return a.id == tu.id; });
    if (it != out.end()) {
      *it = tu;
    } else {
      out.push_back(tu);
    }
  }
  return out;
}

std::optional<TitleInfo> XdbfTitle::title_info() const {
  if (update_.valid())
    if (auto info = update_.title_info()) return info;
  return base_.valid() ? base_.title_info() : std::nullopt;
}

std::span<const uint8_t> XdbfTitle::image(uint64_t image_id) const {
  if (update_.valid())
    if (auto img = update_.image(image_id); !img.empty()) return img;
  return base_.valid() ? base_.image(image_id) : std::span<const uint8_t>{};
}

AchievementSet XdbfTitle::ToAchievementSet(const XdbfSetOptions& options) const {
  AchievementSet set;
  set.id = options.set_id;
  set.platform = Platform::kXbox360;
  set.category = options.category;
  set.title = options.title.empty() ? title(options.language) : options.title;

  nlohmann::json source = {{"platform", "xbox360"}, {"title_update", update_.valid()}};
  if (auto info = title_info()) {
    source["title_id"] = HexTitleId(info->title_id);
    source["title_version"] = info->version_string();
  }
  set.source = std::move(source);

  uint32_t order = 0;
  for (const XdbfAchievement& x : achievements()) {
    Achievement a;
    a.local_id = std::to_string(x.id);
    a.platform_id = x.id;
    a.set_id = set.id;
    a.name = GetString(x.label_id, options.language);
    a.description = GetString(x.description_id, options.language);
    std::string unachieved = GetString(x.unachieved_id, options.language);
    if (unachieved != a.description) a.locked_description = std::move(unachieved);
    a.points = x.gamerscore;
    a.secret = x.secret();
    a.display_order = order++;
    a.icon = "xdbf:" + std::to_string(x.image_id);
    set.achievements.push_back(std::move(a));
  }
  return set;
}

std::span<const uint8_t> XdbfTitle::ResolveIcon(std::string_view ref) const {
  auto id = ParseIconRef(ref);
  return id ? image(*id) : std::span<const uint8_t>{};
}

std::optional<uint64_t> ParseIconRef(std::string_view ref) {
  constexpr std::string_view kPrefix = "xdbf:";
  if (!ref.starts_with(kPrefix)) return std::nullopt;
  ref.remove_prefix(kPrefix.size());
  int base = 10;
  if (ref.starts_with("0x") || ref.starts_with("0X")) {
    ref.remove_prefix(2);
    base = 16;
  }
  uint64_t value = 0;
  auto [end, ec] = std::from_chars(ref.data(), ref.data() + ref.size(), value, base);
  if (ec != std::errc() || end != ref.data() + ref.size() || ref.empty()) return std::nullopt;
  return value;
}

// ---- Title update search --------------------------------------------------

std::filesystem::path FindTitleUpdateSpa(const std::vector<std::filesystem::path>& roots,
                                         uint32_t title_id) {
  namespace fs = std::filesystem;
  std::vector<fs::path> candidates;
  std::unordered_set<std::string> seen;
  std::error_code ec;
  auto add = [&](const fs::path& p) {
    auto canon = fs::weakly_canonical(p, ec).string();
    if (seen.insert(canon.empty() ? p.string() : canon).second) candidates.push_back(p);
  };

  std::string upper = HexTitleId(title_id), lower = upper;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return char(std::tolower(c)); });

  for (const fs::path& root : roots) {
    if (!fs::is_directory(root, ec)) continue;
    for (const std::string& tid : {upper, lower}) {
      for (const char* type : {"00000002", "000B0000", "000b0000"}) {
        const fs::path dir = root / "0000000000000000" / tid / type;
        if (!fs::is_directory(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
          if (ec) break;
          if (it->is_regular_file(ec) && it->path().filename() == "spa.bin") add(it->path());
        }
      }
    }
    if (fs::is_regular_file(root / "spa.bin", ec)) add(root / "spa.bin");
  }

  struct Scored {
    fs::path path;
    uint64_t version;
    size_t achievements;
    uintmax_t size;
  };
  std::optional<Scored> best;
  for (const fs::path& p : candidates) {
    XdbfDatabase db;
    if (!db.LoadFile(p)) continue;
    auto info = db.title_info();
    if (info && title_id != 0 && info->title_id != title_id) continue;
    Scored s{p, info ? info->version_key() : 0, db.achievements().size(), db.bytes().size()};
    if (!best || std::tie(s.version, s.achievements, s.size) >
                     std::tie(best->version, best->achievements, best->size)) {
      best = std::move(s);
    }
  }
  return best ? best->path : fs::path();
}

}  // namespace recomp::achievements::xbox
