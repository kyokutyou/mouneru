#include "timer_manager.h"

namespace mouneru {

using namespace std::chrono;

bool TimerManager::StartImmediate(TimePoint now, SteadyTimePoint steadyNow) noexcept {
  if (state_ == TimerState::ImmediateCountdown ||
      state_ == TimerState::Executing) {
    return false;
  }
  target_ = now + seconds{5};
  steadyTarget_ = steadyNow + seconds{5};
  relative_ = true;
  state_ = TimerState::ImmediateCountdown;
  return true;
}

bool TimerManager::ScheduleAfterMinutes(int minutes, TimePoint now,
                                       SteadyTimePoint steadyNow) noexcept {
  if (minutes < 1 || minutes > 1440 || state_ == TimerState::Executing) {
    return false;
  }
  target_ = now + std::chrono::minutes{minutes};
  steadyTarget_ = steadyNow + std::chrono::minutes{minutes};
  relative_ = true;
  state_ = TimerState::Scheduled;
  return true;
}

bool TimerManager::ScheduleAt(TimePoint target, TimePoint now) noexcept {
  if (target <= now || state_ == TimerState::Executing) {
    return false;
  }
  target_ = target;
  steadyTarget_ = {};
  relative_ = false;
  state_ = TimerState::Scheduled;
  return true;
}

void TimerManager::Cancel() noexcept {
  if (state_ != TimerState::Executing) {
    CompleteExecution();
  }
}

TimerAction TimerManager::Tick(TimePoint now, SteadyTimePoint steadyNow) noexcept {
  if (HasActiveTimer() && (relative_ ? steadyNow >= steadyTarget_ : now >= target_)) {
    state_ = TimerState::Executing;
    return TimerAction::RequestSleep;
  }
  return TimerAction::None;
}

void TimerManager::CompleteExecution() noexcept {
  state_ = TimerState::Idle;
  target_ = {};
  steadyTarget_ = {};
  relative_ = false;
}

system_clock::duration TimerManager::Remaining(TimePoint now,
                                              SteadyTimePoint steadyNow) const noexcept {
  if (!HasActiveTimer()) return system_clock::duration::zero();
  const auto remaining = relative_
      ? duration_cast<system_clock::duration>(steadyTarget_ - steadyNow)
      : target_ - now;
  return remaining > system_clock::duration::zero() ? remaining : system_clock::duration::zero();
}

bool TimerManager::HasActiveTimer() const noexcept {
  return state_ == TimerState::Scheduled ||
         state_ == TimerState::ImmediateCountdown;
}

TimerManager::LocalTime TimerManager::NextLocalClockTime(int hour, int minute,
                                                        LocalTime now) noexcept {
  const auto today = floor<days>(now);
  auto candidate = today + hours{hour} + minutes{minute};
  if (candidate <= now) {
    candidate += days{1};
  }
  return candidate;
}

}  // namespace mouneru
