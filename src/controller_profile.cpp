// SPDX-License-Identifier: Apache-2.0

#include "ayn/controller_profile.h"

#include <utility>

namespace ayn::rsinput {

namespace {

bool ParseProfile(const std::string& value, ControllerProfile* profile) {
  if (profile == nullptr) {
    return false;
  }
  if (value == "0") {
    *profile = ControllerProfile::kStandard;
    return true;
  }
  if (value == "1") {
    *profile = ControllerProfile::kFlippedFace;
    return true;
  }
  return false;
}

const char* SerializeProfile(ControllerProfile profile) {
  return profile == ControllerProfile::kFlippedFace ? "1" : "0";
}

}  // namespace

bool IsSupportedControllerDevice(const std::string& product_device) {
  return product_device == "odin2_mini";
}

ControllerProfileService::ControllerProfileService(std::string product_device,
                                                   ProfileStore store)
    : product_device_(std::move(product_device)), store_(store) {}

ControllerProfileResponse ControllerProfileService::ResponseLocked(
    ControllerProfileResult result, ControllerProfile requested) const {
  return {result, requested, active_profile_};
}

ControllerProfileResponse ControllerProfileService::Initialize() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsSupportedControllerDevice(product_device_)) {
    return ResponseLocked(ControllerProfileResult::kUnsupportedDevice,
                          ControllerProfile::kStandard);
  }
  if (initialized_) {
    return ResponseLocked(ControllerProfileResult::kOk, active_profile_);
  }

  std::string persisted;
  if (store_.read == nullptr || !store_.read(store_.context, &persisted)) {
    active_profile_ = ControllerProfile::kStandard;
    return ResponseLocked(ControllerProfileResult::kStoreReadFailed,
                          ControllerProfile::kStandard);
  }

  ControllerProfile restored = ControllerProfile::kStandard;
  if (ParseProfile(persisted, &restored)) {
    active_profile_ = restored;
  } else {
    active_profile_ = ControllerProfile::kStandard;
  }
  initialized_ = true;
  return ResponseLocked(ControllerProfileResult::kOk, active_profile_);
}

ControllerProfileResponse ControllerProfileService::GetProfile() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsSupportedControllerDevice(product_device_)) {
    return ResponseLocked(ControllerProfileResult::kUnsupportedDevice,
                          ControllerProfile::kStandard);
  }
  if (!initialized_) {
    return ResponseLocked(ControllerProfileResult::kNotInitialized,
                          ControllerProfile::kStandard);
  }
  return ResponseLocked(ControllerProfileResult::kOk, active_profile_);
}

ControllerProfileResponse ControllerProfileService::SetProfile(
    ControllerProfile profile) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsSupportedControllerDevice(product_device_)) {
    return ResponseLocked(ControllerProfileResult::kUnsupportedDevice, profile);
  }
  if (!initialized_) {
    return ResponseLocked(ControllerProfileResult::kNotInitialized, profile);
  }
  if (!IsValidControllerProfile(profile)) {
    return ResponseLocked(ControllerProfileResult::kInvalidProfile, profile);
  }
  if (profile == active_profile_) {
    return ResponseLocked(ControllerProfileResult::kOk, profile);
  }
  if (pressed_buttons_ != 0) {
    return ResponseLocked(ControllerProfileResult::kBusy, profile);
  }
  if (store_.write == nullptr ||
      !store_.write(store_.context, SerializeProfile(profile))) {
    return ResponseLocked(ControllerProfileResult::kStoreWriteFailed, profile);
  }
  active_profile_ = profile;
  return ResponseLocked(ControllerProfileResult::kOk, profile);
}

std::array<InputEvent, kStatusEventCount>
ControllerProfileService::MapStatusToEvents(const Status& status) {
  std::lock_guard<std::mutex> lock(mutex_);
  pressed_buttons_ = status.buttons;
  return ayn::rsinput::MapStatusToEvents(status, active_profile_);
}

}  // namespace ayn::rsinput
