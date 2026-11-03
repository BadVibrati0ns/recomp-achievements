#include "mini_xml.h"
#include "recomp/achievements/sources/trophy.h"
#include "recomp/achievements/sources/trophy_context.h"
#include "recomp/achievements/system.h"
#include "test_util.h"

using namespace recomp::achievements;
using namespace recomp::achievements::trophy;

namespace {

// Shaped like a real TROPCONF.SFM, including the signature comment.
constexpr std::string_view kConf = R"(<!--Sce-Np-Trophy-Signature: AAAA-->
<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<!DOCTYPE trophyconf [ <!ELEMENT trophyconf ANY> ]>
<trophyconf version="1.1" policy="large">
<npcommid>NPWR00000_00</npcommid>
<trophyset-version>01.00</trophyset-version>
<parental-level license-area="default">1</parental-level>
<title-name>Test &amp; Game</title-name>
<title-detail>A <![CDATA[<test>]]> game</title-detail>
<group id="001"><name>DLC Pack</name><detail>Extra</detail></group>
<trophy id="000" hidden="no" ttype="P" pid="-1"><name>Platinum</name><detail>Get them all</detail></trophy>
<trophy id="001" hidden="no" ttype="B" pid="000"><name>Bronze &#x2605;</name><detail>Do a thing</detail></trophy>
<trophy id="002" hidden="yes" ttype="S" pid="000"><name>Secret</name><detail>Shh</detail></trophy>
<trophy id="003" hidden="no" ttype="G" pid="000"><name>Gold</name><detail>Hard</detail></trophy>
<trophy id="004" hidden="no" ttype="B" pid="-1" gid="001"><name>DLC Bronze</name><detail>Not for platinum</detail></trophy>
</trophyconf>
)";

constexpr std::string_view kGerman = R"(<?xml version="1.0" encoding="utf-8"?>
<trophyconf version="1.1">
<title-name>Testspiel</title-name>
<trophy id="001"><name>Bronze DE</name><detail>Etwas tun</detail></trophy>
</trophyconf>
)";

std::vector<uint8_t> BuildTrp(uint32_t version = 2) {
  test::TrpBuilder b;
  b.version = version;
  b.Add("TROPCONF.SFM", kConf);
  b.Add("TROP.SFM", kConf);
  b.Add("TROP_04.SFM", kGerman);
  b.Add("ICON0.PNG", std::vector<uint8_t>{0x89, 'P', 'N', 'G', 0});
  b.Add("TROP001.PNG", std::vector<uint8_t>{0x89, 'P', 'N', 'G', 1, 1});
  return b.Build();
}

TrophyFile LoadFile(int language = 1, uint32_t version = 2) {
  TrpArchive archive;
  std::string error;
  CHECK(archive.Load(BuildTrp(version), &error));
  TrophyFile file;
  CHECK(file.Load(std::move(archive), language, &error));
  if (!error.empty()) std::printf("  error: %s\n", error.c_str());
  return file;
}

}  // namespace

TEST(XmlReader) {
  detail::XmlNode root;
  std::string error;
  CHECK(detail::ParseXml("<a x='1' y=\"&lt;2&gt;\"><b>t&#65;&#x42;</b><c/>tail&bogus;</a>", &root, &error));
  CHECK_EQ(root.name, "a");
  CHECK_EQ(root.attr("x"), "1");
  CHECK_EQ(root.attr("y"), "<2>");
  CHECK_EQ(root.attr("z", "none"), "none");
  CHECK_EQ(root.child_text("b"), "tAB");
  CHECK(root.child("c") != nullptr);
  CHECK_EQ(root.trimmed_text(), "tail&bogus;");
  CHECK(!detail::ParseXml("<a><b></a>", &root, &error));
  CHECK(!error.empty());
  CHECK(!detail::ParseXml("<a>", &root, nullptr));
  CHECK(!detail::ParseXml("", &root, nullptr));
  CHECK(!detail::ParseXml("<!-- never closed", &root, nullptr));
}

TEST(TrophyConfParses) {
  TrophyConfig config;
  std::string error;
  CHECK(ParseTrophyConf(kConf, &config, &error));
  CHECK_EQ(config.npcommid, "NPWR00000_00");
  CHECK_EQ(config.title_name, "Test & Game");
  CHECK_EQ(config.title_detail, "A <test> game");
  CHECK_EQ(config.trophies.size(), size_t(5));
  CHECK_EQ(config.groups.size(), size_t(1));
  const TrophyDef* t = config.Find(1);
  CHECK(t != nullptr);
  if (!t) return;
  CHECK_EQ(t->name, "Bronze \xE2\x98\x85");
  CHECK(t->grade == Grade::kBronze);
  CHECK_EQ(t->pid, 0);
  CHECK(config.Find(2)->hidden);
  CHECK(config.Find(0)->grade == Grade::kPlatinum);
  CHECK_EQ(config.Find(0)->pid, -1);
  CHECK_EQ(config.Find(4)->gid, 1);

  CHECK(!ParseTrophyConf("<other/>", &config, &error));
  CHECK(!ParseTrophyConf("<trophyconf><trophy id=\"x\"/></trophyconf>", &config, &error));
}

TEST(TrpArchiveVersions) {
  for (uint32_t version : {1u, 2u, 3u}) {
    TrpArchive archive;
    std::string error;
    CHECK(archive.Load(BuildTrp(version), &error));
    CHECK_EQ(archive.version(), version);
    CHECK_EQ(archive.entries().size(), size_t(5));
    CHECK_EQ(archive.Find("trop001.png").size(), size_t(6));  // case-insensitive
    CHECK(archive.Find("TROP999.PNG").empty());
    CHECK(archive.FindText("TROPCONF.SFM").starts_with("<!--Sce"));
  }
}

TEST(TrpRejectsEncryptedAndBroken) {
  TrpArchive archive;
  std::string error;
  test::Bytes npd;
  npd.str("NPD");
  npd.u8(0);
  npd.zeros(60);
  CHECK(!archive.Load(npd.v, &error));
  CHECK(error.find("EDAT") != std::string::npos);
  test::Bytes sce;
  sce.str("SCE");
  sce.u8(0);
  sce.zeros(60);
  CHECK(!archive.Load(sce.v, &error));
  CHECK(error.find("encrypted") != std::string::npos);
  CHECK(!archive.Load(std::vector<uint8_t>(80, 0), &error));
  CHECK(error.find("0xDCA24D00") != std::string::npos);

  auto bytes = BuildTrp();
  bytes.resize(0x90);  // table cut short
  CHECK(!archive.Load(std::move(bytes), &error));
  CHECK(!archive.valid());

  // Only .ESFM (PS4/Vita, encrypted) inside.
  test::TrpBuilder esfm;
  esfm.version = 3;
  esfm.Add("TROPCONF.ESFM", std::vector<uint8_t>(64, 0xAB));
  CHECK(archive.Load(esfm.Build(), &error));
  TrophyFile file;
  CHECK(!file.Load(std::move(archive), 1, &error));
  CHECK(error.find("ESFM") != std::string::npos);

  // SFM that isn't XML.
  test::TrpBuilder garbage;
  garbage.Add("TROPCONF.SFM", std::vector<uint8_t>(64, 0xAB));
  CHECK(archive.Load(garbage.Build(), &error));
  CHECK(!file.Load(std::move(archive), 1, &error));
  CHECK(error.find("not XML") != std::string::npos);
}

TEST(TrophyLocalization) {
  TrophyFile german = LoadFile(4);
  CHECK_EQ(german.config().title_name, "Testspiel");
  CHECK_EQ(german.config().Find(1)->name, "Bronze DE");
  CHECK_EQ(german.config().Find(3)->name, "Gold");  // not in TROP_04: base text
  CHECK(german.config().Find(1)->grade == Grade::kBronze);  // structure kept
  TrophyFile english = LoadFile(1);  // no TROP_01: TROP.SFM
  CHECK_EQ(english.config().Find(1)->name, "Bronze \xE2\x98\x85");
  CHECK_EQ(LocalizedFileName(4), "TROP_04.SFM");
}

TEST(TrophyToAchievementSet) {
  TrophyFile file = LoadFile();
  TrophyFile::SetOptions options;
  options.set_id = "ps3";
  options.category = "Base";
  AchievementSet set = file.ToAchievementSet(options);
  CHECK(set.platform == Platform::kPlayStation3);
  CHECK_EQ(set.title, "Test & Game");
  CHECK_EQ(set.source.value("npcommid", ""), "NPWR00000_00");
  CHECK_EQ(set.achievements.size(), size_t(5));
  if (set.achievements.size() != 5) return;
  const Achievement& plat = set.achievements[0];
  CHECK(plat.tier == Tier::kPlatinum);
  CHECK_EQ(plat.points, 180u);
  CHECK_EQ(plat.icon, "trophy:TROP000.PNG");
  CHECK_EQ(set.achievements[1].points, 15u);
  CHECK_EQ(set.achievements[1].platform_id, 1u);
  CHECK_EQ(set.achievements[1].local_id, "1");
  CHECK(set.achievements[2].secret);
  CHECK_EQ(set.achievements[2].points, 30u);
  CHECK(set.achievements[3].tier == Tier::kGold);
  CHECK_EQ(set.achievements[3].points, 90u);
  CHECK_EQ(set.achievements[4].category, "DLC Pack");
  CHECK_EQ(set.achievements[1].category, "");  // set default applies

  CHECK_EQ(file.ResolveIcon("trophy:TROP001.PNG").size(), size_t(6));
  CHECK(file.ResolveIcon("trophy:TROP000.PNG").empty());
  CHECK(file.ResolveIcon("xdbf:1").empty());
  CHECK_EQ(file.game_icon().size(), size_t(5));
}

TEST(TrophyFlagArrayLayout) {
  FlagArray f;
  f.set(0), f.set(31), f.set(32), f.set(127), f.set(128);
  CHECK_EQ(f.flag_bits[0], 0x80000001u);
  CHECK_EQ(f.flag_bits[1], 1u);
  CHECK_EQ(f.flag_bits[3], 0x80000000u);
  CHECK(f.test(127) && !f.test(128) && !f.test(1));
  CHECK_EQ(UnixToRtcTick(0), 62135596800000000ull);
}

TEST(TrophyContextUnlocks) {
  System system;
  TrophyContext ctx(system, "ps3");
  int32_t plat = 123;
  CHECK(ctx.UnlockTrophy(1, &plat) == Result::kNotRegistered);
  CHECK_EQ(plat, kInvalidTrophyId);
  CHECK(ctx.Register(LoadFile(), {}) == Result::kOk);
  CHECK(system.FindSet("ps3") != nullptr);

  CHECK(ctx.UnlockTrophy(0, &plat) == Result::kCannotUnlockPlatinum);
  CHECK(ctx.UnlockTrophy(42, &plat) == Result::kInvalidTrophyId);
  CHECK(ctx.UnlockTrophy(1, &plat) == Result::kOk);
  CHECK_EQ(plat, kInvalidTrophyId);
  CHECK(ctx.UnlockTrophy(1, &plat) == Result::kAlreadyUnlocked);
  CHECK(ctx.UnlockTrophy(4, &plat) == Result::kOk);  // DLC, not part of the platinum
  CHECK(ctx.UnlockTrophy(2, &plat) == Result::kOk);
  CHECK_EQ(plat, kInvalidTrophyId);

  FlagArray flags;
  uint32_t count = 0;
  CHECK(ctx.GetTrophyUnlockState(&flags, &count) == Result::kOk);
  CHECK_EQ(count, 5u);
  CHECK_EQ(flags.flag_bits[0], 0x16u);  // 1, 2, 4
  CHECK(ctx.GetTrophyUnlockState(nullptr, &count) == Result::kInvalidArgument);

  CHECK(ctx.UnlockTrophy(3, &plat) == Result::kOk);
  CHECK_EQ(plat, 0);  // last one completes the platinum
  CHECK(system.IsUnlocked("ps3:0"));
  CHECK(ctx.UnlockTrophy(0, &plat) == Result::kCannotUnlockPlatinum);

  GameDetails details;
  GameData data;
  CHECK(ctx.GetGameInfo(&details, &data) == Result::kOk);
  CHECK_EQ(details.num_trophies, 5u);
  CHECK_EQ(details.num_platinum, 1u);
  CHECK_EQ(details.num_gold, 1u);
  CHECK_EQ(details.num_silver, 1u);
  CHECK_EQ(details.num_bronze, 2u);
  CHECK_EQ(details.title, "Test & Game");
  CHECK_EQ(data.unlocked_trophies, 5u);
  CHECK_EQ(data.unlocked_platinum, 1u);
  CHECK_EQ(data.unlocked_bronze, 2u);

  TrophyDetails td;
  TrophyData tdata;
  CHECK(ctx.GetTrophyInfo(3, &td, &tdata) == Result::kOk);
  CHECK(td.grade == Grade::kGold);
  CHECK_EQ(td.name, "Gold");
  CHECK(tdata.unlocked);
  CHECK(tdata.timestamp == 0 || tdata.timestamp > UnixToRtcTick(0));
  CHECK(ctx.GetTrophyInfo(99, &td, &tdata) == Result::kInvalidTrophyId);
  CHECK_EQ(ctx.GetTrophyIcon(1).size(), size_t(6));
  CHECK_EQ(ctx.GetGameIcon().size(), size_t(5));
}

TEST(TrophyContextHiddenAndSync) {
  System system;
  TrophyContext ctx(system, "ps3");
  CHECK(ctx.Register(LoadFile(), {}) == Result::kOk);
  TrophyDetails td;
  TrophyData tdata;
  CHECK(ctx.GetTrophyInfo(2, &td, &tdata) == Result::kOk);
  CHECK(td.hidden);
  CHECK_EQ(td.name, "");  // locked hidden trophy
  CHECK(!tdata.unlocked);
  CHECK_EQ(tdata.timestamp, 0u);
  CHECK(ctx.GetTrophyInfo(2, &td, nullptr, true) == Result::kOk);
  CHECK_EQ(td.name, "Secret");

  // Unlocked behind the context's back (e.g. an older save): SyncPlatinum
  // catches the platinum up.
  CHECK(system.Unlock("ps3", 1));
  CHECK(system.Unlock("ps3", 2));
  CHECK(system.Unlock("ps3", 3));
  CHECK(!system.IsUnlocked("ps3:0"));
  CHECK_EQ(ctx.SyncPlatinum(), 1u);
  CHECK(system.IsUnlocked("ps3:0"));
  CHECK_EQ(ctx.SyncPlatinum(), 0u);
}

TEST(TrophyContextRegisterFromPath) {
  test::TempDir dir;
  test::WriteFile(dir.path / "TROPHY.TRP", BuildTrp());
  System system;
  TrophyContext ctx(system, "ps3");
  std::string error;
  CHECK(ctx.Register(dir.path / "missing.TRP", &error) == Result::kLoadFailed);
  CHECK(!error.empty());
  CHECK(!ctx.registered());
  TrophyContext::RegisterOptions options;
  options.language = 4;
  CHECK(ctx.Register(dir.path / "TROPHY.TRP", options, &error) == Result::kOk);
  CHECK(ctx.registered());
  CHECK_EQ(system.FindSet("ps3")->title, "Testspiel");
}
