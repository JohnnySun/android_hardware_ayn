// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_lifecycle.h"

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
      callbacks.monotonic_ms == nullptr || response_timeout_ms == 0) {
    failure = HandshakeFailure::kInvalidConfiguration;
    handshake.Fail();
    return finish(StartupResult::kFailed);
  }
  if (callbacks.stop_requested(callbacks.context)) {
    return finish(StartupResult::kStopped);
  }

  const auto frames = BuildQ9HandshakeFrames();
  handshake.Start();
  if (!callbacks.write_frame(callbacks.context, frames[0].data(),
                             frames[0].size())) {
    failure = HandshakeFailure::kWrite;
    handshake.Fail();
    return finish(StartupResult::kFailed);
  }

  uint64_t phase_started_ms = callbacks.monotonic_ms(callbacks.context);
  std::array<uint8_t, 256> buffer{};
  while (true) {
    if (callbacks.stop_requested(callbacks.context)) {
      return finish(StartupResult::kStopped);
    }

    if (handshake.state() == HandshakeState::kSendConfiguration) {
      for (size_t index = 1; index < frames.size(); ++index) {
        if (callbacks.stop_requested(callbacks.context)) {
          return finish(StartupResult::kStopped);
        }
        if (!callbacks.write_frame(callbacks.context, frames[index].data(),
                                   frames[index].size())) {
          failure = HandshakeFailure::kWrite;
          handshake.Fail();
          return finish(StartupResult::kFailed);
        }
      }
      handshake.ConfigurationSent();
      phase_started_ms = callbacks.monotonic_ms(callbacks.context);
      if (handshake.state() == HandshakeState::kInitialized) {
        return finish(StartupResult::kCompleted);
      }
    }
    if (handshake.state() == HandshakeState::kInitialized) {
      return finish(StartupResult::kCompleted);
    }
    if (handshake.state() == HandshakeState::kFailed) {
      failure = HandshakeFailure::kProtocol;
      return finish(StartupResult::kFailed);
    }

    const uint64_t now_ms = callbacks.monotonic_ms(callbacks.context);
    const uint64_t elapsed_ms = now_ms - phase_started_ms;
    if (elapsed_ms >= response_timeout_ms) {
      failure = HandshakeFailure::kTimeout;
      handshake.Fail();
      return finish(StartupResult::kFailed);
    }
    const uint64_t remaining_ms = response_timeout_ms - elapsed_ms;
    const uint32_t read_timeout_ms = static_cast<uint32_t>(std::min<uint64_t>(
        remaining_ms, std::numeric_limits<uint32_t>::max()));
    size_t received_size = 0;
    const HandshakeReadResult read_result = callbacks.read_bytes(
        callbacks.context, buffer.data(), buffer.size(), &received_size,
        read_timeout_ms);
    if (callbacks.stop_requested(callbacks.context)) {
      return finish(StartupResult::kStopped);
    }
    if (read_result != HandshakeReadResult::kData || received_size == 0 ||
        received_size > buffer.size()) {
      failure = read_result == HandshakeReadResult::kTimeout
                    ? HandshakeFailure::kTimeout
                    : HandshakeFailure::kRead;
      handshake.Fail();
      return finish(StartupResult::kFailed);
    }
    handshake.Feed(buffer.data(), received_size);
  }
}

StartupResult RunReconnectLoop(const LifecycleCallbacks& callbacks,
                               RetryPolicy retry_policy) {
  if (callbacks.stop_requested == nullptr || callbacks.power_on == nullptr ||
      callbacks.power_off == nullptr || callbacks.open_uart == nullptr ||
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
  // A successful power-on is owned across reconnect attempts and released once
  // when the daemon loop exits; failed power-on attempts never earn ownership.
  bool power_owned = false;
  const auto finish = [&callbacks, &power_owned](StartupResult result) {
    if (power_owned) {
      power_owned = false;
      if (!callbacks.power_off(callbacks.context)) {
        return StartupResult::kFailed;
      }
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
    }

    int uart_fd = -1;
    StartupResult attempt_result = OpenUartUnlessStopped(
        callbacks.stop_requested, callbacks.context, callbacks.open_uart,
        callbacks.context, &uart_fd);

    int uinput_fd = -1;
    if (attempt_result == StartupResult::kCompleted) {
      attempt_result = OpenUartUnlessStopped(
          callbacks.stop_requested, callbacks.context, callbacks.open_uinput,
          callbacks.context, &uinput_fd);
    }
    if (attempt_result == StartupResult::kCompleted) {
      attempt_result =
          callbacks.initialize_session(callbacks.context, uart_fd);
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
