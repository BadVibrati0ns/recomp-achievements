#include "recomp/achievements/rexglue.h"

#include <cstdio>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>

#include "recomp/achievements/system.h"

namespace recomp::achievements {

struct RexglueBinding::State {
  rex::system::KernelState* kernel_state = nullptr;
  System* system = nullptr;
  std::string set_id;
  RexglueOptions options;
  xbox::XdbfTitle title;
  std::filesystem::path tu_path;
  uint32_t title_id = 0;
  std::unique_ptr<xbox::XamAchievements> xam;
  bool bound = false;
  std::mutex mutex;  // Guards bound against Unbind racing a kernel callback.
};

namespace {

xbox::Language ResolveLanguage(uint32_t requested) {
  uint32_t lang = requested ? requested : REXCVAR_QUERY(uint32_t, user_language);
  return lang ? xbox::Language(lang) : xbox::Language::kEnglish;
}

// title_xdbf() only wraps guest memory and doesn't expose its size, so read
// the same resource section (named after the title id) ourselves and copy it.
bool ReadTitleXdbf(rex::system::KernelState* ks, xbox::XdbfDatabase& out, uint32_t& title_id) {
  auto module = ks->GetExecutableModule();
  if (!module) return false;
  title_id = module->title_id();
  char name[9];
  std::snprintf(name, sizeof(name), "%08X", title_id);
  uint32_t data = 0, size = 0;
  if (XFAILED(module->GetSection(name, &data, &size)) || !data || !size) return false;
  const uint8_t* bytes = ks->memory()->TranslateVirtual<const uint8_t*>(data);
  return bytes && out.Load(std::span<const uint8_t>(bytes, size));
}

RexglueTitle LoadTitle(rex::system::KernelState* ks, const RexglueOptions& options) {
  RexglueTitle out;
  if (!ks) return out;
  xbox::XdbfDatabase base;
  if (ReadTitleXdbf(ks, base, out.title_id)) {
    out.title.SetBase(std::move(base));
  } else {
    out.title_id = ks->title_id();
    REXLOG_WARN("recomp-achievements: no title XDBF (is the executable module loaded?)");
  }

  if (options.load_title_update) {
    auto roots = options.content_roots;
    if (roots.empty()) {
      std::error_code ec;
      const auto cwd = std::filesystem::current_path(ec);
      roots = {cwd, cwd / "game"};
    }
    auto tu_path = xbox::FindTitleUpdateSpa(roots, out.title_id);
    if (!tu_path.empty()) {
      xbox::XdbfDatabase tu;
      std::string error;
      if (tu.LoadFile(tu_path, &error)) {
        out.title.SetTitleUpdate(std::move(tu));
        out.title_update_path = std::move(tu_path);
        REXLOG_INFO("recomp-achievements: title update database {}", out.title_update_path.string());
      } else {
        REXLOG_WARN("recomp-achievements: {}: {}", tu_path.string(), error);
      }
    }
  }
  return out;
}

bool LoadInto(RexglueBinding::State& s) {
  RexglueTitle loaded = LoadTitle(s.kernel_state, s.options);
  s.title = std::move(loaded.title);
  s.tu_path = std::move(loaded.title_update_path);
  s.title_id = loaded.title_id;
  if (s.options.add_set && s.title.valid()) {
    AchievementSet set = s.title.ToAchievementSet(RexglueSetOptions(s.set_id, s.options));
    REXLOG_INFO("recomp-achievements: {} achievements for {:08X} as set '{}'",
                set.achievements.size(), s.title_id, s.set_id);
    s.system->AddSet(std::move(set));
  }
  return s.title.valid();
}

void Hook(const std::shared_ptr<RexglueBinding::State>& state) {
#if RECOMP_ACH_REXGLUE_KERNEL_HOOKS
  std::weak_ptr<RexglueBinding::State> weak = state;
  auto* ks = state->kernel_state;

  ks->SetAchievementUnlockCallback([weak](uint32_t user_index, uint32_t id) {
    auto s = weak.lock();
    if (!s) return;
    std::lock_guard lock(s->mutex);
    if (!s->bound) return;
    // One XUSER_ACHIEVEMENT, in the guest's big-endian layout.
    const uint8_t entry[8] = {uint8_t(user_index >> 24), uint8_t(user_index >> 16),
                              uint8_t(user_index >> 8), uint8_t(user_index),
                              uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)};
    s->xam->WriteAchievements(entry, 1);
  });

  ks->SetIsAchievementUnlockedCallback([weak](uint32_t, uint32_t id) {
    auto s = weak.lock();
    if (!s) return false;
    std::lock_guard lock(s->mutex);
    return s->bound && s->xam->IsUnlocked(id);
  });

  ks->SetShowAchievementsUICallback([weak](uint32_t user_index, uint32_t title_id) {
    auto s = weak.lock();
    if (!s) return;
    {
      std::lock_guard lock(s->mutex);
      if (!s->bound) return;
    }
    // Outside the lock: the consumer may open UI that queries the System.
    s->xam->ShowAchievementsUI(user_index, title_id);
  });
#else
  (void)state;
#endif
}

}  // namespace

RexglueBinding::RexglueBinding(std::shared_ptr<State> state) : state_(std::move(state)) {}

RexglueBinding::~RexglueBinding() { Unbind(); }

void RexglueBinding::Unbind() {
  if (!state_) return;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->bound) return;
    state_->bound = false;
  }
#if RECOMP_ACH_REXGLUE_KERNEL_HOOKS
  auto* ks = state_->kernel_state;
  ks->SetAchievementUnlockCallback(nullptr);
  ks->SetIsAchievementUnlockedCallback(nullptr);
  ks->SetShowAchievementsUICallback(nullptr);
#endif
}

bool RexglueBinding::Reload() { return LoadInto(*state_); }

const xbox::XdbfTitle& RexglueBinding::title() const { return state_->title; }
const std::filesystem::path& RexglueBinding::title_update_path() const { return state_->tu_path; }
const std::string& RexglueBinding::set_id() const { return state_->set_id; }
uint32_t RexglueBinding::title_id() const { return state_->title_id; }
xbox::XamAchievements& RexglueBinding::xam() { return *state_->xam; }
bool RexglueBinding::hooks_kernel() const { return RECOMP_ACH_REXGLUE_KERNEL_HOOKS != 0; }

std::span<const uint8_t> RexglueBinding::ResolveIcon(std::string_view ref) const {
  return state_->title.ResolveIcon(ref);
}

RexglueTitle LoadRexglueTitle(rex::system::KernelState* kernel_state, const RexglueOptions& options) {
  return LoadTitle(kernel_state, options);
}

xbox::XdbfSetOptions RexglueSetOptions(std::string set_id, const RexglueOptions& options) {
  xbox::XdbfSetOptions set_options;
  set_options.set_id = std::move(set_id);
  set_options.language = ResolveLanguage(options.language);
  set_options.category = options.category;
  set_options.title = options.title;
  return set_options;
}

std::unique_ptr<RexglueBinding> BindRexglue(rex::system::KernelState* kernel_state, System& system,
                                            std::string set_id, RexglueOptions options) {
  if (!kernel_state) return nullptr;
  auto state = std::make_shared<RexglueBinding::State>();
  state->kernel_state = kernel_state;
  state->system = &system;
  state->set_id = std::move(set_id);
  state->options = std::move(options);
  state->xam = std::make_unique<xbox::XamAchievements>(system, state->set_id);
  state->xam->SetUnknownHandler([](uint32_t, uint32_t id) {
    REXLOG_WARN("recomp-achievements: game wrote unknown achievement {}", id);
  });
  if (state->options.on_unknown_achievement) {
    state->xam->SetUnknownHandler([warn = state->options.on_unknown_achievement](uint32_t user, uint32_t id) {
      REXLOG_WARN("recomp-achievements: game wrote unknown achievement {}", id);
      warn(user, id);
    });
  }
  state->xam->SetShowHandler(state->options.on_show_achievements);
  LoadInto(*state);
  state->bound = true;
  Hook(state);
  return std::unique_ptr<RexglueBinding>(new RexglueBinding(std::move(state)));
}

}  // namespace recomp::achievements
