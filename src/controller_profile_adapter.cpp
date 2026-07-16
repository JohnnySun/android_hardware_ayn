// SPDX-License-Identifier: Apache-2.0

#include "ayn/controller_profile_adapter.h"

#include <android-base/properties.h>

namespace ayn::rsinput {

namespace {

bool ReadPersistentProfile(void*, std::string* value) {
  if (value == nullptr) {
    return false;
  }
  *value = android::base::GetProperty(kControllerProfileProperty, "0");
  return true;
}

bool WritePersistentProfile(void*, const std::string& value) {
  return android::base::SetProperty(kControllerProfileProperty, value);
}

}  // namespace

ProfileStore ProductionControllerProfileStore() {
  return {ReadPersistentProfile, WritePersistentProfile, nullptr};
}

}  // namespace ayn::rsinput
