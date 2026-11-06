#pragma once

// The seam for a later accounts/sync service. Nothing in the SDK talks to a
// network; an app (not a game) would implement this over its own service and
// merge with MergeEvents(). See docs/library.md for the trust model: unlocks
// are self-reported, so an account can display them but not vouch for them.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "recomp/achievements/library_reader.h"

namespace recomp::achievements {

class SyncProvider {
 public:
  virtual ~SyncProvider() = default;

  virtual bool signed_in() const = 0;
  // Display name of the account; empty when signed out.
  virtual std::string account() const = 0;
  // Sends local events. The service keeps the union, deduplicated by
  // (install_id, t, key, kind), so sending one twice is harmless. False when
  // nothing was accepted (signed out, offline).
  virtual bool Push(std::span<const LibraryEvent> events) = 0;
  // Events from every install of the account at or after since_ms.
  virtual std::vector<LibraryEvent> Pull(uint64_t since_ms) = 0;
};

// Never signed in; pushes nothing, pulls nothing.
class NullSyncProvider final : public SyncProvider {
 public:
  bool signed_in() const override { return false; }
  std::string account() const override { return {}; }
  bool Push(std::span<const LibraryEvent>) override { return false; }
  std::vector<LibraryEvent> Pull(uint64_t) override { return {}; }
};

}  // namespace recomp::achievements
