// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ayn/rsinput_mapping.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

namespace ayn::rsinput {

constexpr char kControllerProfileProperty[] =
    "persist.sys.ayn.controller.profile";

enum class ControllerProfileResult : int32_t {
  kOk = 0,
  kUnsupportedDevice = 1,
  kInvalidProfile = 2,
  kBusy = 3,
  kStoreReadFailed = 4,
  kStoreWriteFailed = 5,
  kNotInitialized = 6,
};

struct ControllerProfileResponse {
  ControllerProfileResult result = ControllerProfileResult::kNotInitialized;
  ControllerProfile requested_profile = ControllerProfile::kStandard;
  ControllerProfile active_profile = ControllerProfile::kStandard;
};

using ProfileStoreReader = bool (*)(void* context, std::string* value);
using ProfileStoreWriter = bool (*)(void* context, const std::string& value);

struct ProfileStore {
  ProfileStoreReader read;
  ProfileStoreWriter write;
  void* context;
};

bool IsSupportedControllerDevice(const std::string& product_device);

class ControllerProfileService {
 public:
  ControllerProfileService(std::string product_device, ProfileStore store);

  ControllerProfileResponse Initialize();
  ControllerProfileResponse GetProfile();
  ControllerProfileResponse SetProfile(ControllerProfile profile);
  std::array<InputEvent, kStatusEventCount> MapStatusToEvents(
      const Status& status);

 private:
  ControllerProfileResponse ResponseLocked(ControllerProfileResult result,
                                           ControllerProfile requested) const;

  const std::string product_device_;
  const ProfileStore store_;
  ControllerProfile active_profile_ = ControllerProfile::kStandard;
  uint16_t pressed_buttons_ = 0;
  bool initialized_ = false;
  std::mutex mutex_;
};

}  // namespace ayn::rsinput
