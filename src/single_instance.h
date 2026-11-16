#pragma once

#include <windows.h>

namespace mouneru {

enum class InstanceState { Acquired, AlreadyRunning, Failed };

// Retain the named object for the application lifetime without taking mutex ownership.
class SingleInstance final {
 public:
  explicit SingleInstance(const wchar_t* name = L"Local\\MouNeru.SingleInstance") noexcept;
  ~SingleInstance();
  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;
  SingleInstance(SingleInstance&&) = delete;
  SingleInstance& operator=(SingleInstance&&) = delete;

  [[nodiscard]] InstanceState state() const noexcept { return state_; }
  [[nodiscard]] DWORD error_code() const noexcept { return errorCode_; }

 private:
  HANDLE mutex_{};
  InstanceState state_{InstanceState::Failed};
  DWORD errorCode_{ERROR_SUCCESS};
};

}  // namespace mouneru
