// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

namespace ayn::rsinput {

// Who is listening for events the daemon sees before the framework does.
//
// Keyed on an opaque handle so the bookkeeping can be tested without binder.
// The daemon passes the AIBinder pointer, which is the identity binder itself
// uses, so a client that registers twice with the same object is one listener.
//
// Notification copies the list and releases the lock first. A listener call
// that reached back into the registry - unregistering from inside its own
// callback is the obvious way - would otherwise deadlock.
class ListenerRegistry {
 public:
  using Notifier = void (*)(void* context, void* listener);

  // False when the handle was already registered, so the caller can skip the
  // reference it would otherwise have taken.
  bool Add(void* listener);

  // False when the handle was not registered, which a death recipient racing
  // an explicit unregister will see.
  bool Remove(void* listener);

  size_t size() const;

  void NotifyAll(Notifier notify, void* context) const;

 private:
  mutable std::mutex mutex_;
  std::vector<void*> listeners_;
};

}  // namespace ayn::rsinput
