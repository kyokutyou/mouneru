// Exercise the real service with every power and privilege API replaced below.
// This executable cannot suspend or shut down the PC or change token privileges.
#include <windows.h>
#include <powrprof.h>
#include <reason.h>

#include <iostream>
#include <string_view>

namespace {
int failures{};
void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}
}

namespace fake {
struct Scenario {
  BOOL openResult{TRUE}, lookupResult{TRUE}, adjustResult{TRUE};
  BOOL shutdownResult{TRUE}, sleepResult{TRUE};
  DWORD openError{ERROR_SUCCESS}, lookupError{ERROR_SUCCESS};
  DWORD adjustError{ERROR_SUCCESS}, shutdownError{ERROR_SUCCESS}, sleepError{ERROR_SUCCESS};
  DWORD lastError{};
  int opens{}, lookups{}, adjustments{}, shutdowns{}, sleeps{}, closes{};
  bool restored{};
} state;

HANDLE Token() { return reinterpret_cast<HANDLE>(static_cast<INT_PTR>(123)); }
DWORD WINAPI LastError() { return state.lastError; }
void WINAPI SetError(DWORD error) { state.lastError = error; }
BOOL WINAPI OpenToken(HANDLE, DWORD access, PHANDLE token) {
  ++state.opens;
  Expect(access == (TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY), "token has only required access");
  state.lastError = state.openError;
  if (state.openResult) *token = Token();
  return state.openResult;
}
BOOL WINAPI Lookup(LPCWSTR system, LPCWSTR name, PLUID luid) {
  ++state.lookups;
  Expect(system == nullptr && std::wstring_view(name) == SE_SHUTDOWN_NAME,
         "lookup requests local shutdown privilege");
  state.lastError = state.lookupError;
  if (state.lookupResult) *luid = {17, 23};
  return state.lookupResult;
}
BOOL WINAPI Adjust(HANDLE token, BOOL disableAll, PTOKEN_PRIVILEGES requested,
                   DWORD capacity, PTOKEN_PRIVILEGES previous, PDWORD returnedSize) {
  ++state.adjustments;
  Expect(token == Token() && disableAll == FALSE, "only the acquired token is adjusted");
  Expect(requested && requested->PrivilegeCount == 1 &&
         requested->Privileges[0].Luid.LowPart == 17 &&
         requested->Privileges[0].Luid.HighPart == 23, "adjustment addresses shutdown privilege");
  if (state.adjustments == 1) {
    Expect(requested->Privileges[0].Attributes == SE_PRIVILEGE_ENABLED,
           "shutdown privilege is enabled before requesting shutdown");
    Expect(previous && returnedSize && capacity >= sizeof(TOKEN_PRIVILEGES),
           "original privilege state is requested for restoration");
    if (previous && returnedSize && capacity >= sizeof(TOKEN_PRIVILEGES)) {
      *previous = *requested;
      previous->Privileges[0].Attributes = 0;
      *returnedSize = sizeof(TOKEN_PRIVILEGES);
    }
    state.lastError = state.adjustError;
    return state.adjustResult;
  }
  state.restored = requested->Privileges[0].Attributes == 0;
  Expect(previous == nullptr, "restoration does not replace saved privilege state");
  // Cleanup must not replace an error returned by the power request.
  state.lastError = ERROR_INVALID_DATA;
  return TRUE;
}
BOOL WINAPI Shutdown(UINT flags, DWORD reason) {
  ++state.shutdowns;
  Expect(state.adjustments == 1 && state.closes == 0,
         "shutdown is requested while acquired privilege is enabled");
  Expect(flags == EWX_POWEROFF, "shutdown never forces other applications to close");
  Expect(reason != 0 && (reason & SHTDN_REASON_FLAG_PLANNED) != 0,
         "shutdown supplies a nonzero planned reason");
  state.lastError = state.shutdownError;
  return state.shutdownResult;
}
BOOLEAN WINAPI Suspend(BOOLEAN hibernate, BOOLEAN force, BOOLEAN disableWake) {
  ++state.sleeps;
  Expect(hibernate == FALSE && force == FALSE && disableWake == FALSE,
         "sleep uses nonhibernating, nonforced suspend and preserves wake events");
  state.lastError = state.sleepError;
  return state.sleepResult != FALSE ? TRUE : FALSE;
}
BOOL WINAPI Close(HANDLE token) {
  ++state.closes;
  Expect(token == Token(), "only acquired fake token is closed");
  state.lastError = ERROR_INVALID_HANDLE;
  return TRUE;
}
}  // namespace fake

#define GetLastError fake::LastError
#define SetLastError fake::SetError
#define OpenProcessToken fake::OpenToken
#define LookupPrivilegeValueW fake::Lookup
#define AdjustTokenPrivileges fake::Adjust
#define ExitWindowsEx fake::Shutdown
#define SetSuspendState fake::Suspend
#define CloseHandle fake::Close
#include "../src/sleep_service.cpp"
#undef GetLastError
#undef SetLastError
#undef OpenProcessToken
#undef LookupPrivilegeValueW
#undef AdjustTokenPrivileges
#undef ExitWindowsEx
#undef SetSuspendState
#undef CloseHandle

namespace {
using mouneru::PowerAction;
void Reset() { fake::state = {}; }
void ExpectResult(mouneru::PowerResult result, bool success, DWORD error) {
  Expect(result.succeeded == success && result.errorCode == error,
         "public service returns expected result and error");
}
void ExpectShutdownCleanup() {
  Expect(fake::state.restored && fake::state.adjustments == 2 && fake::state.closes == 1,
         "shutdown restores original privilege state and releases token exactly once");
  Expect(fake::state.sleeps == 0, "shutdown never calls suspend");
}
void TestSleep() {
  for (DWORD error : {ERROR_SUCCESS, ERROR_ACCESS_DENIED}) {
    Reset();
    fake::state.sleepResult = FALSE;
    fake::state.sleepError = error;
    ExpectResult(mouneru::RequestSystemPower(PowerAction::Sleep), false,
                 error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
    Expect(fake::state.sleeps == 1 && fake::state.shutdowns == 0 && fake::state.opens == 0,
           "sleep does not enter shutdown or token handling");
  }
  Reset();
  ExpectResult(mouneru::RequestSystemPower(PowerAction::Sleep), true, ERROR_SUCCESS);
  Expect(fake::state.sleeps == 1 && fake::state.shutdowns == 0,
         "successful sleep issues exactly one suspend request");
}
void TestShutdown() {
  Reset();
  ExpectResult(mouneru::RequestSystemPower(PowerAction::Shutdown), true, ERROR_SUCCESS);
  Expect(fake::state.shutdowns == 1, "successful shutdown issues exactly one poweroff request");
  ExpectShutdownCleanup();
  for (DWORD error : {ERROR_SUCCESS, ERROR_ACCESS_DENIED}) {
    Reset();
    fake::state.shutdownResult = FALSE;
    fake::state.shutdownError = error;
    ExpectResult(mouneru::RequestSystemPower(PowerAction::Shutdown), false,
                 error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
    ExpectShutdownCleanup();
  }
}
void TestTokenFailures() {
  Reset();
  fake::state.openResult = FALSE;
  fake::state.openError = ERROR_ACCESS_DENIED;
  ExpectResult(mouneru::RequestSystemPower(PowerAction::Shutdown), false, ERROR_ACCESS_DENIED);
  Expect(fake::state.lookups == 0 && fake::state.adjustments == 0 &&
         fake::state.shutdowns == 0 && fake::state.closes == 0,
         "failed token open causes no further work or invalid close");
  Reset();
  fake::state.lookupResult = FALSE;
  fake::state.lookupError = ERROR_NO_SUCH_PRIVILEGE;
  ExpectResult(mouneru::RequestSystemPower(PowerAction::Shutdown), false, ERROR_NO_SUCH_PRIVILEGE);
  Expect(fake::state.adjustments == 0 && fake::state.shutdowns == 0 && fake::state.closes == 1,
         "failed privilege lookup releases token without a power request");
  for (DWORD error : {ERROR_SUCCESS, ERROR_ACCESS_DENIED, ERROR_NOT_ALL_ASSIGNED}) {
    Reset();
    fake::state.adjustResult = error == ERROR_NOT_ALL_ASSIGNED ? TRUE : FALSE;
    fake::state.adjustError = error;
    ExpectResult(mouneru::RequestSystemPower(PowerAction::Shutdown), false,
                 error == ERROR_SUCCESS ? ERROR_PRIVILEGE_NOT_HELD : error);
    Expect(fake::state.adjustments == 1 && fake::state.shutdowns == 0 && fake::state.closes == 1,
           "failed or incomplete privilege grant releases token and prevents shutdown");
  }
}
}  // namespace

int main() {
  TestSleep();
  TestShutdown();
  TestTokenFailures();
  if (failures) return 1;
  std::cout << "All power service tests passed; power and privilege APIs were stubbed.\n";
  return 0;
}
