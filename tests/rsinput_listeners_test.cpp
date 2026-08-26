// SPDX-License-Identifier: Apache-2.0
#include "ayn/rsinput_listeners.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using ayn::rsinput::ListenerRegistry;

int g_failures = 0;

void Check(bool value, const std::string& what) {
  std::cout << (value ? "[PASS] " : "[FAIL] ") << what << "\n";
  if (!value) {
    ++g_failures;
  }
}

struct Collector {
  std::vector<void*> seen;
};

void Collect(void* context, void* listener) {
  static_cast<Collector*>(context)->seen.push_back(listener);
}

int a = 0;
int b = 0;

void RegisteringTwiceIsOneListener() {
  // The app rebinding after its own restart is the ordinary case, and a
  // duplicate would mean the chord opened the overlay twice.
  ListenerRegistry registry;
  Check(registry.Add(&a), "the first registration is new");
  Check(!registry.Add(&a), "the second with the same binder is not");
  Check(registry.size() == 1, "and only one listener is held");
}

void EachDistinctListenerIsKept() {
  ListenerRegistry registry;
  registry.Add(&a);
  registry.Add(&b);
  Check(registry.size() == 2, "two different listeners both register");

  Collector collector;
  registry.NotifyAll(Collect, &collector);
  Check(collector.seen.size() == 2, "and both are notified");
}

void RemovingSomethingUnknownIsAnswered() {
  // A death recipient racing an explicit unregister hits exactly this, and it
  // must not drop a reference twice.
  ListenerRegistry registry;
  registry.Add(&a);
  Check(registry.Remove(&a), "removing a registered listener succeeds");
  Check(!registry.Remove(&a), "removing it again says so");
  Check(registry.size() == 0, "and nothing is left");
}

void NullIsRefusedRatherThanStored() {
  ListenerRegistry registry;
  Check(!registry.Add(nullptr), "a null listener is not registered");
  Check(registry.size() == 0, "and nothing is stored");
}

void NotifyingNobodyIsHarmless() {
  ListenerRegistry registry;
  Collector collector;
  registry.NotifyAll(Collect, &collector);
  Check(collector.seen.empty(), "an empty registry notifies nobody");
  registry.Add(&a);
  registry.NotifyAll(nullptr, &collector);
  Check(collector.seen.empty(), "and a null notifier does nothing");
}

void UnregisteringFromInsideACallbackDoesNotDeadlock() {
  // NotifyAll copies and unlocks first precisely so this is safe. If it held
  // the lock across the callback this test would hang rather than fail.
  static ListenerRegistry registry;
  registry.Add(&a);
  registry.NotifyAll([](void*, void* listener) { registry.Remove(listener); }, nullptr);
  Check(registry.size() == 0, "a listener may unregister itself from its own callback");
}

}  // namespace

int main() {
  RegisteringTwiceIsOneListener();
  EachDistinctListenerIsKept();
  RemovingSomethingUnknownIsAnswered();
  NullIsRefusedRatherThanStored();
  NotifyingNobodyIsHarmless();
  UnregisteringFromInsideACallbackDoesNotDeadlock();
  if (g_failures != 0) {
    std::cout << g_failures << " failed\n";
    return 1;
  }
  std::cout << "rsinput listener registry tests passed\n";
  return 0;
}
