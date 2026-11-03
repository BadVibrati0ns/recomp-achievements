#include "recomp/achievements/sources/trophy.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>

#include "mini_xml.h"

namespace recomp::achievements::trophy {
namespace {

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }

void SetError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool IEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i]))) return false;
  }
  return true;
}

bool IEndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && IEquals(s.substr(s.size() - suffix.size()), suffix);
}

std::optional<int32_t> ParseInt(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  int32_t v = 0;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || end != s.data() + s.size() || s.empty()) return std::nullopt;
  return v;
}

constexpr size_t kEntrySize = 0x40;  // name[32], offset u64, size u64, flags u32, pad[12]

// Header sizes seen in the wild: 0x40 for v1/v2 (PS3), 0x60 for v3 (adds a
// key index and padding). Try the one the version suggests first.
bool ParseEntries(const std::vector<uint8_t>& data, size_t header_size, uint32_t count,
                  size_t stride, std::vector<TrpEntry>& out) {
  out.clear();
  if (header_size + uint64_t(count) * stride > data.size()) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = data.data() + header_size + size_t(i) * stride;
    size_t len = 0;
    while (len < 32 && e[len] != 0) {
      if (e[len] < 0x20 || e[len] > 0x7E) return false;
      ++len;
    }
    if (len == 0 || len == 32) return false;
    TrpEntry entry;
    entry.name.assign(reinterpret_cast<const char*>(e), len);
    entry.offset = Be64(e + 32);
    entry.size = Be64(e + 40);
    entry.flags = Be32(e + 48);
    if (entry.offset > data.size() || entry.size > data.size() - entry.offset) return false;
    out.push_back(std::move(entry));
  }
  return true;
}

}  // namespace

// ---- TRP ------------------------------------------------------------------

bool TrpArchive::Load(std::span<const uint8_t> data, std::string* error) {
  return Load(std::vector<uint8_t>(data.begin(), data.end()), error);
}

bool TrpArchive::Load(std::vector<uint8_t>&& data, std::string* error) {
  *this = TrpArchive();
  data_ = std::move(data);
  if (data_.size() < 4) {
    SetError(error, "TROPHY.TRP: file too small");
    return false;
  }
  const uint32_t magic = Be32(data_.data());
  if (magic == 0x4E504400) {  // "NPD\0"
    SetError(error,
             "TROPHY.TRP is EDAT/NPDRM encrypted; decrypt it first (the reader expects the "
             "decrypted TROPHY.TRP from PS3_GAME/TROPDIR)");
    return false;
  }
  if (magic == 0x53434500) {  // "SCE\0"
    SetError(error, "TROPHY.TRP is inside an encrypted SCE container; decrypt it first");
    return false;
  }
  if (magic != kTrpMagic || data_.size() < 0x40) {
    SetError(error, "not a TROPHY.TRP (missing 0xDCA24D00 magic)");
    return false;
  }
  version_ = Be32(data_.data() + 4);
  const uint32_t count = Be32(data_.data() + 16);
  const uint32_t element_size = Be32(data_.data() + 20);
  const size_t stride = element_size >= kEntrySize && element_size <= 0x1000 ? element_size : kEntrySize;

  const size_t order_v3[] = {0x60, 0x40};
  const size_t order_v1[] = {0x40, 0x60};
  bool ok = false;
  for (size_t header_size : version_ >= 3 ? order_v3 : order_v1) {
    if (ParseEntries(data_, header_size, count, stride, entries_)) {
      ok = true;
      break;
    }
  }
  if (!ok) {
    entries_.clear();
    SetError(error, "TROPHY.TRP: unreadable file table (version " + std::to_string(version_) + ")");
    return false;
  }
  valid_ = true;
  return true;
}

bool TrpArchive::LoadFile(const std::filesystem::path& path, std::string* error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    *this = TrpArchive();
    SetError(error, "cannot open " + path.string());
    return false;
  }
  const std::streamsize size = f.tellg();
  std::vector<uint8_t> bytes(size > 0 ? size_t(size) : 0);
  f.seekg(0);
  if (size > 0 && !f.read(reinterpret_cast<char*>(bytes.data()), size)) {
    *this = TrpArchive();
    SetError(error, "cannot read " + path.string());
    return false;
  }
  return Load(std::move(bytes), error);
}

std::span<const uint8_t> TrpArchive::Find(std::string_view name) const {
  for (const TrpEntry& e : entries_) {
    if (IEquals(e.name, name)) return {data_.data() + e.offset, size_t(e.size)};
  }
  return {};
}

std::string TrpArchive::FindText(std::string_view name) const {
  auto bytes = Find(name);
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// ---- Grades ---------------------------------------------------------------

Grade GradeFromType(std::string_view ttype) {
  if (ttype.empty()) return Grade::kUnknown;
  switch (std::toupper(uint8_t(ttype[0]))) {
    case 'P': return Grade::kPlatinum;
    case 'G': return Grade::kGold;
    case 'S': return Grade::kSilver;
    case 'B': return Grade::kBronze;
    default: return Grade::kUnknown;
  }
}

Tier TierFromGrade(Grade grade) {
  switch (grade) {
    case Grade::kPlatinum: return Tier::kPlatinum;
    case Grade::kGold: return Tier::kGold;
    case Grade::kSilver: return Tier::kSilver;
    case Grade::kBronze: return Tier::kBronze;
    default: return Tier::kNone;
  }
}

uint32_t PointsForGrade(Grade grade) {
  switch (grade) {
    case Grade::kPlatinum: return 180;
    case Grade::kGold: return 90;
    case Grade::kSilver: return 30;
    case Grade::kBronze: return 15;
    default: return 0;
  }
}

// ---- SFM ------------------------------------------------------------------

const TrophyDef* TrophyConfig::Find(int32_t id) const {
  for (const TrophyDef& t : trophies) {
    if (t.id == id) return &t;
  }
  return nullptr;
}

bool ParseTrophyConf(std::string_view xml, TrophyConfig* out, std::string* error) {
  *out = TrophyConfig();
  detail::XmlNode root;
  if (!detail::ParseXml(xml, &root, error)) return false;
  if (root.name != "trophyconf") {
    SetError(error, "SFM root element is <" + root.name + ">, expected <trophyconf>");
    return false;
  }
  out->npcommid = root.child_text("npcommid");
  out->trophyset_version = root.child_text("trophyset-version");
  out->title_name = root.child_text("title-name");
  out->title_detail = root.child_text("title-detail");
  for (const detail::XmlNode& node : root.children) {
    if (node.name == "group") {
      TrophyGroup g;
      g.id = ParseInt(node.attr("id")).value_or(-1);
      g.name = node.child_text("name");
      g.detail = node.child_text("detail");
      out->groups.push_back(std::move(g));
    } else if (node.name == "trophy") {
      auto id = ParseInt(node.attr("id"));
      if (!id || *id < 0) {
        SetError(error, "<trophy> with missing or bad id \"" + node.attr("id") + "\"");
        return false;
      }
      TrophyDef t;
      t.id = *id;
      t.grade = GradeFromType(node.attr("ttype"));
      t.hidden = IEquals(node.attr("hidden"), "yes");
      t.pid = ParseInt(node.attr("pid", "-1")).value_or(kInvalidTrophyId);
      t.gid = node.has_attr("gid") ? ParseInt(node.attr("gid")).value_or(-1) : -1;
      t.name = node.child_text("name");
      t.detail = node.child_text("detail");
      out->trophies.push_back(std::move(t));
    }
  }
  return true;
}

void ApplyLocalization(TrophyConfig& base, const TrophyConfig& localized) {
  if (!localized.title_name.empty()) base.title_name = localized.title_name;
  if (!localized.title_detail.empty()) base.title_detail = localized.title_detail;
  for (const TrophyGroup& lg : localized.groups) {
    for (TrophyGroup& g : base.groups) {
      if (g.id != lg.id) continue;
      if (!lg.name.empty()) g.name = lg.name;
      if (!lg.detail.empty()) g.detail = lg.detail;
    }
  }
  for (const TrophyDef& lt : localized.trophies) {
    for (TrophyDef& t : base.trophies) {
      if (t.id != lt.id) continue;
      if (!lt.name.empty()) t.name = lt.name;
      if (!lt.detail.empty()) t.detail = lt.detail;
    }
  }
}

std::string LocalizedFileName(int language) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "TROP_%02d.SFM", language);
  return buf;
}

std::string TrophyIconName(int32_t id) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "TROP%03d.PNG", id);
  return buf;
}

// ---- TrophyFile -----------------------------------------------------------

namespace {

bool LooksLikeXml(std::string_view text) {
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  while (!text.empty() && (text.front() == ' ' || text.front() == '\r' || text.front() == '\n' ||
                           text.front() == '\t'))
    text.remove_prefix(1);
  return text.starts_with("<");
}

bool ParseSfm(const TrpArchive& archive, std::string_view name, TrophyConfig* out,
              std::string* error) {
  std::string text = archive.FindText(name);
  if (!LooksLikeXml(text)) {
    SetError(error, std::string(name) + " is not XML (encrypted or damaged)");
    return false;
  }
  std::string parse_error;
  if (!ParseTrophyConf(text, out, &parse_error)) {
    SetError(error, std::string(name) + ": " + parse_error);
    return false;
  }
  return true;
}

}  // namespace

bool TrophyFile::Load(TrpArchive archive, int language, std::string* error) {
  archive_ = TrpArchive();
  config_ = TrophyConfig();
  if (!archive.valid()) {
    SetError(error, "TROPHY.TRP was not loaded");
    return false;
  }
  const char* base_name = !archive.Find("TROPCONF.SFM").empty() ? "TROPCONF.SFM"
                          : !archive.Find("TROP.SFM").empty()   ? "TROP.SFM"
                                                                : nullptr;
  if (!base_name) {
    const bool encrypted = std::any_of(archive.entries().begin(), archive.entries().end(),
                                       [](const TrpEntry& e) { return IEndsWith(e.name, ".ESFM"); });
    SetError(error, encrypted ? "TROPHY.TRP holds only encrypted .ESFM files (PS4/Vita format); "
                                "decrypt them to .SFM first"
                              : "TROPHY.TRP has no TROPCONF.SFM");
    return false;
  }
  TrophyConfig config;
  if (!ParseSfm(archive, base_name, &config, error)) return false;

  // Localized text is optional: a bad localized file falls back silently.
  std::string localized_name;
  if (language >= 0 && !archive.Find(LocalizedFileName(language)).empty()) {
    localized_name = LocalizedFileName(language);
  } else if (std::string_view(base_name) != "TROP.SFM" && !archive.Find("TROP.SFM").empty()) {
    localized_name = "TROP.SFM";
  }
  if (!localized_name.empty()) {
    TrophyConfig localized;
    if (ParseSfm(archive, localized_name, &localized, nullptr)) ApplyLocalization(config, localized);
  }

  archive_ = std::move(archive);
  config_ = std::move(config);
  return true;
}

bool TrophyFile::LoadFile(const std::filesystem::path& trp, int language, std::string* error) {
  TrpArchive archive;
  if (!archive.LoadFile(trp, error)) {
    archive_ = TrpArchive();
    config_ = TrophyConfig();
    return false;
  }
  return Load(std::move(archive), language, error);
}

std::span<const uint8_t> TrophyFile::trophy_icon(int32_t id) const {
  return archive_.Find(TrophyIconName(id));
}

std::span<const uint8_t> TrophyFile::ResolveIcon(std::string_view ref) const {
  constexpr std::string_view kPrefix = "trophy:";
  if (!ref.starts_with(kPrefix)) return {};
  ref.remove_prefix(kPrefix.size());
  if (ref.empty()) return {};
  return archive_.Find(ref);
}

AchievementSet TrophyFile::ToAchievementSet(const SetOptions& options) const {
  AchievementSet set;
  set.id = options.set_id;
  set.platform = Platform::kPlayStation3;
  set.title = options.title.empty() ? config_.title_name : options.title;
  set.category = options.category;
  set.source = {{"platform", "ps3"},
                {"npcommid", config_.npcommid},
                {"trophyset_version", config_.trophyset_version}};
  uint32_t order = 0;
  for (const TrophyDef& t : config_.trophies) {
    Achievement a;
    a.local_id = std::to_string(t.id);
    a.platform_id = uint32_t(t.id);
    a.set_id = set.id;
    a.name = t.name;
    a.description = t.detail;
    a.tier = TierFromGrade(t.grade);
    a.points = PointsForGrade(t.grade);
    a.secret = t.hidden;
    a.display_order = order++;
    a.icon = "trophy:" + TrophyIconName(t.id);
    if (t.gid >= 0) {
      for (const TrophyGroup& g : config_.groups) {
        if (g.id == t.gid && !g.name.empty()) a.category = g.name;
      }
    }
    set.achievements.push_back(std::move(a));
  }
  return set;
}

}  // namespace recomp::achievements::trophy
