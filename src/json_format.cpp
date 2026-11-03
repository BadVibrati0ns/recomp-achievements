#include "json_format.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace recomp::achievements::detail {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::string_view kNativeSuffix = ".achievements.json";
// Old packs cap each achievement and the pack as a whole.
constexpr uint32_t kPackPointsCap = 1000;
constexpr uint32_t kPackAchievementCap = 500;

std::string PathString(const fs::path& path) {
  const auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

bool EndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

// Keys starting with '$' or '_' are comments or editor hints ("$schema").
bool IsCommentKey(std::string_view key) {
  return !key.empty() && (key[0] == '$' || key[0] == '_');
}

// ---- Native format ---------------------------------------------------------
// Errors are thrown as ParseError and turned into the error string at the top,
// so every check can name its location without threading a result through.

struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Reader {
 public:
  explicit Reader(std::string origin) : origin_(std::move(origin)) {}

  [[noreturn]] void Fail(std::string_view where, std::string_view message) const {
    std::string text = origin_;
    if (!where.empty()) {
      text += ": ";
      text += where;
    }
    text += ": ";
    text += message;
    throw ParseError(text);
  }

  void ExpectObject(const json& j, std::string_view where) const {
    if (!j.is_object()) Fail(where, std::string("expected an object, got ") + j.type_name());
  }

  void CheckKeys(const json& obj, std::string_view where,
                 std::initializer_list<std::string_view> known) const {
    for (const auto& [key, value] : obj.items()) {
      if (IsCommentKey(key)) continue;
      if (std::find(known.begin(), known.end(), key) != known.end()) continue;
      std::string message = "unknown field \"" + key + "\" (expected one of:";
      for (const auto k : known) {
        message += ' ';
        message += k;
      }
      message += ")";
      Fail(where, message);
    }
  }

  std::optional<std::string> String(const json& obj, const char* key, std::string_view where) const {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return std::nullopt;
    if (!it->is_string()) Fail(Field(where, key), std::string("expected a string, got ") + it->type_name());
    return it->get<std::string>();
  }

  std::optional<bool> Bool(const json& obj, const char* key, std::string_view where) const {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return std::nullopt;
    if (!it->is_boolean()) Fail(Field(where, key), std::string("expected true or false, got ") + it->type_name());
    return it->get<bool>();
  }

  std::optional<double> Number(const json& obj, const char* key, std::string_view where) const {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return std::nullopt;
    if (!it->is_number()) Fail(Field(where, key), std::string("expected a number, got ") + it->type_name());
    return it->get<double>();
  }

  std::optional<uint32_t> U32(const json& obj, const char* key, std::string_view where) const {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return std::nullopt;
    return ToU32(*it, Field(where, key));
  }

  uint32_t ToU32(const json& j, std::string_view where) const {
    if (j.is_number_unsigned() && j.get<uint64_t>() <= std::numeric_limits<uint32_t>::max()) {
      return j.get<uint32_t>();
    }
    if (j.is_number_integer() && j.get<int64_t>() >= 0 &&
        j.get<int64_t>() <= std::numeric_limits<uint32_t>::max()) {
      return static_cast<uint32_t>(j.get<int64_t>());
    }
    if (j.is_number_float()) {
      const double d = j.get<double>();
      if (d >= 0 && d <= std::numeric_limits<uint32_t>::max() && std::floor(d) == d) {
        return static_cast<uint32_t>(d);
      }
    }
    Fail(where, "expected a whole number from 0 to 4294967295, got " + j.dump());
  }

  std::vector<std::string> Strings(const json& obj, const char* key, std::string_view where,
                                   bool allow_numbers = false) const {
    std::vector<std::string> out;
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return out;
    const std::string field = Field(where, key);
    if (!it->is_array()) Fail(field, std::string("expected an array of strings, got ") + it->type_name());
    for (size_t i = 0; i < it->size(); ++i) {
      const json& item = (*it)[i];
      const std::string item_where = field + "[" + std::to_string(i) + "]";
      if (item.is_string()) {
        if (item.get_ref<const std::string&>().empty()) Fail(item_where, "must not be empty");
        out.push_back(item.get<std::string>());
      } else if (allow_numbers && item.is_number_integer()) {
        out.push_back(item.dump());
      } else {
        Fail(item_where, std::string("expected a string, got ") + item.type_name());
      }
    }
    return out;
  }

  std::vector<Credit> Credits(const json& obj, std::string_view where) const {
    std::vector<Credit> out;
    const auto it = obj.find("credits");
    if (it == obj.end() || it->is_null()) return out;
    const std::string field = Field(where, "credits");
    if (!it->is_array()) Fail(field, std::string("expected an array, got ") + it->type_name());
    for (size_t i = 0; i < it->size(); ++i) {
      const json& item = (*it)[i];
      const std::string item_where = field + "[" + std::to_string(i) + "]";
      Credit credit;
      if (item.is_string()) {
        credit.name = item.get<std::string>();
      } else {
        ExpectObject(item, item_where);
        CheckKeys(item, item_where, {"name", "role", "url"});
        credit.name = String(item, "name", item_where).value_or("");
        credit.role = String(item, "role", item_where).value_or("");
        credit.url = String(item, "url", item_where).value_or("");
      }
      if (credit.name.empty()) Fail(item_where, "a credit needs a \"name\"");
      out.push_back(std::move(credit));
    }
    return out;
  }

  static std::string Field(std::string_view where, std::string_view key) {
    std::string out(where);
    if (!out.empty()) out += '.';
    out += key;
    return out;
  }

 private:
  std::string origin_;
};

void ParseTrigger(const Reader& r, const json& a, std::string_view where, Achievement& out) {
  Trigger& t = out.trigger;
  const auto it = a.find("trigger");
  const std::string field = Reader::Field(where, "trigger");
  bool explicit_type = false;
  if (it == a.end() || it->is_null()) {
    t.type = "manual";
    explicit_type = true;
  } else if (it->is_string()) {
    t.type = it->get<std::string>();
    explicit_type = true;
  } else if (it->is_object()) {
    const json& obj = *it;
    for (const auto& [key, value] : obj.items()) {
      if (key == "type") {
        if (!value.is_string()) r.Fail(field + ".type", "expected a string");
        t.type = value.get<std::string>();
        explicit_type = true;
      } else if (key == "stat") {
        t.stat = r.String(obj, "stat", field).value_or("");
      } else if (key == "event") {
        t.event = r.String(obj, "event", field).value_or("");
      } else if (key == "target") {
        if (value.is_number_integer()) {
          t.target = value.dump();
        } else {
          t.target = r.String(obj, "target", field).value_or("");
        }
      } else if (key == "threshold") {
        t.threshold = r.Number(obj, "threshold", field).value_or(0.0);
      } else if (key == "params") {
        r.ExpectObject(value, field + ".params");
        for (const auto& [pk, pv] : value.items()) t.params[pk] = pv;
      } else if (!IsCommentKey(key)) {
        // Game-defined and memory triggers read their extra fields from params.
        t.params[key] = value;
      }
    }
    if (!explicit_type) {
      if (!t.stat.empty()) {
        t.type = "stat";
      } else if (t.params.contains("condition")) {
        t.type = "memory";
      } else if (!t.event.empty()) {
        t.type = out.checklist.empty() ? "event" : "checklist";
      } else {
        t.type = "manual";
      }
    }
  } else {
    r.Fail(field, std::string("expected a type name or an object, got ") + it->type_name());
  }

  if (t.type.empty()) r.Fail(field, "the trigger type must not be empty");
  const std::string type_where = field + " (\"" + t.type + "\")";
  if (t.type == "stat" && t.stat.empty()) r.Fail(type_where, "needs a \"stat\" name");
  if ((t.type == "event" || t.type == "checklist" || t.type == "count") && t.event.empty()) {
    r.Fail(type_where, "needs an \"event\" name");
  }
  if (t.type == "checklist" && out.checklist.empty()) {
    r.Fail(type_where, "needs a non-empty \"checklist\" on the achievement");
  }
  if (t.type == "count" && t.threshold < 1.0) {
    r.Fail(type_where, "needs a \"threshold\" of at least 1");
  }
  if (t.type == "memory" && !(t.params.contains("condition") && t.params["condition"].is_string())) {
    r.Fail(type_where, "needs a \"condition\" string");
  }
  if (t.params.contains("distinct") && !t.params["distinct"].is_boolean()) {
    r.Fail(field + ".distinct", "expected true or false");
  }
}

Achievement ParseNativeAchievement(const Reader& r, const json& a, size_t index) {
  std::string where = "achievements[" + std::to_string(index) + "]";
  r.ExpectObject(a, where);
  if (const auto name = a.find("name"); name != a.end() && name->is_string()) {
    where += " (\"" + name->get<std::string>() + "\")";
  }
  r.CheckKeys(a, where,
              {"id", "platform_id", "name", "description", "locked_description", "category",
               "points", "tier", "secret", "display_order", "icon", "locked_icon", "legacy_keys",
               "credits", "checklist", "trigger"});

  Achievement out;
  out.platform_id = r.U32(a, "platform_id", where).value_or(0);
  if (const auto id = a.find("id"); id != a.end() && !id->is_null()) {
    if (id->is_string()) {
      out.local_id = id->get<std::string>();
    } else if (id->is_number()) {
      out.local_id = std::to_string(r.ToU32(*id, where + ".id"));
    } else {
      r.Fail(where + ".id", std::string("expected a string or number, got ") + id->type_name());
    }
    if (out.local_id.empty()) r.Fail(where + ".id", "must not be empty");
  } else if (a.contains("platform_id") && !a["platform_id"].is_null()) {
    out.local_id = std::to_string(out.platform_id);
  } else {
    r.Fail(where, "needs an \"id\" (or a \"platform_id\" to use as one)");
  }

  out.name = r.String(a, "name", where).value_or("");
  if (out.name.empty()) r.Fail(where, "needs a \"name\"");
  out.description = r.String(a, "description", where).value_or("");
  out.locked_description = r.String(a, "locked_description", where).value_or("");
  out.category = r.String(a, "category", where).value_or("");
  out.points = r.U32(a, "points", where).value_or(0);
  if (const auto tier = r.String(a, "tier", where)) {
    const auto parsed = ParseTier(*tier);
    if (!parsed) r.Fail(where + ".tier", "\"" + *tier + "\" is not one of: none bronze silver gold platinum");
    out.tier = *parsed;
  }
  out.secret = r.Bool(a, "secret", where).value_or(false);
  // File order is the display order unless the file says otherwise.
  out.display_order = r.U32(a, "display_order", where).value_or(static_cast<uint32_t>(index));
  out.icon = r.String(a, "icon", where).value_or("");
  out.locked_icon = r.String(a, "locked_icon", where).value_or("");
  out.legacy_keys = r.Strings(a, "legacy_keys", where, /*allow_numbers=*/true);
  out.credits = r.Credits(a, where);
  out.checklist = r.Strings(a, "checklist", where);
  ParseTrigger(r, a, where, out);
  return out;
}

std::string DefaultSetId(const fs::path& origin) {
  const std::string file = PathString(origin.filename());
  if (EndsWith(file, kNativeSuffix) && file.size() > kNativeSuffix.size()) {
    return file.substr(0, file.size() - kNativeSuffix.size());
  }
  if (file == "achievements.json" || file == "pack.json") {
    return PathString(origin.parent_path().filename());
  }
  return PathString(origin.stem());
}

// ---- Old pack format -------------------------------------------------------
// Lenient like the old loader: wrong-typed fields fall back to defaults.

std::string LooseString(const json& obj, const char* key, std::string fallback = {}) {
  const auto it = obj.find(key);
  if (it != obj.end() && it->is_string()) return it->get<std::string>();
  return fallback;
}

// A string, or an integer written as text (codes and ids are sometimes numbers).
std::optional<std::string> LooseText(const json& obj, const char* key) {
  const auto it = obj.find(key);
  if (it == obj.end()) return std::nullopt;
  if (it->is_string()) return it->get<std::string>();
  if (it->is_number_integer()) return it->dump();
  return std::nullopt;
}

std::optional<double> LooseNumber(const json& obj, const char* key) {
  const auto it = obj.find(key);
  if (it != obj.end() && it->is_number()) return it->get<double>();
  return std::nullopt;
}

uint32_t LooseU32(const json& j) {
  if (j.is_number_unsigned()) {
    return static_cast<uint32_t>(std::min<uint64_t>(j.get<uint64_t>(), std::numeric_limits<uint32_t>::max()));
  }
  if (j.is_number_integer()) {
    return static_cast<uint32_t>(std::clamp<int64_t>(j.get<int64_t>(), 0, std::numeric_limits<uint32_t>::max()));
  }
  if (j.is_number_float()) {
    // Truncates like the old loader's get<uint32_t>().
    return static_cast<uint32_t>(std::clamp(j.get<double>(), 0.0, double(std::numeric_limits<uint32_t>::max())));
  }
  return 0;
}

// Fields that become Achievement members; everything else is kept in params.
bool IsPackMemberField(std::string_view key) {
  static const std::set<std::string_view> kMembers = {
      "id",          "name",        "description", "unachieved_desc", "locked_description",
      "category",    "gamerscore",  "points",      "is_secret",       "secret",
      "icon",        "locked_icon", "retail_id",   "platform_id",     "trigger",
      "tricks",      "checklist",   "legacy_keys", "display_order",   "tier"};
  return kMembers.contains(key);
}

std::optional<Achievement> ParsePackAchievement(const json& a, size_t index) {
  if (!a.is_object()) return std::nullopt;
  Achievement out;
  if (const auto it = a.find("id"); it != a.end()) {
    if (it->is_string()) {
      out.local_id = it->get<std::string>();
    } else if (it->is_number()) {
      out.local_id = std::to_string(LooseU32(*it));
    }
  }
  if (out.local_id.empty()) out.local_id = LooseString(a, "name");
  // The old loader numbered these from its global definition count, which
  // a set loader can't know; packs in the wild always have an id or name.
  if (out.local_id.empty()) out.local_id = "ach_" + std::to_string(index);

  out.name = LooseString(a, "name", "Mod Achievement (" + out.local_id + ")");
  out.description = LooseString(a, "description");
  out.locked_description =
      LooseString(a, "unachieved_desc", LooseString(a, "locked_description", out.description));
  out.category = LooseString(a, "category");
  uint32_t points = 0;
  if (const auto it = a.find("gamerscore"); it != a.end()) {
    points = LooseU32(*it);
  } else if (const auto p = a.find("points"); p != a.end()) {
    points = LooseU32(*p);
  }
  out.points = std::min(points, kPackAchievementCap);
  if (const auto it = a.find("is_secret"); it != a.end() && it->is_boolean()) {
    out.secret = it->get<bool>();
  } else if (const auto s = a.find("secret"); s != a.end() && s->is_boolean()) {
    out.secret = s->get<bool>();
  }
  if (const auto it = a.find("retail_id"); it != a.end() && it->is_number_unsigned()) {
    out.platform_id = LooseU32(*it);
  } else if (const auto p = a.find("platform_id"); p != a.end() && p->is_number_unsigned()) {
    out.platform_id = LooseU32(*p);
  }
  out.icon = LooseString(a, "icon");
  out.locked_icon = LooseString(a, "locked_icon");
  out.display_order = static_cast<uint32_t>(index);
  if (const auto it = a.find("display_order"); it != a.end()) out.display_order = LooseU32(*it);
  if (const auto it = a.find("tier"); it != a.end() && it->is_string()) {
    out.tier = ParseTier(it->get<std::string>()).value_or(Tier::kNone);
  }
  if (const auto it = a.find("legacy_keys"); it != a.end() && it->is_array()) {
    for (const auto& k : *it) {
      if (k.is_string()) out.legacy_keys.push_back(k.get<std::string>());
      else if (k.is_number_integer()) out.legacy_keys.push_back(k.dump());
    }
  }
  for (const char* key : {"tricks", "checklist"}) {
    const auto it = a.find(key);
    if (it == a.end() || !it->is_array()) continue;
    for (const auto& item : *it) {
      if (item.is_string() && !item.get_ref<const std::string&>().empty()) {
        out.checklist.push_back(item.get<std::string>());
      }
    }
    break;
  }

  Trigger& t = out.trigger;
  t.type = "manual";
  if (const auto it = a.find("trigger"); it != a.end()) {
    if (it->is_string() && !it->get_ref<const std::string&>().empty()) {
      t.type = it->get<std::string>();
    } else if (it->is_object()) {
      // A native trigger object inside an old pack: take it as written.
      const json& obj = *it;
      t.type = LooseString(obj, "type", "manual");
      t.stat = LooseString(obj, "stat");
      t.event = LooseString(obj, "event");
      t.target = LooseText(obj, "target").value_or("");
      t.threshold = LooseNumber(obj, "threshold").value_or(0.0);
      for (const auto& [k, v] : obj.items()) {
        if (k == "params" && v.is_object()) {
          for (const auto& [pk, pv] : v.items()) t.params[pk] = pv;
        } else if (k != "type" && k != "stat" && k != "event" && k != "target" && k != "threshold") {
          t.params[k] = v;
        }
      }
    }
  }
  if (!a.contains("trigger") || !a["trigger"].is_object()) {
    for (const char* key : {"spot", "target", "group", "code"}) {
      if (a.contains(key)) {
        t.target = LooseText(a, key).value_or("");
        break;
      }
    }
    for (const char* key : {"threshold", "count", "score"}) {
      if (a.contains(key)) {
        t.threshold = LooseNumber(a, key).value_or(0.0);
        break;
      }
    }
    t.stat = LooseText(a, "stat").value_or("");
    t.event = LooseText(a, "event").value_or("");
  }
  // Raw fields (radius, position, takeoff, landing, group, ...) stay available
  // so the game can evaluate its own trigger types.
  for (const auto& [key, value] : a.items()) {
    if (!IsPackMemberField(key)) t.params[key] = value;
  }
  return out;
}

json ParseFile(const fs::path& path, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = PathString(path) + ": cannot open file";
    return json(json::value_t::discarded);
  }
  try {
    return json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
  } catch (const json::parse_error& e) {
    if (error) *error = PathString(path) + ": invalid JSON: " + e.what();
  }
  return json(json::value_t::discarded);
}

bool LoadAnyDocument(const json& doc, const fs::path& path, AchievementSet& out, std::string* error) {
  const std::string file = PathString(path.filename());
  if (EndsWith(file, kNativeSuffix) || IsNativeDocument(doc)) {
    return ParseNativeSet(doc, path, out, error);
  }
  std::string pack_error;
  if (ParsePackDocument(doc, PathString(path.parent_path().filename()), path.parent_path(), out,
                        &pack_error)) {
    return true;
  }
  if (error) {
    *error = PathString(path) + ": " +
             (pack_error.empty() ? std::string("pack is disabled or has no achievements") : pack_error);
  }
  return false;
}

}  // namespace

std::string_view PlatformName(Platform platform) {
  switch (platform) {
    case Platform::kXbox360: return "xbox360";
    case Platform::kPlayStation3: return "ps3";
    case Platform::kRetroAchievements: return "retroachievements";
    case Platform::kCustom: break;
  }
  return "custom";
}

std::optional<Platform> ParsePlatform(std::string_view name) {
  const std::string n = Lower(name);
  if (n == "custom") return Platform::kCustom;
  if (n == "xbox360") return Platform::kXbox360;
  if (n == "ps3") return Platform::kPlayStation3;
  if (n == "retroachievements") return Platform::kRetroAchievements;
  return std::nullopt;
}

std::string_view TierName(Tier tier) {
  switch (tier) {
    case Tier::kBronze: return "bronze";
    case Tier::kSilver: return "silver";
    case Tier::kGold: return "gold";
    case Tier::kPlatinum: return "platinum";
    case Tier::kNone: break;
  }
  return "none";
}

std::optional<Tier> ParseTier(std::string_view name) {
  const std::string n = Lower(name);
  if (n == "none" || n.empty()) return Tier::kNone;
  if (n == "bronze") return Tier::kBronze;
  if (n == "silver") return Tier::kSilver;
  if (n == "gold") return Tier::kGold;
  if (n == "platinum") return Tier::kPlatinum;
  return std::nullopt;
}

bool IsNativeDocument(const json& doc) {
  return doc.is_object() && (doc.contains("schema") || doc.contains("set"));
}

bool ParseNativeSet(const json& doc, const fs::path& origin, AchievementSet& out, std::string* error) {
  const Reader r(PathString(origin));
  try {
    if (!doc.is_object()) r.Fail("", "expected an object with \"set\" and \"achievements\"");
    r.CheckKeys(doc, "", {"schema", "set", "achievements"});
    if (const auto schema = r.String(doc, "schema", "")) {
      constexpr std::string_view kPrefix = "recomp-achievements/";
      if (schema->rfind(kPrefix, 0) != 0) {
        r.Fail("schema", "\"" + *schema + "\" is not a recomp-achievements schema (expected \"" +
                             std::string(kSchemaId) + "\")");
      }
      const std::string version = schema->substr(kPrefix.size());
      if (version != "1" && version.rfind("1.", 0) != 0) {
        r.Fail("schema", "version \"" + version + "\" is newer than this SDK supports (1)");
      }
    }

    AchievementSet set;
    set.base_dir = origin.parent_path();
    const json empty = json::object();
    const auto set_it = doc.find("set");
    const json& s = set_it == doc.end() || set_it->is_null() ? empty : *set_it;
    r.ExpectObject(s, "set");
    r.CheckKeys(s, "set", {"id", "title", "platform", "category", "points_cap", "credits", "source"});
    set.id = r.String(s, "id", "set").value_or(DefaultSetId(origin));
    if (set.id.empty()) r.Fail("set.id", "must not be empty");
    set.title = r.String(s, "title", "set").value_or(set.id);
    if (const auto platform = r.String(s, "platform", "set")) {
      const auto parsed = ParsePlatform(*platform);
      if (!parsed) {
        r.Fail("set.platform", "\"" + *platform + "\" is not one of: custom xbox360 ps3 retroachievements");
      }
      set.platform = *parsed;
    }
    set.category = r.String(s, "category", "set").value_or("");
    set.points_cap = r.U32(s, "points_cap", "set").value_or(0);
    set.credits = r.Credits(s, "set");
    if (const auto src = s.find("source"); src != s.end() && !src->is_null()) {
      r.ExpectObject(*src, "set.source");
      set.source = *src;
    }

    const auto list = doc.find("achievements");
    if (list == doc.end() || list->is_null()) r.Fail("", "needs an \"achievements\" array");
    if (!list->is_array()) r.Fail("achievements", std::string("expected an array, got ") + list->type_name());
    std::set<std::string> seen;
    for (size_t i = 0; i < list->size(); ++i) {
      Achievement a = ParseNativeAchievement(r, (*list)[i], i);
      if (!seen.insert(a.local_id).second) {
        r.Fail("achievements[" + std::to_string(i) + "]", "duplicate id \"" + a.local_id + "\"");
      }
      set.achievements.push_back(std::move(a));
    }
    out = std::move(set);
    return true;
  } catch (const ParseError& e) {
    if (error) *error = e.what();
  } catch (const json::exception& e) {
    if (error) *error = PathString(origin) + ": " + e.what();
  }
  return false;
}

json NativeSetToJson(const AchievementSet& set) {
  json s = json::object();
  s["id"] = set.id;
  if (!set.title.empty() && set.title != set.id) s["title"] = set.title;
  if (set.platform != Platform::kCustom) s["platform"] = PlatformName(set.platform);
  if (!set.category.empty()) s["category"] = set.category;
  if (set.points_cap) s["points_cap"] = set.points_cap;
  auto credits_json = [](const std::vector<Credit>& credits) {
    json out = json::array();
    for (const auto& c : credits) {
      json item = {{"name", c.name}};
      if (!c.role.empty()) item["role"] = c.role;
      if (!c.url.empty()) item["url"] = c.url;
      out.push_back(std::move(item));
    }
    return out;
  };
  if (!set.credits.empty()) s["credits"] = credits_json(set.credits);
  if (set.source.is_object() && !set.source.empty()) s["source"] = set.source;

  json list = json::array();
  for (size_t i = 0; i < set.achievements.size(); ++i) {
    const Achievement& a = set.achievements[i];
    json j = json::object();
    j["id"] = a.local_id;
    if (a.platform_id) j["platform_id"] = a.platform_id;
    j["name"] = a.name;
    if (!a.description.empty()) j["description"] = a.description;
    if (!a.locked_description.empty()) j["locked_description"] = a.locked_description;
    if (!a.category.empty() && a.category != set.category) j["category"] = a.category;
    if (a.points) j["points"] = a.points;
    if (a.tier != Tier::kNone) j["tier"] = TierName(a.tier);
    if (a.secret) j["secret"] = true;
    if (a.display_order != i) j["display_order"] = a.display_order;
    if (!a.icon.empty()) j["icon"] = a.icon;
    if (!a.locked_icon.empty()) j["locked_icon"] = a.locked_icon;
    if (!a.legacy_keys.empty()) j["legacy_keys"] = a.legacy_keys;
    if (!a.credits.empty()) j["credits"] = credits_json(a.credits);
    if (!a.checklist.empty()) j["checklist"] = a.checklist;

    const Trigger& t = a.trigger;
    const bool params_empty = !t.params.is_object() || t.params.empty();
    if (t.stat.empty() && t.event.empty() && t.target.empty() && t.threshold == 0.0 && params_empty) {
      if (t.type != "manual") j["trigger"] = t.type;
    } else {
      json trig = {{"type", t.type}};
      if (!t.stat.empty()) trig["stat"] = t.stat;
      if (!t.event.empty()) trig["event"] = t.event;
      if (!t.target.empty()) trig["target"] = t.target;
      if (t.threshold != 0.0) trig["threshold"] = t.threshold;
      if (!params_empty) {
        json nested = json::object();
        for (const auto& [k, v] : t.params.items()) {
          // Extra fields sit beside the known ones unless they'd shadow one.
          const bool shadows = k == "type" || k == "stat" || k == "event" || k == "target" ||
                               k == "threshold" || k == "params" || IsCommentKey(k);
          if (shadows) nested[k] = v;
          else trig[k] = v;
        }
        if (!nested.empty()) trig["params"] = std::move(nested);
      }
      j["trigger"] = std::move(trig);
    }
    list.push_back(std::move(j));
  }
  return json{{"schema", kSchemaId}, {"set", std::move(s)}, {"achievements", std::move(list)}};
}

bool ParsePackDocument(const json& doc, std::string_view folder_name, const fs::path& base_dir,
                       AchievementSet& out, std::string* error) {
  if (error) error->clear();
  std::string category(folder_name);
  std::string title;
  json list = json::array();
  if (doc.is_object()) {
    title = LooseString(doc, "name");
    category = LooseString(doc, "category", title.empty() ? category : title);
    if (const auto it = doc.find("enabled"); it != doc.end() && it->is_boolean() && !it->get<bool>()) {
      return false;
    }
    if (const auto it = doc.find("achievements"); it != doc.end() && it->is_array()) list = *it;
  } else if (doc.is_array()) {
    list = doc;
  } else {
    if (error) *error = "expected an object or an array of achievements";
    return false;
  }
  if (list.empty()) return false;

  AchievementSet set;
  // The set id is the old key prefix, so "<category>:<id>" keys still match saves.
  set.id = category;
  set.title = title.empty() ? category : title;
  set.category = category;
  set.points_cap = kPackPointsCap;
  set.base_dir = base_dir;
  for (size_t i = 0; i < list.size(); ++i) {
    if (auto a = ParsePackAchievement(list[i], i)) set.achievements.push_back(std::move(*a));
  }
  if (set.achievements.empty()) return false;
  out = std::move(set);
  return true;
}

bool LoadSetFile(const fs::path& path, AchievementSet& out, std::string* error) {
  const json doc = ParseFile(path, error);
  if (doc.is_discarded()) return false;
  return LoadAnyDocument(doc, path, out, error);
}

bool LoadSetText(std::string_view text, const fs::path& origin, AchievementSet& out,
                 std::string* error) {
  json doc;
  try {
    doc = json::parse(text, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
  } catch (const json::parse_error& e) {
    if (error) *error = PathString(origin) + ": invalid JSON: " + e.what();
    return false;
  }
  return LoadAnyDocument(doc, origin, out, error);
}

std::vector<AchievementSet> LoadPackFolder(const fs::path& folder, std::vector<std::string>* errors) {
  std::vector<AchievementSet> sets;
  std::error_code ec;
  std::vector<fs::path> native;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    if (entry.is_regular_file(ec) && EndsWith(PathString(entry.path().filename()), kNativeSuffix)) {
      native.push_back(entry.path());
    }
  }
  std::sort(native.begin(), native.end());
  if (native.empty()) {
    // achievements.json wins over pack.json even when it turns out empty.
    for (const char* name : {"achievements.json", "pack.json"}) {
      if (fs::exists(folder / name, ec)) {
        native.push_back(folder / name);
        break;
      }
    }
  }
  for (const auto& path : native) {
    std::string error;
    const json doc = ParseFile(path, &error);
    AchievementSet set;
    if (!doc.is_discarded() && LoadAnyDocument(doc, path, set, &error)) {
      sets.push_back(std::move(set));
    } else if (errors && !error.empty() && error.find("pack is disabled") == std::string::npos) {
      errors->push_back(std::move(error));
    }
  }
  return sets;
}

Progress ProgressFromJson(const json& entry) {
  Progress p;
  if (!entry.is_object()) return p;
  if (const auto it = entry.find("unlocked"); it != entry.end()) {
    p.unlocked = it->is_boolean() ? it->get<bool>() : (it->is_number() && it->get<double>() != 0.0);
  }
  if (const auto it = entry.find("unlock_time"); it != entry.end() && it->is_number()) {
    p.unlock_time = it->is_number_unsigned() ? it->get<uint64_t>()
                                             : static_cast<uint64_t>(std::max(0.0, it->get<double>()));
  }
  auto completed = entry.find("completed");
  if (completed == entry.end() || !completed->is_array()) completed = entry.find("completed_tricks");
  if (completed != entry.end() && completed->is_array()) {
    for (const auto& item : *completed) {
      if (item.is_string()) p.completed.push_back(item.get<std::string>());
    }
  }
  if (const auto it = entry.find("count"); it != entry.end() && it->is_number()) p.count = LooseU32(*it);
  auto best = entry.find("best_value");
  if (best == entry.end() || !best->is_number()) best = entry.find("best_stat");
  if (best != entry.end() && best->is_number()) p.best_value = best->get<double>();
  return p;
}

json ProgressToJson(const Progress& progress, json entry) {
  if (!entry.is_object()) entry = json::object();
  entry.erase("completed_tricks");
  entry.erase("best_stat");
  entry["unlocked"] = progress.unlocked;
  entry["unlock_time"] = progress.unlock_time;
  if (!progress.completed.empty()) entry["completed"] = progress.completed;
  else entry.erase("completed");
  if (progress.count) entry["count"] = progress.count;
  else entry.erase("count");
  if (progress.best_value > 0.0) entry["best_value"] = progress.best_value;
  else entry.erase("best_value");
  return entry;
}

bool HasProgress(const Progress& p) {
  return p.unlocked || !p.completed.empty() || p.count > 0 || p.best_value > 0.0;
}

void ApplyPointsCap(std::vector<Achievement>& achievements, uint32_t cap) {
  if (cap == 0 || achievements.empty()) return;
  uint64_t total = 0;
  for (const auto& a : achievements) total += a.points;
  if (total <= cap) return;

  const double factor = static_cast<double>(cap) / static_cast<double>(total);
  uint64_t scaled_total = 0;
  for (auto& a : achievements) {
    if (a.points > 0) a.points = std::max(1u, static_cast<uint32_t>(std::round(a.points * factor)));
    scaled_total += a.points;
  }
  auto largest = [&] {
    return std::max_element(achievements.begin(), achievements.end(),
                            [](const Achievement& x, const Achievement& y) { return x.points < y.points; });
  };
  while (scaled_total > cap) {
    const auto it = largest();
    if (it->points <= 1) break;
    --it->points;
    --scaled_total;
  }
  while (scaled_total < cap) {
    ++largest()->points;
    ++scaled_total;
  }
}

}  // namespace recomp::achievements::detail
