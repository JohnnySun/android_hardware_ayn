// SPDX-License-Identifier: Apache-2.0
#include "ayn/rsinput_listeners.h"

#include <algorithm>

namespace ayn::rsinput {

bool ListenerRegistry::Add(void* listener) {
  if (listener == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> guard(mutex_);
  if (std::find(listeners_.begin(), listeners_.end(), listener) != listeners_.end()) {
    return false;
  }
  listeners_.push_back(listener);
  return true;
}

bool ListenerRegistry::Remove(void* listener) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = std::find(listeners_.begin(), listeners_.end(), listener);
  if (found == listeners_.end()) {
    return false;
  }
  listeners_.erase(found);
  return true;
}

size_t ListenerRegistry::size() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return listeners_.size();
}

void ListenerRegistry::NotifyAll(Notifier notify, void* context) const {
  if (notify == nullptr) {
    return;
  }
  std::vector<void*> snapshot;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    snapshot = listeners_;
  }
  for (void* listener : snapshot) {
    notify(context, listener);
  }
}

}  // namespace ayn::rsinput
