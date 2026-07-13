// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_adapter.h"

#include <android-base/logging.h>
#include <android-base/properties.h>

#include <cstdlib>
#include <string>

namespace {

bool StopWasRequested(void*) {
  return false;
}

bool ReadUnwiredSettings(void*, ayn::fan::FanSettings*) {
  return false;
}

bool ReadUnwiredTemperature(void*, int*) {
  return false;
}

}  // namespace

int main() {
  const std::string product_device =
      android::base::GetProperty("ro.product.device", "");
  const ayn::fan::AdapterResult result =
      ayn::fan::ApplyCurrentSettingsUnlessStopped(
          product_device, ayn::fan::StockSysfsPaths(), StopWasRequested,
          nullptr, ReadUnwiredSettings, nullptr, ReadUnwiredTemperature,
          nullptr, ayn::fan::ReadPosixFile, nullptr, ayn::fan::WritePosixFile,
          nullptr);
  if (!ayn::fan::IsSupportedDevice(product_device)) {
    LOG(ERROR) << "odinfand is disabled for this product device";
  } else {
    LOG(ERROR) << "odinfand has no product settings wiring; refusing sysfs I/O";
  }
  return result == ayn::fan::AdapterResult::kApplied ? EXIT_SUCCESS
                                                     : EXIT_FAILURE;
}
