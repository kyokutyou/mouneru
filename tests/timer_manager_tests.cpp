#include "timer_manager.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
using namespace std::chrono;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}

void test_immediate_countdown_fires_once() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.StartImmediate(now), "immediate countdown starts");
  expect(timer.state() == mouneru::TimerState::ImmediateCountdown,
         "state is immediate countdown");
  expect(timer.Tick(now + 4s) == mouneru::TimerAction::None,
         "does not fire before five seconds");
  expect(timer.Tick(now + 5s) == mouneru::TimerAction::RequestSleep,
         "fires at five seconds");
  expect(timer.Tick(now + 6s) == mouneru::TimerAction::None,
         "does not fire twice");
}

void test_cancel_prevents_immediate_sleep() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.StartImmediate(now), "countdown starts before cancellation");
  timer.Cancel();
  expect(timer.Tick(now + 10s) == mouneru::TimerAction::None,
         "cancel prevents sleep");
  expect(timer.state() == mouneru::TimerState::Idle, "cancel returns to idle");
}

void test_delay_validation_and_execution() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(!timer.ScheduleAfterMinutes(0, now), "zero minutes is rejected");
  expect(!timer.ScheduleAfterMinutes(1441, now), "more than one day is rejected");
  expect(timer.ScheduleAfterMinutes(1440, now), "one full day is accepted");
  timer.Cancel();
  expect(timer.ScheduleAfterMinutes(15, now), "fifteen minutes is accepted");
  expect(timer.target() == now + 15min, "delay target is exact");
  expect(timer.Tick(now + 14min + 59s) == mouneru::TimerAction::None,
         "delay does not fire early");
  expect(timer.Tick(now + 15min) == mouneru::TimerAction::RequestSleep,
         "delay fires on time");
}

void test_scheduled_timer_can_be_cancelled() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.ScheduleAfterMinutes(30, now), "schedule starts before cancellation");
  timer.Cancel();
  expect(timer.Tick(now + 1h) == mouneru::TimerAction::None,
         "cancelled schedule never fires");
}

void test_late_tick_fires_immediately() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.ScheduleAfterMinutes(60, now), "late schedule starts");
  expect(timer.Tick(now + 2h) == mouneru::TimerAction::RequestSleep,
         "overdue schedule fires on next tick");
}

void test_new_schedule_replaces_old_target() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.ScheduleAfterMinutes(15, now), "initial schedule starts");
  expect(timer.ScheduleAfterMinutes(60, now), "replacement schedule starts");
  expect(timer.Tick(now + 15min) == mouneru::TimerAction::None,
         "old target is replaced");
  expect(timer.Tick(now + 60min) == mouneru::TimerAction::RequestSleep,
         "replacement target fires");
}

void test_past_clock_time_targets_next_day() {
  const auto now = local_days{year{2026}/9/2} + 23h;
  const auto target = mouneru::TimerManager::NextLocalClockTime(22, 30, now);
  expect(target == local_days{year{2026}/9/3} + 22h + 30min,
         "past time targets the next day");
}

void test_future_clock_time_targets_today() {
  const auto now = local_days{year{2026}/9/2} + 12h;
  const auto target = mouneru::TimerManager::NextLocalClockTime(22, 30, now);
  expect(target == local_days{year{2026}/9/2} + 22h + 30min,
         "future time targets today");
}
}  // namespace

int main() {
  test_immediate_countdown_fires_once();
  test_cancel_prevents_immediate_sleep();
  test_delay_validation_and_execution();
  test_scheduled_timer_can_be_cancelled();
  test_late_tick_fires_immediately();
  test_new_schedule_replaces_old_target();
  test_past_clock_time_targets_next_day();
  test_future_clock_time_targets_today();
  if (failures == 0) {
    std::cout << "All timer tests passed.\n";
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
