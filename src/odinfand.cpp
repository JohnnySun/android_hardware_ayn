// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_policy.h"

#include <android-base/logging.h>
#include <android-base/properties.h>

#include <cstdlib>
#include <string>

int main() {
  const std::string product_device =
      android::base::GetProperty("ro.product.device", "");
  if (!ayn::fan::IsSupportedDevice(product_device)) {
    LOG(ERROR) << "odinfand is disabled for this product device";
    return EXIT_FAILURE;
  }

  LOG(ERROR) << "odinfand has no product settings wiring; refusing sysfs I/O";
  return EXIT_FAILURE;
}
