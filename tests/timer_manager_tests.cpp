#include "timer_manager.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
using namespace std::chrono;
constexpr auto kSteadyStart = steady_clock::time_point{};

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
  expect(timer.StartImmediate(now, kSteadyStart), "immediate countdown starts");
  expect(timer.state() == mouneru::TimerState::ImmediateCountdown,
         "state is immediate countdown");
  expect(timer.Tick(now + 4s, kSteadyStart + 4s) == mouneru::TimerAction::None,
         "does not fire before five seconds");
  expect(timer.Tick(now + 5s, kSteadyStart + 5s) == mouneru::TimerAction::RequestSleep,
         "fires at five seconds");
  expect(timer.Tick(now + 6s, kSteadyStart + 6s) == mouneru::TimerAction::None,
         "does not fire twice");
}

void test_cancel_prevents_immediate_sleep() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.StartImmediate(now, kSteadyStart), "countdown starts before cancellation");
  timer.Cancel();
  expect(timer.Tick(now + 10s, kSteadyStart + 10s) == mouneru::TimerAction::None,
         "cancel prevents sleep");
  expect(timer.state() == mouneru::TimerState::Idle, "cancel returns to idle");
}

void test_delay_validation_and_execution() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(!timer.ScheduleAfterMinutes(0, now, kSteadyStart), "zero minutes is rejected");
  expect(!timer.ScheduleAfterMinutes(1441, now, kSteadyStart), "more than one day is rejected");
  expect(timer.ScheduleAfterMinutes(1440, now, kSteadyStart), "one full day is accepted");
  timer.Cancel();
  expect(timer.ScheduleAfterMinutes(15, now, kSteadyStart), "fifteen minutes is accepted");
  expect(timer.target() == now + 15min, "delay target is exact");
  expect(timer.Tick(now + 14min + 59s, kSteadyStart + 14min + 59s) == mouneru::TimerAction::None,
         "delay does not fire early");
  expect(timer.Tick(now + 15min, kSteadyStart + 15min) == mouneru::TimerAction::RequestSleep,
         "delay fires on time");
}

void test_scheduled_timer_can_be_cancelled() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.ScheduleAfterMinutes(30, now, kSteadyStart), "schedule starts before cancellation");
  timer.Cancel();
  expect(timer.Tick(now + 1h, kSteadyStart + 1h) == mouneru::TimerAction::None,
         "cancelled schedule never fires");
}

void test_late_tick_fires_immediately() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.ScheduleAfterMinutes(60, now, kSteadyStart), "late schedule starts");
  expect(timer.Tick(now + 2h, kSteadyStart + 2h) == mouneru::TimerAction::RequestSleep,
         "overdue schedule fires on next tick");
}

void test_new_schedule_replaces_old_target() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.ScheduleAfterMinutes(15, now, kSteadyStart), "initial schedule starts");
  expect(timer.ScheduleAfterMinutes(60, now, kSteadyStart), "replacement schedule starts");
  expect(timer.Tick(now + 15min, kSteadyStart + 15min) == mouneru::TimerAction::None,
         "old target is replaced");
  expect(timer.Tick(now + 60min, kSteadyStart + 60min) == mouneru::TimerAction::RequestSleep,
         "replacement target fires");
}

void test_wall_clock_jump_does_not_expire_relative_timer() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.ScheduleAfterMinutes(30, now, kSteadyStart), "relative timer starts before clock correction");
  expect(timer.Tick(now + 1h, kSteadyStart + 1s) == mouneru::TimerAction::None,
         "advancing only the wall clock cannot expire a relative timer");
  expect(timer.Remaining(now + 1h, kSteadyStart + 1s) == 29min + 59s,
         "relative countdown ignores the forward correction");
  expect(timer.Tick(now - 1h, kSteadyStart + 30min) == mouneru::TimerAction::RequestSleep,
         "a backward wall-clock correction cannot delay the elapsed deadline");
}

void test_immediate_countdown_ignores_wall_clock_changes() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.StartImmediate(now, kSteadyStart), "five-second timer starts before clock changes");
  expect(timer.Tick(now + 1h, kSteadyStart + 4s) == mouneru::TimerAction::None,
         "forward clock jump cannot shorten five-second cancellation time");
  expect(timer.Remaining(now - 1h, kSteadyStart + 4s) == 1s,
         "five-second countdown display uses elapsed time after backward correction");
  expect(timer.Tick(now - 1h, kSteadyStart + 5s) == mouneru::TimerAction::RequestSleep,
         "backward clock jump cannot extend five-second countdown");
  expect(timer.Remaining(now, kSteadyStart + 5s) == 0s,
         "executing reservation has no remaining countdown");
}

void test_absolute_clock_schedule_stays_absolute() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.ScheduleAt(now + 30min, now), "absolute clock reservation starts");
  expect(timer.Tick(now + 1min, kSteadyStart + 1h) == mouneru::TimerAction::None,
         "elapsed time alone cannot expire an absolute clock reservation");
  expect(timer.Remaining(now + 1min, kSteadyStart + 1h) == 29min,
         "absolute countdown follows the wall clock");
  expect(timer.Tick(now + 30min, kSteadyStart + 1s) == mouneru::TimerAction::RequestSleep,
         "absolute reservation fires at the selected wall-clock time");
}

void test_deadline_mode_is_replaced_and_cleared() {
  mouneru::TimerManager timer;
  const auto now = sys_days{year{2026}/9/2} + 12h;
  expect(timer.StartImmediate(now, kSteadyStart), "relative countdown starts before replacement");
  expect(timer.ScheduleAt(now + 30min, now), "absolute reservation replaces relative deadline");
  expect(timer.Tick(now + 1s, kSteadyStart + 10s) == mouneru::TimerAction::None,
         "replaced five-second deadline no longer fires");
  expect(timer.ScheduleAfterMinutes(1, now, kSteadyStart), "relative reservation replaces absolute deadline");
  expect(timer.Tick(now + 1h, kSteadyStart + 59s) == mouneru::TimerAction::None,
         "replaced absolute deadline no longer fires");
  timer.Cancel();
  expect(timer.target() == mouneru::TimerManager::TimePoint{} &&
         timer.Remaining(now, kSteadyStart) == 0s &&
         timer.Tick(now + 1h, kSteadyStart + 1h) == mouneru::TimerAction::None,
         "cancel clears both clocks and leaves no executable reservation");
  expect(timer.StartImmediate(now, kSteadyStart), "fresh countdown can start after cancellation");
  expect(timer.Tick(now, kSteadyStart + 5s) == mouneru::TimerAction::RequestSleep,
         "fresh countdown fires after five elapsed seconds");
  expect(!timer.ScheduleAfterMinutes(1, now, kSteadyStart) &&
         !timer.ScheduleAt(now + 1min, now) && !timer.StartImmediate(now, kSteadyStart),
         "executing reservation cannot be replaced");
  timer.Cancel();
  expect(timer.state() == mouneru::TimerState::Executing, "cancel cannot undo an executing request");
  timer.CompleteExecution();
  expect(timer.state() == mouneru::TimerState::Idle &&
         timer.target() == mouneru::TimerManager::TimePoint{} &&
         timer.Remaining(now, kSteadyStart) == 0s &&
         timer.Tick(now + 1h, kSteadyStart + 1h) == mouneru::TimerAction::None,
         "completion clears both clocks and prevents later retries");
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
  test_wall_clock_jump_does_not_expire_relative_timer();
  test_immediate_countdown_ignores_wall_clock_changes();
  test_absolute_clock_schedule_stays_absolute();
  test_deadline_mode_is_replaced_and_cleared();
  test_past_clock_time_targets_next_day();
  test_future_clock_time_targets_today();
  if (failures == 0) {
    std::cout << "All timer tests passed.\n";
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
