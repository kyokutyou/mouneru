#pragma once

#include <chrono>

namespace mouneru {

enum class TimerState { Idle, Scheduled, ImmediateCountdown, Executing };
enum class TimerAction { None, RequestSleep };

class TimerManager final {
 public:
  using TimePoint = std::chrono::system_clock::time_point;
  using SteadyTimePoint = std::chrono::steady_clock::time_point;
  using LocalTime = std::chrono::local_seconds;

  [[nodiscard]] bool StartImmediate(
      TimePoint now, SteadyTimePoint steadyNow = std::chrono::steady_clock::now()) noexcept;
  [[nodiscard]] bool ScheduleAfterMinutes(
      int minutes, TimePoint now,
      SteadyTimePoint steadyNow = std::chrono::steady_clock::now()) noexcept;
  [[nodiscard]] bool ScheduleAt(TimePoint target, TimePoint now) noexcept;
  void Cancel() noexcept;
  [[nodiscard]] TimerAction Tick(
      TimePoint now, SteadyTimePoint steadyNow = std::chrono::steady_clock::now()) noexcept;
  void CompleteExecution() noexcept;

  [[nodiscard]] TimerState state() const noexcept { return state_; }
  // The wall-clock estimate captured when the reservation was created.
  [[nodiscard]] TimePoint target() const noexcept { return target_; }
  [[nodiscard]] std::chrono::system_clock::duration Remaining(
      TimePoint now, SteadyTimePoint steadyNow = std::chrono::steady_clock::now()) const noexcept;
  [[nodiscard]] bool HasActiveTimer() const noexcept;
  [[nodiscard]] static LocalTime NextLocalClockTime(int hour, int minute,
                                                    LocalTime now) noexcept;

 private:
  TimerState state_{TimerState::Idle};
  TimePoint target_{};
  SteadyTimePoint steadyTarget_{};
  bool relative_{};
};

}  // namespace mouneru
