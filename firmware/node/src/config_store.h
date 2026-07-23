#pragma once

#include <cstddef>
#include <cstdint>

namespace lorahome {

enum class SlotId : uint8_t { A = 0, B = 1 };
enum class SlotStatus : uint8_t { INACTIVE = 0, STAGING = 1, PENDING_ACTIVATION = 2, ACTIVE = 3 };

/**
 * NVS-backed A/B config store — see ARCHITECTURE.md §4. A new config is
 * always written to the currently-inactive slot; the active slot is never
 * overwritten in place. promoteStagedToActive() must only be called after
 * one full stable boot cycle on the staged config (no crash-loop, no
 * watchdog reset) — the caller (scheduler/main) owns that decision.
 */
class ConfigStore {
 public:
  bool begin();

  SlotId activeSlot() const { return active_; }
  SlotStatus statusOf(SlotId slot) const;

  /** Writes `data` into the currently-inactive slot, marked STAGING. */
  bool stageConfig(const uint8_t* data, size_t len);

  /** Promotes the staged slot to ACTIVE and demotes the old active slot to INACTIVE. */
  void promoteStagedToActive();

  /** Loads the active slot's raw config bytes. Returns false if no slot is ACTIVE yet. */
  bool loadActive(uint8_t* out_buf, size_t out_buf_cap, size_t* out_len) const;

 private:
  SlotId active_ = SlotId::A;
  SlotId staged_ = SlotId::B;
  bool has_staged_ = false;

  const char* namespaceFor(SlotId slot) const { return slot == SlotId::A ? "lh_cfg_a" : "lh_cfg_b"; }
};

}  // namespace lorahome
