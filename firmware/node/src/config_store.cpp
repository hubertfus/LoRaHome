#include "config_store.h"

#include <Preferences.h>

namespace lorahome {

namespace {
constexpr const char* kStatusKey = "status";
constexpr const char* kBlobKey = "blob";
constexpr const char* kMetaNamespace = "lh_cfg_meta";
constexpr const char* kActiveSlotKey = "active_slot";
}  // namespace

bool ConfigStore::begin() {
  Preferences meta;
  if (!meta.begin(kMetaNamespace, /*readOnly=*/false)) return false;
  active_ = static_cast<SlotId>(meta.getUChar(kActiveSlotKey, static_cast<uint8_t>(SlotId::A)));
  staged_ = (active_ == SlotId::A) ? SlotId::B : SlotId::A;
  meta.end();
  return true;
}

SlotStatus ConfigStore::statusOf(SlotId slot) const {
  Preferences prefs;
  if (!prefs.begin(namespaceFor(slot), /*readOnly=*/true)) return SlotStatus::INACTIVE;
  const auto status = static_cast<SlotStatus>(prefs.getUChar(kStatusKey, static_cast<uint8_t>(SlotStatus::INACTIVE)));
  prefs.end();
  return status;
}

bool ConfigStore::stageConfig(const uint8_t* data, size_t len) {
  Preferences prefs;
  if (!prefs.begin(namespaceFor(staged_), /*readOnly=*/false)) return false;

  const size_t written = prefs.putBytes(kBlobKey, data, len);
  if (written != len) {
    prefs.end();
    return false;
  }
  prefs.putUChar(kStatusKey, static_cast<uint8_t>(SlotStatus::STAGING));
  prefs.end();

  has_staged_ = true;
  return true;
}

void ConfigStore::promoteStagedToActive() {
  if (!has_staged_) return;

  Preferences newActive;
  if (newActive.begin(namespaceFor(staged_), /*readOnly=*/false)) {
    newActive.putUChar(kStatusKey, static_cast<uint8_t>(SlotStatus::ACTIVE));
    newActive.end();
  }

  Preferences oldActive;
  if (oldActive.begin(namespaceFor(active_), /*readOnly=*/false)) {
    oldActive.putUChar(kStatusKey, static_cast<uint8_t>(SlotStatus::INACTIVE));
    oldActive.end();
  }

  Preferences meta;
  if (meta.begin(kMetaNamespace, /*readOnly=*/false)) {
    meta.putUChar(kActiveSlotKey, static_cast<uint8_t>(staged_));
    meta.end();
  }

  const SlotId old_active = active_;
  active_ = staged_;
  staged_ = old_active;
  has_staged_ = false;
}

bool ConfigStore::loadActive(uint8_t* out_buf, size_t out_buf_cap, size_t* out_len) const {
  Preferences prefs;
  if (!prefs.begin(namespaceFor(active_), /*readOnly=*/true)) return false;

  const size_t len = prefs.getBytesLength(kBlobKey);
  if (len == 0 || len > out_buf_cap) {
    prefs.end();
    return false;
  }

  prefs.getBytes(kBlobKey, out_buf, len);
  *out_len = len;
  prefs.end();
  return true;
}

}  // namespace lorahome
