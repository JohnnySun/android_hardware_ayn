// SPDX-License-Identifier: Apache-2.0

#include "ayn/controller_profile.h"

#include <atomic>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression);
  }
}

using ayn::rsinput::ControllerProfile;
using ayn::rsinput::ControllerProfileResult;
using ayn::rsinput::ControllerProfileService;
using ayn::rsinput::InputEvent;
using ayn::rsinput::ProfileStore;
using ayn::rsinput::Status;

struct StoreHarness {
  std::string value = "0";
  bool read_ok = true;
  bool write_ok = true;
  size_t writes = 0;
};

bool ReadStore(void* context, std::string* value) {
  auto* store = static_cast<StoreHarness*>(context);
  if (!store->read_ok || value == nullptr) return false;
  *value = store->value;
  return true;
}

bool WriteStore(void* context, const std::string& value) {
  auto* store = static_cast<StoreHarness*>(context);
  ++store->writes;
  if (!store->write_ok) return false;
  store->value = value;
  return true;
}

ControllerProfileService Service(const std::string& device,
                                 StoreHarness* store) {
  return ControllerProfileService(
      device, ProfileStore{ReadStore, WriteStore, store});
}

void ProfileMappingOnlyFlipsFacePairs() {
  StoreHarness store;
  ControllerProfileService service = Service("odin2_mini", &store);
  CHECK(service.Initialize().result == ControllerProfileResult::kOk);
  Status status;
  status.buttons = 0x00f0;
  const auto standard = service.MapStatusToEvents(status);
  CHECK((standard[4] == InputEvent{ayn::rsinput::kEventTypeKey,
                                   ayn::rsinput::kBtnWest, 1}));
  CHECK((standard[5] == InputEvent{ayn::rsinput::kEventTypeKey,
                                   ayn::rsinput::kBtnNorth, 1}));
  CHECK((standard[6] == InputEvent{ayn::rsinput::kEventTypeKey,
                                   ayn::rsinput::kBtnEast, 1}));
  CHECK((standard[7] == InputEvent{ayn::rsinput::kEventTypeKey,
                                   ayn::rsinput::kBtnSouth, 1}));
  service.MapStatusToEvents(Status{});
  CHECK(service.SetProfile(ControllerProfile::kFlippedFace).result ==
        ControllerProfileResult::kOk);
  const auto flipped = service.MapStatusToEvents(status);
  CHECK((flipped[4] == InputEvent{ayn::rsinput::kEventTypeKey,
                                  ayn::rsinput::kBtnNorth, 1}));
  CHECK((flipped[5] == InputEvent{ayn::rsinput::kEventTypeKey,
                                  ayn::rsinput::kBtnWest, 1}));
  CHECK((flipped[6] == InputEvent{ayn::rsinput::kEventTypeKey,
                                  ayn::rsinput::kBtnSouth, 1}));
  CHECK((flipped[7] == InputEvent{ayn::rsinput::kEventTypeKey,
                                  ayn::rsinput::kBtnEast, 1}));
  CHECK(flipped[0] == standard[0]);
  CHECK(flipped[16] == standard[16]);
  CHECK(flipped[22] == standard[22]);
}

void InvalidProfileIsRejectedWithoutPersistence() {
  StoreHarness store;
  ControllerProfileService service = Service("odin2_mini", &store);
  CHECK(service.Initialize().result == ControllerProfileResult::kOk);
  const auto response = service.SetProfile(static_cast<ControllerProfile>(99));
  CHECK(response.result == ControllerProfileResult::kInvalidProfile);
  CHECK(response.active_profile == ControllerProfile::kStandard);
  CHECK(store.writes == 0);
}

void HeldButtonsMakeProfileSwitchBusy() {
  StoreHarness store;
  ControllerProfileService service = Service("odin2_mini", &store);
  CHECK(service.Initialize().result == ControllerProfileResult::kOk);
  Status held;
  held.buttons = 1;
  service.MapStatusToEvents(held);
  const auto response = service.SetProfile(ControllerProfile::kFlippedFace);
  CHECK(response.result == ControllerProfileResult::kBusy);
  CHECK(response.active_profile == ControllerProfile::kStandard);
  CHECK(store.writes == 0);
}

void FailedPersistenceDoesNotChangeLiveProfile() {
  StoreHarness store;
  store.write_ok = false;
  ControllerProfileService service = Service("odin2_mini", &store);
  CHECK(service.Initialize().result == ControllerProfileResult::kOk);
  const auto response = service.SetProfile(ControllerProfile::kFlippedFace);
  CHECK(response.result == ControllerProfileResult::kStoreWriteFailed);
  CHECK(response.active_profile == ControllerProfile::kStandard);
  CHECK(service.GetProfile().active_profile == ControllerProfile::kStandard);
  CHECK(store.writes == 1);
}

void UnsupportedDeviceCannotReadOrWriteProfileState() {
  StoreHarness store;
  ControllerProfileService service = Service("kalama", &store);
  CHECK(service.Initialize().result ==
        ControllerProfileResult::kUnsupportedDevice);
  CHECK(service.GetProfile().result ==
        ControllerProfileResult::kUnsupportedDevice);
  CHECK(service.SetProfile(ControllerProfile::kFlippedFace).result ==
        ControllerProfileResult::kUnsupportedDevice);
  CHECK(store.writes == 0);
}

void InvalidStartupProfileFallsBackToStandard() {
  StoreHarness store;
  store.value = "invalid";
  ControllerProfileService service = Service("odin2_mini", &store);
  const auto response = service.Initialize();
  CHECK(response.result == ControllerProfileResult::kOk);
  CHECK(response.active_profile == ControllerProfile::kStandard);
  CHECK(store.writes == 0);
}

void MappingAndProfileSwitchingAreThreadSafe() {
  StoreHarness store;
  ControllerProfileService service = Service("odin2_mini", &store);
  CHECK(service.Initialize().result == ControllerProfileResult::kOk);
  std::atomic<bool> mapper_ok{true};
  std::thread mapper([&service, &mapper_ok]() {
    for (size_t iteration = 0; iteration < 1000; ++iteration) {
      const auto events = service.MapStatusToEvents(Status{});
      const bool standard = events[4].code == ayn::rsinput::kBtnWest &&
                            events[5].code == ayn::rsinput::kBtnNorth &&
                            events[6].code == ayn::rsinput::kBtnEast &&
                            events[7].code == ayn::rsinput::kBtnSouth;
      const bool flipped = events[4].code == ayn::rsinput::kBtnNorth &&
                           events[5].code == ayn::rsinput::kBtnWest &&
                           events[6].code == ayn::rsinput::kBtnSouth &&
                           events[7].code == ayn::rsinput::kBtnEast;
      if (!standard && !flipped) mapper_ok.store(false);
    }
  });
  for (size_t iteration = 0; iteration < 1000; ++iteration) {
    const ControllerProfile requested = iteration % 2 == 0
                                            ? ControllerProfile::kFlippedFace
                                            : ControllerProfile::kStandard;
    CHECK(service.SetProfile(requested).result ==
          ControllerProfileResult::kOk);
  }
  mapper.join();
  CHECK(mapper_ok.load());
}

}  // namespace

int main() {
  try {
    ProfileMappingOnlyFlipsFacePairs();
    InvalidProfileIsRejectedWithoutPersistence();
    HeldButtonsMakeProfileSwitchBusy();
    FailedPersistenceDoesNotChangeLiveProfile();
    UnsupportedDeviceCannotReadOrWriteProfileState();
    InvalidStartupProfileFallsBackToStandard();
    MappingAndProfileSwitchingAreThreadSafe();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "controller_profile_test: PASS\n";
  return 0;
}
