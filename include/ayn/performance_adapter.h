// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace ayn::performance {

bool ReadPosixFile(void* context, const std::string& path,
                   std::string* value);

}  // namespace ayn::performance
