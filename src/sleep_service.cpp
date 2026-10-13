#include "sleep_service.h"

#include <powrprof.h>
#include <reason.h>

namespace mouneru {

namespace {
PowerResult RequestSystemShutdown() noexcept {
  HANDLE token{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    return {false, GetLastError()};
  TOKEN_PRIVILEGES privilege{}, previous{};
  privilege.PrivilegeCount = 1;
  privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privilege.Privileges[0].Luid)) {
    const DWORD error = GetLastError();
    CloseHandle(token);
    return {false, error};
  }
  DWORD previousSize = sizeof(previous);
  SetLastError(ERROR_SUCCESS);
  const BOOL adjusted = AdjustTokenPrivileges(token, FALSE, &privilege, sizeof(previous),
                                              &previous, &previousSize);
  const DWORD privilegeError = GetLastError();
  if (!adjusted || privilegeError != ERROR_SUCCESS) {
    CloseHandle(token);
    return {false, privilegeError == ERROR_SUCCESS ? ERROR_PRIVILEGE_NOT_HELD : privilegeError};
  }
  // Respect applications with unsaved work; never force them to close.
  const BOOL accepted = ExitWindowsEx(EWX_POWEROFF,
      SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
  const DWORD error = accepted ? ERROR_SUCCESS : GetLastError();
  (void)AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr);
  CloseHandle(token);
  return {accepted != FALSE, error == ERROR_SUCCESS && !accepted ? ERROR_GEN_FAILURE : error};
}
}  // namespace

PowerResult RequestSystemPower(PowerAction action) noexcept {
  if (action == PowerAction::Shutdown) return RequestSystemShutdown();
  SetLastError(ERROR_SUCCESS);
  if (SetSuspendState(FALSE, FALSE, FALSE) != FALSE) {
    return {true, ERROR_SUCCESS};
  }
  const DWORD error = GetLastError();
  return {false, error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error};
}

}  // namespace mouneru
