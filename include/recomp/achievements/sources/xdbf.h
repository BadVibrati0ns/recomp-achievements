#pragma once

// Xbox 360 XDBF (SPA) reader: the title database embedded in a XEX as the
// "<title id>" resource section, and the spa.bin shipped in title updates and
// marketplace content. Standalone: no rexglue or other framework needed.
//
// Layout (all big-endian): a 24-byte header, entry_count 18-byte entries
// (section, id, offset, size), free_count 8-byte free-space records, then the
// content area that entry offsets are relative to.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "recomp/achievements/types.h"

namespace recomp::achievements::xbox {

// XLanguage values used as XSTR string-table ids.
enum class Language : uint32_t {
  kInvalid = 0,
  kEnglish = 1,
  kJapanese = 2,
  kGerman = 3,
  kFrench = 4,
  kSpanish = 5,
  kItalian = 6,
  kKorean = 7,
  kTChinese = 8,
  kPortuguese = 9,
  kSChinese = 10,
  kPolish = 11,
  kRussian = 12,
};

enum class Section : uint16_t {
  kMetadata = 1,     // XACH, XSTC, XTHD, ... keyed by their fourcc
  kImage = 2,        // PNGs keyed by image id (0x8000 = title icon)
  kStringTable = 3,  // XSTR tables keyed by Language
};

inline constexpr uint64_t kTitleStringId = 0x8000;  // Title name string / icon image.

struct XdbfAchievement {
  uint16_t id = 0;
  uint16_t label_id = 0;
  uint16_t description_id = 0;
  uint16_t unachieved_id = 0;
  uint32_t image_id = 0;
  uint16_t gamerscore = 0;
  uint32_t flags = 0;
  // Bit 3 is "show details while locked"; achievements without it are secret.
  bool secret() const { return (flags & 0x8) == 0; }
};

// XTHD: title id and version of the database's title.
struct TitleInfo {
  uint32_t title_id = 0;
  uint32_t title_type = 0;
  uint16_t major = 0, minor = 0, build = 0, revision = 0;
  uint64_t version_key() const {
    return (uint64_t(major) << 48) | (uint64_t(minor) << 32) | (uint64_t(build) << 16) | revision;
  }
  std::string version_string() const;  // "1.0.607.0"
};

// One parsed XDBF. Owns a copy of the bytes, so it can outlive guest memory.
class XdbfDatabase {
 public:
  XdbfDatabase() = default;

  bool Load(std::span<const uint8_t> data, std::string* error = nullptr);
  bool Load(std::vector<uint8_t>&& data, std::string* error = nullptr);
  bool LoadFile(const std::filesystem::path& path, std::string* error = nullptr);
  bool valid() const { return valid_; }
  const std::vector<uint8_t>& bytes() const { return data_; }

  // Empty span when missing or out of bounds.
  std::span<const uint8_t> GetEntry(Section section, uint64_t id) const;
  std::span<const uint8_t> image(uint64_t image_id) const { return GetEntry(Section::kImage, image_id); }

  // XSTC default language; kEnglish when the database has none.
  Language default_language() const;
  // Languages that have an XSTR table, in file order.
  std::vector<Language> languages() const;
  bool HasLanguage(Language language) const;

  // The string in exactly that language, if the table has it.
  std::optional<std::string> FindString(Language language, uint16_t string_id) const;
  // preferred, then English, then the default language, then any table.
  std::string GetString(uint16_t string_id, Language preferred = Language::kEnglish) const;

  std::vector<XdbfAchievement> achievements() const;
  std::optional<TitleInfo> title_info() const;
  std::string title(Language preferred = Language::kEnglish) const {
    return GetString(uint16_t(kTitleStringId), preferred);
  }

 private:
  struct Entry {
    uint16_t section;
    uint64_t id;
    uint32_t offset;
    uint32_t size;
  };
  std::vector<uint8_t> data_;
  std::vector<Entry> entries_;
  size_t content_offset_ = 0;
  bool valid_ = false;
};

struct XdbfSetOptions {
  std::string set_id;  // Required.
  Language language = Language::kEnglish;
  std::string category;  // Default category for the set.
  std::string title;     // Overrides the XDBF title name.
};

// The base database (from the XEX) with an optional title update / content
// spa.bin over it. Lookups try the TU first, then the base, per language.
class XdbfTitle {
 public:
  void SetBase(XdbfDatabase db) { base_ = std::move(db); }
  void SetTitleUpdate(XdbfDatabase db) { update_ = std::move(db); }
  const XdbfDatabase& base() const { return base_; }
  const XdbfDatabase& title_update() const { return update_; }
  bool valid() const { return base_.valid() || update_.valid(); }

  std::string GetString(uint16_t string_id, Language preferred = Language::kEnglish) const;
  // Union of both XACH tables; a TU entry replaces the base entry with its id.
  // Base order first, then ids only the TU has.
  std::vector<XdbfAchievement> achievements() const;
  std::optional<TitleInfo> title_info() const;
  std::string title(Language preferred = Language::kEnglish) const {
    return GetString(uint16_t(kTitleStringId), preferred);
  }
  std::span<const uint8_t> image(uint64_t image_id) const;

  // platform kXbox360, platform_id = XDBF id, icon "xdbf:<image id>",
  // points = gamerscore, locked_description = the unachieved string.
  AchievementSet ToAchievementSet(const XdbfSetOptions& options) const;

  // Resolves an icon reference made by ToAchievementSet ("xdbf:<id>", decimal
  // or 0x-hex) to PNG bytes. Empty span for other references or missing ids.
  std::span<const uint8_t> ResolveIcon(std::string_view ref) const;

 private:
  XdbfDatabase base_;
  XdbfDatabase update_;
};

// Parses "xdbf:<id>" (decimal or 0x-prefixed hex).
std::optional<uint64_t> ParseIconRef(std::string_view ref);

// Searches each root for title update / marketplace spa.bin files of title_id:
//   <root>/0000000000000000/<TITLEID>/00000002/**/spa.bin   (marketplace content)
//   <root>/0000000000000000/<TITLEID>/000B0000/**/spa.bin   (title updates)
//   <root>/spa.bin
// Files whose XTHD names another title are skipped. The best is the highest
// XTHD title version, then the most achievements, then the largest file.
// Returns an empty path when nothing is found.
std::filesystem::path FindTitleUpdateSpa(const std::vector<std::filesystem::path>& roots,
                                         uint32_t title_id);

}  // namespace recomp::achievements::xbox
