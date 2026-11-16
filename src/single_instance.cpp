#include "single_instance.h"

namespace mouneru {

SingleInstance::SingleInstance(const wchar_t* name) noexcept {
  if (!name || !*name) {
    errorCode_ = ERROR_INVALID_PARAMETER;
    return;
  }
  SetLastError(ERROR_SUCCESS);
  mutex_ = CreateMutexW(nullptr, FALSE, name);
  const DWORD error = GetLastError();
  if (!mutex_) {
    errorCode_ = error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
    return;
  }
  state_ = error == ERROR_ALREADY_EXISTS ? InstanceState::AlreadyRunning : InstanceState::Acquired;
}

SingleInstance::~SingleInstance() {
  if (mutex_) CloseHandle(mutex_);
}

}  // namespace mouneru
