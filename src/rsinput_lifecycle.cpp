// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_lifecycle.h"

#include "ayn/rsinput_poll.h"
#include "ayn/rsinput_protocol.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace ayn::rsinput {

StartupResult OpenUartUnlessStopped(StopRequested stop_requested,
                                    void* stop_context,
                                    UartOpener open_uart,
                                    void* opener_context,
                                    int* uart_fd) {
  if (stop_requested(stop_context)) {
    return StartupResult::kStopped;
  }
  *uart_fd = open_uart(opener_context);
  if (*uart_fd < 0) {
    return StartupResult::kFailed;
  }
  return stop_requested(stop_context) ? StartupResult::kStopped
                                      : StartupResult::kCompleted;
}

bool WriteMcuPowerState(McuPowerWriter writer, void* context, bool enabled) {
  if (writer == nullptr) {
    return false;
  }
  constexpr char kControlPath[] = "/sys/devices/platform/rsgpio/driver_ctl";
  constexpr std::array<uint8_t, 10> kPowerOn = {
      'm', 'c', 'u', 'p', 'o', 'w', 'e', 'r', ' ', '1'};
  constexpr std::array<uint8_t, 10> kPowerOff = {
      'm', 'c', 'u', 'p', 'o', 'w', 'e', 'r', ' ', '0'};
  const auto& command = enabled ? kPowerOn : kPowerOff;
  return writer(context, kControlPath, command.data(), command.size());
}

StartupResult RunQ9Handshake(const HandshakeCallbacks& callbacks,
                             uint32_t response_timeout_ms,
                             uint32_t configuration_frame_delay_ms,
                             HandshakeDiagnostics* diagnostics) {
  Q9Handshake handshake;
  HandshakeFailure failure = HandshakeFailure::kNone;
  const auto finish = [&handshake, &failure,
                       diagnostics](StartupResult result) {
    if (diagnostics != nullptr) {
      diagnostics->state = handshake.state();
      diagnostics->stats = handshake.stats();
      diagnostics->expected_response_type =
          handshake.expected_response_type();
      diagnostics->failure = failure;
    }
    return result;
  };
  if (callbacks.stop_requested == nullptr ||
      callbacks.write_frame == nullptr || callbacks.read_bytes == nullptr ||
      callbacks.monotonic_ms == nullptr || response_timeout_ms == 0 ||
      configuration_frame_delay_ms == 0) {
    failure = HandshakeFailure::kInvalidConfiguration;
    handshake.Fail();
    return finish(StartupResult::kFailed);
  }
  if (callbacks.stop_requested(callbacks.context)) {
    return finish(StartupResult::kStopped);
  }

  const auto frames = BuildQ9HandshakeFrames();
  StartupPollRxDetector poll_detector;
  handshake.Start();
  if (!callbacks.write_frame(callbacks.context, frames[0].data(),
                             frames[0].size())) {
    failure = HandshakeFailure::kWrite;
    handshake.Fail();
    return finish(StartupResult::kFailed);
  }

  bool version_sent = false;
  size_t next_configuration_frame = 2;
  uint64_t next_tx_ms =
      callbacks.monotonic_ms(callbacks.context) +
      configuration_frame_delay_ms;
  uint64_t response_deadline_ms = std::numeric_limits<uint64_t>::max();
  std::array<uint8_t, 256> buffer{};
  while (true) {
    if (callbacks.stop_requested(callbacks.context)) {
      return finish(StartupResult::kStopped);
    }

    if (handshake.state() == HandshakeState::kInitialized) {
      return finish(StartupResult::kCompleted);
    }
    if (handshake.state() == HandshakeState::kFailed) {
      failure = HandshakeFailure::kProtocol;
      return finish(StartupResult::kFailed);
    }

    const uint64_t now_ms = callbacks.monotonic_ms(callbacks.context);
    if (response_deadline_ms != std::numeric_limits<uint64_t>::max() &&
        now_ms >= response_deadline_ms) {
      failure = HandshakeFailure::kTimeout;
      handshake.Fail();
      return finish(StartupResult::kFailed);
    }

    if (now_ms >= next_tx_ms) {
      if (!poll_detector.poll_stop_requested() &&
          !callbacks.write_frame(callbacks.context, frames[0].data(),
                                 frames[0].size())) {
        failure = HandshakeFailure::kWrite;
        handshake.Fail();
        return finish(StartupResult::kFailed);
      }

      if (!version_sent) {
        if (!callbacks.write_frame(callbacks.context, frames[1].data(),
                                   frames[1].size())) {
          failure = HandshakeFailure::kWrite;
          handshake.Fail();
          return finish(StartupResult::kFailed);
        }
        version_sent = true;
        response_deadline_ms = now_ms + response_timeout_ms;
      } else if (handshake.state() == HandshakeState::kSendConfiguration &&
                 next_configuration_frame < frames.size()) {
        const auto& frame = frames[next_configuration_frame++];
        if (!callbacks.write_frame(callbacks.context, frame.data(),
                                   frame.size())) {
          failure = HandshakeFailure::kWrite;
          handshake.Fail();
          return finish(StartupResult::kFailed);
        }
        if (next_configuration_frame == frames.size()) {
          handshake.ConfigurationSent();
          response_deadline_ms = now_ms + response_timeout_ms;
        }
      }
      next_tx_ms = now_ms + configuration_frame_delay_ms;
      continue;
    }

    uint64_t wake_at_ms = next_tx_ms;
    if (response_deadline_ms != std::numeric_limits<uint64_t>::max()) {
      wake_at_ms = std::min(wake_at_ms, response_deadline_ms);
    }
    const uint64_t wait_ms = wake_at_ms - now_ms;
    const uint32_t read_timeout_ms = static_cast<uint32_t>(std::min<uint64_t>(
        wait_ms, std::numeric_limits<uint32_t>::max()));
    size_t received_size = 0;
    const HandshakeReadResult read_result = callbacks.read_bytes(
        callbacks.context, buffer.data(), buffer.size(), &received_size,
        read_timeout_ms);
    if (callbacks.stop_requested(callbacks.context)) {
      return finish(StartupResult::kStopped);
    }
    if (response_deadline_ms != std::numeric_limits<uint64_t>::max() &&
        callbacks.monotonic_ms(callbacks.context) >= response_deadline_ms) {
      failure = HandshakeFailure::kTimeout;
      handshake.Fail();
      return finish(StartupResult::kFailed);
    }
    if (read_result == HandshakeReadResult::kTimeout) {
      continue;
    }
    if (read_result != HandshakeReadResult::kData || received_size == 0 ||
        received_size > buffer.size()) {
      failure = HandshakeFailure::kRead;
      handshake.Fail();
      return finish(StartupResult::kFailed);
    }

    poll_detector.Feed(buffer.data(), received_size);
    const uint64_t immediate_polls =
        poll_detector.consume_immediate_poll_requests();
    for (uint64_t index = 0; index < immediate_polls; ++index) {
      if (!callbacks.write_frame(callbacks.context, frames[0].data(),
                                 frames[0].size())) {
        failure = HandshakeFailure::kWrite;
        handshake.Fail();
        return finish(StartupResult::kFailed);
      }
    }
    if (version_sent) {
      handshake.Feed(buffer.data(), received_size);
      if (handshake.state() == HandshakeState::kSendConfiguration) {
        response_deadline_ms = std::numeric_limits<uint64_t>::max();
      }
    }
  }
}

StartupResult RunReconnectLoop(const LifecycleCallbacks& callbacks,
                               RetryPolicy retry_policy) {
  if (callbacks.stop_requested == nullptr || callbacks.power_on == nullptr ||
      callbacks.power_off == nullptr || callbacks.open_uart == nullptr ||
      callbacks.settle_after_power_on == nullptr ||
      callbacks.open_uinput == nullptr || callbacks.close_uart == nullptr ||
      callbacks.close_uinput == nullptr ||
      callbacks.initialize_session == nullptr ||
      callbacks.forward_session == nullptr ||
      callbacks.wait_before_retry == nullptr ||
      retry_policy.initial_delay_ms == 0 ||
      retry_policy.max_delay_ms < retry_policy.initial_delay_ms) {
    return StartupResult::kFailed;
  }

  uint32_t retry_delay_ms = retry_policy.initial_delay_ms;
  bool power_owned = false;
  const auto release_power = [&callbacks, &power_owned]() {
    if (!power_owned) {
      return true;
    }
    power_owned = false;
    return callbacks.power_off(callbacks.context);
  };
  const auto finish = [&release_power](StartupResult result) {
    if (!release_power()) {
      return StartupResult::kFailed;
    }
    return result;
  };
  while (!callbacks.stop_requested(callbacks.context)) {
    if (!power_owned) {
      if (!callbacks.power_on(callbacks.context)) {
        callbacks.wait_before_retry(callbacks.context, retry_delay_ms);
        if (callbacks.stop_requested(callbacks.context)) {
          return finish(StartupResult::kStopped);
        }
        retry_delay_ms = std::min(
            retry_policy.max_delay_ms,
            retry_delay_ms > retry_policy.max_delay_ms / 2
                ? retry_policy.max_delay_ms
                : retry_delay_ms * 2);
        continue;
      }
      power_owned = true;
      const StartupResult settle_result =
          callbacks.settle_after_power_on(callbacks.context);
      if (settle_result == StartupResult::kStopped ||
          callbacks.stop_requested(callbacks.context)) {
        return finish(StartupResult::kStopped);
      }
      if (settle_result == StartupResult::kFailed) {
        if (!release_power()) {
          return StartupResult::kFailed;
        }
        callbacks.wait_before_retry(callbacks.context, retry_delay_ms);
        if (callbacks.stop_requested(callbacks.context)) {
          return finish(StartupResult::kStopped);
        }
        retry_delay_ms = std::min(
            retry_policy.max_delay_ms,
            retry_delay_ms > retry_policy.max_delay_ms / 2
                ? retry_policy.max_delay_ms
                : retry_delay_ms * 2);
        continue;
      }
    }

    int uart_fd = -1;
    StartupResult attempt_result = OpenUartUnlessStopped(
        callbacks.stop_requested, callbacks.context, callbacks.open_uart,
        callbacks.context, &uart_fd);

    bool initialization_failed = false;
    if (attempt_result == StartupResult::kCompleted) {
      attempt_result =
          callbacks.initialize_session(callbacks.context, uart_fd);
      initialization_failed = attempt_result == StartupResult::kFailed;
    }
    int uinput_fd = -1;
    if (attempt_result == StartupResult::kCompleted) {
      attempt_result = OpenUartUnlessStopped(
          callbacks.stop_requested, callbacks.context, callbacks.open_uinput,
          callbacks.context, &uinput_fd);
    }
    if (attempt_result == StartupResult::kCompleted) {
      attempt_result = callbacks.forward_session(callbacks.context, uart_fd,
                                                  uinput_fd);
    }

    if (uinput_fd >= 0) {
      callbacks.close_uinput(callbacks.context, uinput_fd);
    }
    if (uart_fd >= 0) {
      callbacks.close_uart(callbacks.context, uart_fd);
    }

    if (attempt_result == StartupResult::kStopped ||
        callbacks.stop_requested(callbacks.context)) {
      return finish(StartupResult::kStopped);
    }

    if (initialization_failed && !release_power()) {
      return StartupResult::kFailed;
    }
    callbacks.wait_before_retry(callbacks.context, retry_delay_ms);
    if (callbacks.stop_requested(callbacks.context)) {
      return finish(StartupResult::kStopped);
    }
    retry_delay_ms = std::min(
        retry_policy.max_delay_ms,
        retry_delay_ms > retry_policy.max_delay_ms / 2
            ? retry_policy.max_delay_ms
            : retry_delay_ms * 2);
  }
  return finish(StartupResult::kStopped);
}

}  // namespace ayn::rsinput
