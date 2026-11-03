#pragma once

// PlayStation 3 trophy reader: TROPHY.TRP archives and the TROPCONF.SFM /
// TROP.SFM / TROP_XX.SFM XML inside them. No dependencies.
//
// The reader expects a decrypted TROPHY.TRP, as found under
// PS3_GAME/TROPDIR/<NPWRxxxxx_xx>/ of a decrypted disc or an installed game.
// Files taken straight from an encrypted source (EDAT/NPDRM "NPD", SCE/SELF
// containers, or PS4/Vita .ESFM entries) are rejected with an error that says
// so; decrypt them first (for example with RPCS3's "Decrypt PS3 binaries" or
// an EDAT tool).

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "recomp/achievements/types.h"

namespace recomp::achievements::trophy {

inline constexpr uint32_t kTrpMagic = 0xDCA24D00;
inline constexpr int32_t kInvalidTrophyId = -1;  // SCE_NP_TROPHY_INVALID_TROPHY_ID

// One file stored in a TRP. The SHA-1 and signature fields are not checked.
struct TrpEntry {
  std::string name;
  uint64_t offset = 0;
  uint64_t size = 0;
  uint32_t flags = 0;
};

class TrpArchive {
 public:
  bool Load(std::vector<uint8_t>&& data, std::string* error = nullptr);
  bool Load(std::span<const uint8_t> data, std::string* error = nullptr);
  bool LoadFile(const std::filesystem::path& path, std::string* error = nullptr);
  bool valid() const { return valid_; }

  uint32_t version() const { return version_; }
  const std::vector<TrpEntry>& entries() const { return entries_; }
  // Case-insensitive name lookup. Empty span when missing.
  std::span<const uint8_t> Find(std::string_view name) const;
  std::string FindText(std::string_view name) const;

 private:
  std::vector<uint8_t> data_;
  std::vector<TrpEntry> entries_;
  uint32_t version_ = 0;
  bool valid_ = false;
};

// SCE_NP_TROPHY_GRADE_* values.
enum class Grade : uint32_t { kUnknown = 0, kPlatinum = 1, kGold = 2, kSilver = 3, kBronze = 4 };

Grade GradeFromType(std::string_view ttype);  // "P", "G", "S", "B"
Tier TierFromGrade(Grade grade);
// PSN trophy points: bronze 15, silver 30, gold 90, platinum 180.
uint32_t PointsForGrade(Grade grade);

struct TrophyDef {
  int32_t id = 0;
  Grade grade = Grade::kUnknown;
  bool hidden = false;
  int32_t pid = kInvalidTrophyId;  // Platinum this trophy counts towards.
  int32_t gid = -1;                // Group (DLC pack); -1 = base game.
  std::string name;
  std::string detail;
};

struct TrophyGroup {
  int32_t id = 0;
  std::string name;
  std::string detail;
};

struct TrophyConfig {
  std::string npcommid;
  std::string trophyset_version;
  std::string title_name;
  std::string title_detail;
  std::vector<TrophyGroup> groups;
  std::vector<TrophyDef> trophies;  // In file order.

  const TrophyDef* Find(int32_t id) const;
};

// Parses TROPCONF.SFM / TROP.SFM / TROP_XX.SFM text. Tolerates the
// signature comment at the top, the XML declaration, CDATA and entities.
bool ParseTrophyConf(std::string_view xml, TrophyConfig* out, std::string* error = nullptr);

// Copies names and details from a localized file over `base`, by id. Entries
// missing from the localized file keep the base text.
void ApplyLocalization(TrophyConfig& base, const TrophyConfig& localized);

// "TROP_01.SFM" for CELL_SYSUTIL_LANG_* language 1 (English).
std::string LocalizedFileName(int language);

// A loaded trophy set: the archive plus its parsed, localized config.
class TrophyFile {
 public:
  // language is a CELL_SYSUTIL_LANG_* value (0 Japanese, 1 English, ...), or
  // -1 for the set's default text. Falls back to TROP.SFM, then TROPCONF.SFM.
  bool Load(TrpArchive archive, int language = 1, std::string* error = nullptr);
  bool LoadFile(const std::filesystem::path& trp, int language = 1, std::string* error = nullptr);
  bool valid() const { return archive_.valid(); }

  const TrpArchive& archive() const { return archive_; }
  const TrophyConfig& config() const { return config_; }
  // ICON0.PNG.
  std::span<const uint8_t> game_icon() const { return archive_.Find("ICON0.PNG"); }
  std::span<const uint8_t> trophy_icon(int32_t id) const;
  // Resolves "trophy:<file>" (as made by ToAchievementSet) to the file's
  // bytes. Empty span for other references or missing files.
  std::span<const uint8_t> ResolveIcon(std::string_view ref) const;

  struct SetOptions {
    std::string set_id;    // Required.
    std::string category;  // Default category; DLC groups use their group name.
    std::string title;     // Overrides title-name.
  };
  // platform kPlayStation3, platform_id = trophy id, icon
  // "trophy:TROP###.PNG", tier and PSN points from the grade, secret from
  // hidden="yes".
  AchievementSet ToAchievementSet(const SetOptions& options) const;

 private:
  TrpArchive archive_;
  TrophyConfig config_;
};

std::string TrophyIconName(int32_t id);  // "TROP007.PNG"

}  // namespace recomp::achievements::trophy
