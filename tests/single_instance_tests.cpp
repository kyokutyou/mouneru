#include "single_instance.h"

#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {
int failures{};
constexpr DWORD kAlreadyRunning = 20;

void Expect(bool condition, std::string_view description) {
  if (!condition) {
    std::cerr << "FAILED: " << description << '\n';
    ++failures;
  }
}

std::wstring TestName(std::wstring_view suffix) {
  return L"Local\\MouNeru.Test." + std::to_wstring(GetCurrentProcessId()) + L"." +
         std::to_wstring(GetTickCount64()) + L"." + std::wstring(suffix);
}

class ChildProcess final {
 public:
  explicit ChildProcess(const std::wstring& arguments) {
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) return;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &process)) {
      process_ = process.hProcess;
      CloseHandle(process.hThread);
    }
  }
  ~ChildProcess() {
    if (!process_) return;
    // Only this test's own child is ever terminated, including failed assertions.
    if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
      TerminateProcess(process_, 99);
      WaitForSingleObject(process_, 2000);
    }
    CloseHandle(process_);
  }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  [[nodiscard]] bool started() const { return process_ != nullptr; }
  DWORD Wait() const {
    if (!process_ || WaitForSingleObject(process_, 5000) != WAIT_OBJECT_0) return 98;
    DWORD result{};
    return GetExitCodeProcess(process_, &result) ? result : 97;
  }
  void CrashForTest() const {
    if (process_) TerminateProcess(process_, 17);
  }

 private:
  HANDLE process_{};
};

int Probe(const wchar_t* name) {
  mouneru::SingleInstance instance(name);
  if (instance.state() == mouneru::InstanceState::Acquired) return 0;
  return instance.state() == mouneru::InstanceState::AlreadyRunning ? kAlreadyRunning : 21;
}

int Hold(const wchar_t* name, const wchar_t* readyName, const wchar_t* releaseName) {
  mouneru::SingleInstance instance(name);
  if (instance.state() != mouneru::InstanceState::Acquired) return 22;
  HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
  HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, releaseName);
  if (!ready || !release) {
    if (ready) CloseHandle(ready);
    if (release) CloseHandle(release);
    return 23;
  }
  SetEvent(ready);
  const DWORD result = WaitForSingleObject(release, 5000);
  CloseHandle(ready);
  CloseHandle(release);
  return result == WAIT_OBJECT_0 ? 0 : 24;
}

void TestLivePrimaryAndRelease() {
  const auto name = TestName(L"lifetime");
  {
    mouneru::SingleInstance primary(name.c_str());
    Expect(primary.state() == mouneru::InstanceState::Acquired && primary.error_code() == ERROR_SUCCESS,
           "first instance is acquired");
    ChildProcess second(L"--probe \"" + name + L"\"");
    Expect(second.started() && second.Wait() == kAlreadyRunning,
           "an independent process cannot acquire a live primary's instance");
  }
  ChildProcess replacement(L"--probe \"" + name + L"\"");
  Expect(replacement.started() && replacement.Wait() == 0,
         "a new process can acquire after the primary guard is destroyed");
}

void TestPrimaryProcessExit(bool crash) {
  const auto name = TestName(crash ? L"crash" : L"normal-exit");
  const auto readyName = name + L".ready", releaseName = name + L".release";
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
  if (!ready || !release) {
    Expect(false, "child process coordination events are created");
    if (ready) CloseHandle(ready);
    if (release) CloseHandle(release);
    return;
  }
  {
    ChildProcess primary(L"--hold \"" + name + L"\" \"" + readyName + L"\" \"" + releaseName + L"\"");
    const bool active = primary.started() && WaitForSingleObject(ready, 2000) == WAIT_OBJECT_0;
    Expect(active, "independent primary signals readiness while holding the instance");
    if (active) {
      ChildProcess second(L"--probe \"" + name + L"\"");
      Expect(second.started() && second.Wait() == kAlreadyRunning,
             "a second process is refused while the primary process is alive");
      if (crash) primary.CrashForTest();
      else SetEvent(release);
      Expect(primary.Wait() == (crash ? 17U : 0U), "primary process has exited");
    }
  }
  CloseHandle(ready);
  CloseHandle(release);
  ChildProcess replacement(L"--probe \"" + name + L"\"");
  Expect(replacement.started() && replacement.Wait() == 0,
         crash ? "process death leaves no stale single-instance registration"
               : "normal process exit releases single-instance registration");
}

void TestCreationFailure() {
  const auto name = TestName(L"name-conflict");
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
  Expect(event != nullptr, "conflicting event object is created");
  if (event) {
    mouneru::SingleInstance instance(name.c_str());
    Expect(instance.state() == mouneru::InstanceState::Failed && instance.error_code() != ERROR_SUCCESS,
           "mutex creation failure does not admit another application instance");
    CloseHandle(event);
  }
  for (const wchar_t* nameValue : {static_cast<const wchar_t*>(nullptr), L""}) {
    mouneru::SingleInstance instance(nameValue);
    Expect(instance.state() == mouneru::InstanceState::Failed &&
           instance.error_code() == ERROR_INVALID_PARAMETER,
           "empty names cannot silently disable single-instance protection");
  }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring_view(argv[1]) == L"--probe") return Probe(argv[2]);
  if (argc == 5 && std::wstring_view(argv[1]) == L"--hold") return Hold(argv[2], argv[3], argv[4]);
  TestLivePrimaryAndRelease();
  TestPrimaryProcessExit(false);
  TestPrimaryProcessExit(true);
  TestCreationFailure();
  if (failures) return 1;
  std::cout << "All single-instance tests passed; independent child processes were isolated.\n";
  return 0;
}
