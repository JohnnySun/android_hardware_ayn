// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>

#include <sys/types.h>

namespace ayn::performance {

struct PosixWriterOperations {
  void* context;
  int (*open_writer)(void* context, const char* path);
  ssize_t (*write_writer)(void* context, int fd, const void* data,
                          size_t size);
  int (*close_writer)(void* context, int fd);
  bool (*readback)(void* context, const std::string& path,
                   std::string* value);
};

bool ReadPosixFile(void* context, const std::string& path,
                   std::string* value);
bool WritePosixFile(void* context, const std::string& path,
                    const std::string& value);

}  // namespace ayn::performance
