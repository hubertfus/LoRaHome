#pragma once

#include <cstddef>
#include <cstdint>

namespace lorahome {

/**
 * Fixed-capacity rolling-window duty cycle tracker — the Bridge's own
 * enforcement of the ETSI 1% limit, independent of the Host's
 * (packages/host/src/duty-cycle-guard). See ARCHITECTURE.md §7: two
 * independent layers of defense, neither trusting the other. No dynamic
 * allocation — a fixed ring buffer bounds how many past transmissions we
 * remember.
 */
class DutyCycleTracker {
 public:
  DutyCycleTracker(float limitFraction, uint32_t windowMs) : limitFraction_(limitFraction), windowMs_(windowMs) {}

  /** Returns true if transmitting a frame of `durationMs` would exceed the duty cycle budget. */
  bool wouldExceed(uint32_t durationMs, uint32_t nowMs) {
    prune(nowMs);
    return usedMs_ + durationMs > static_cast<uint32_t>(limitFraction_ * windowMs_);
  }

  /** Records a transmission if it fits the budget. Returns false (and records nothing) if it doesn't. */
  bool tryRecord(uint32_t durationMs, uint32_t nowMs) {
    if (wouldExceed(durationMs, nowMs)) return false;

    entries_[head_] = {nowMs, durationMs};
    head_ = (head_ + 1) % kCapacity;
    if (count_ < kCapacity) count_++;
    usedMs_ += durationMs;
    return true;
  }

 private:
  struct Entry {
    uint32_t atMs;
    uint32_t durationMs;
  };

  // Bounds how many past transmissions we track; once full, the oldest
  // entry is evicted on the next record even if still inside the window.
  // At typical LoRa frame rates this is generously large for a 1h window.
  static constexpr size_t kCapacity = 256;

  void prune(uint32_t nowMs) {
    while (count_ > 0) {
      const size_t oldestIdx = (head_ + kCapacity - count_) % kCapacity;
      const Entry& oldest = entries_[oldestIdx];
      if (nowMs - oldest.atMs <= windowMs_) break;
      usedMs_ -= oldest.durationMs;
      count_--;
    }
  }

  Entry entries_[kCapacity] = {};
  size_t head_ = 0;
  size_t count_ = 0;
  uint32_t usedMs_ = 0;

  float limitFraction_;
  uint32_t windowMs_;
};

}  // namespace lorahome
