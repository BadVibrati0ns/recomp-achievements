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

bool LoadInto(RexglueBinding::State& s) {
  xbox::XdbfTitle title;
  xbox::XdbfDatabase base;
  uint32_t title_id = 0;
  if (ReadTitleXdbf(s.kernel_state, base, title_id)) {
    title.SetBase(std::move(base));
  } else {
    title_id = s.kernel_state->title_id();
    REXLOG_WARN("recomp-achievements: no title XDBF (is the executable module loaded?)");
  }

  std::filesystem::path tu_path;
  if (s.options.load_title_update) {
    auto roots = s.options.content_roots;
    if (roots.empty()) {
      std::error_code ec;
      const auto cwd = std::filesystem::current_path(ec);
      roots = {cwd, cwd / "game"};
    }
    tu_path = xbox::FindTitleUpdateSpa(roots, title_id);
    if (!tu_path.empty()) {
      xbox::XdbfDatabase tu;
      std::string error;
      if (tu.LoadFile(tu_path, &error)) {
        title.SetTitleUpdate(std::move(tu));
        REXLOG_INFO("recomp-achievements: title update database {}", tu_path.string());
      } else {
        REXLOG_WARN("recomp-achievements: {}: {}", tu_path.string(), error);
        tu_path.clear();
      }
    }
  }

  s.title = std::move(title);
  s.tu_path = std::move(tu_path);
  s.title_id = title_id;
  if (s.options.add_set && s.title.valid()) {
    xbox::XdbfSetOptions set_options;
    set_options.set_id = s.set_id;
    set_options.language = ResolveLanguage(s.options.language);
    set_options.category = s.options.category;
    set_options.title = s.options.title;
    AchievementSet set = s.title.ToAchievementSet(set_options);
    REXLOG_INFO("recomp-achievements: {} achievements for {:08X} as set '{}'",
                set.achievements.size(), title_id, s.set_id);
    s.system->AddSet(std::move(set));
  }
  return s.title.valid();
}

void Hook(const std::shared_ptr<RexglueBinding::State>& state) {
  std::weak_ptr<RexglueBinding::State> weak = state;
  auto* ks = state->kernel_state;

  ks->SetAchievementUnlockCallback([weak](uint32_t user_index, uint32_t id) {
    auto s = weak.lock();
    if (!s) return;
    std::lock_guard lock(s->mutex);
    if (!s->bound) return;
    // Only one local profile exists, so every user index unlocks it.
    if (!s->system->FindByPlatformId(s->set_id, id)) {
      REXLOG_WARN("recomp-achievements: game wrote unknown achievement {}", id);
      if (s->options.on_unknown_achievement) s->options.on_unknown_achievement(user_index, id);
      return;
    }
    s->system->Unlock(s->set_id, id);
  });

  ks->SetIsAchievementUnlockedCallback([weak](uint32_t, uint32_t id) {
    auto s = weak.lock();
    if (!s) return false;
    std::lock_guard lock(s->mutex);
    return s->bound && s->system->IsUnlocked(s->set_id, id);
  });

  ks->SetShowAchievementsUICallback([weak](uint32_t user_index, uint32_t title_id) {
    auto s = weak.lock();
    if (!s) return;
    std::function<void(uint32_t, uint32_t)> show;
    {
      std::lock_guard lock(s->mutex);
      if (!s->bound) return;
      show = s->options.on_show_achievements;
    }
    // Outside the lock: the consumer may open UI that queries the System.
    if (show) show(user_index, title_id);
  });
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
  auto* ks = state_->kernel_state;
  ks->SetAchievementUnlockCallback(nullptr);
  ks->SetIsAchievementUnlockedCallback(nullptr);
  ks->SetShowAchievementsUICallback(nullptr);
}

bool RexglueBinding::Reload() { return LoadInto(*state_); }

const xbox::XdbfTitle& RexglueBinding::title() const { return state_->title; }
const std::filesystem::path& RexglueBinding::title_update_path() const { return state_->tu_path; }
const std::string& RexglueBinding::set_id() const { return state_->set_id; }
uint32_t RexglueBinding::title_id() const { return state_->title_id; }

std::span<const uint8_t> RexglueBinding::ResolveIcon(std::string_view ref) const {
  return state_->title.ResolveIcon(ref);
}

std::unique_ptr<RexglueBinding> BindRexglue(rex::system::KernelState* kernel_state, System& system,
                                            std::string set_id, RexglueOptions options) {
  if (!kernel_state) return nullptr;
  auto state = std::make_shared<RexglueBinding::State>();
  state->kernel_state = kernel_state;
  state->system = &system;
  state->set_id = std::move(set_id);
  state->options = std::move(options);
  LoadInto(*state);
  state->bound = true;
  Hook(state);
  return std::unique_ptr<RexglueBinding>(new RexglueBinding(std::move(state)));
}

}  // namespace recomp::achievements
