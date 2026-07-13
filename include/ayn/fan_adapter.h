// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ayn/fan_lifecycle.h"

#include <string>

namespace ayn::fan {

enum class AdapterResult {
  kApplied,
  kStopped,
  kFailedClosed,
  kDisableUnconfirmed,
};

using FanSettingsReader = bool (*)(void* context, FanSettings* settings);

bool ReadPosixFile(void* context, const std::string& path, std::string* value);
bool WritePosixFile(void* context, const std::string& path,
                    const std::string& value);

AdapterResult ApplyCurrentSettingsUnlessStopped(
    const std::string& product_device, const SysfsPaths& paths,
    StopRequested stop_requested, void* stop_context,
    FanSettingsReader read_settings, void* settings_context,
    TemperatureReader read_temperature, void* temperature_context,
    SysfsReader read_file, void* reader_context, SysfsWriter write_file,
    void* writer_context);

}  // namespace ayn::fan
