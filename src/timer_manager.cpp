#include "timer_manager.h"

namespace mouneru {

using namespace std::chrono;

bool TimerManager::StartImmediate(TimePoint now) noexcept {
  if (state_ == TimerState::ImmediateCountdown ||
      state_ == TimerState::Executing) {
    return false;
  }
  target_ = now + seconds{5};
  state_ = TimerState::ImmediateCountdown;
  return true;
}

bool TimerManager::ScheduleAfterMinutes(int minutes, TimePoint now) noexcept {
  if (minutes < 1 || minutes > 1440 || state_ == TimerState::Executing) {
    return false;
  }
  target_ = now + std::chrono::minutes{minutes};
  state_ = TimerState::Scheduled;
  return true;
}

bool TimerManager::ScheduleAt(TimePoint target, TimePoint now) noexcept {
  if (target <= now || state_ == TimerState::Executing) {
    return false;
  }
  target_ = target;
  state_ = TimerState::Scheduled;
  return true;
}

void TimerManager::Cancel() noexcept {
  if (state_ != TimerState::Executing) {
    state_ = TimerState::Idle;
    target_ = {};
  }
}

TimerAction TimerManager::Tick(TimePoint now) noexcept {
  if ((state_ == TimerState::Scheduled ||
       state_ == TimerState::ImmediateCountdown) &&
      now >= target_) {
    state_ = TimerState::Executing;
    return TimerAction::RequestSleep;
  }
  return TimerAction::None;
}

void TimerManager::CompleteExecution() noexcept {
  state_ = TimerState::Idle;
  target_ = {};
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
