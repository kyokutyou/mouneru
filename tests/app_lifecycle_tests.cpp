// Exercise the production window/message handlers in separate test processes.
// The real power service is never linked: these tests cannot sleep or shut down the PC.
#include "../src/main.cpp"

#include <atomic>
#include <iostream>

namespace {
bool suspendScenario{};
std::atomic<int> powerRequests{};
std::atomic<bool> workerBlocked{};
}

namespace mouneru {
PowerResult RequestSystemPower(PowerAction) noexcept {
  ++powerRequests;
  if (suspendScenario) {
    workerBlocked = true;
    if (!PostMessageW(g_app.window, WM_POWERBROADCAST, PBT_APMSUSPEND, 0))
      return {false, GetLastError()};
    // CTest allows five seconds for the entire process. A test that waits for
    // this worker instead of exiting on WM_QUIT must therefore fail its timeout.
    Sleep(15000);
    workerBlocked = false;
    return {false, ERROR_TIMEOUT};
  }
  return {true, ERROR_SUCCESS};
}
}  // namespace mouneru

int wmain(int argc, wchar_t** argv) {
  if (argc != 2 || (std::wcscmp(argv[1], L"success") != 0 &&
                    std::wcscmp(argv[1], L"suspend") != 0)) {
    std::cerr << "Expected success or suspend scenario\n";
    return 1;
  }
  suspendScenario = std::wcscmp(argv[1], L"suspend") == 0;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  Gdiplus::GdiplusStartupInput input;
  ULONG_PTR token{};
  if (!InitCommonControlsEx(&controls) ||
      Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
    if (SUCCEEDED(com)) CoUninitialize();
    std::cerr << "Cannot initialize UI test process\n";
    return 1;
  }
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const HWND window = CreateAppWindow(instance);
  if (!window) {
    Gdiplus::GdiplusShutdown(token);
    if (SUCCEEDED(com)) CoUninitialize();
    std::cerr << "Cannot create production test window\n";
    return 1;
  }
  const auto now = system_clock::now();
  const auto steadyNow = steady_clock::now();
  const bool armed = g_app.timer.StartImmediate(now, steadyNow);
  const bool executing = g_app.timer.Tick(now + seconds{6}, steadyNow + seconds{6}) ==
                         mouneru::TimerAction::RequestSleep;
  if (!armed || !executing) {
    DestroyWindow(window);
    Gdiplus::GdiplusShutdown(token);
    if (SUCCEEDED(com)) CoUninitialize();
    std::cerr << "Cannot drive immediate timer to execution\n";
    return 1;
  }
  ExecutePower();
  MSG message{};
  int result{};
  while ((result = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  const bool passed = result == 0 && message.wParam == 0 && !IsWindow(window) &&
                      powerRequests.load() == 1 &&
                      (!suspendScenario || workerBlocked.load());
  Gdiplus::GdiplusShutdown(token);
  if (SUCCEEDED(com)) CoUninitialize();
  UnregisterClassW(kWindowClass, instance);
  std::cout << (passed ? "PASS: " : "FAIL: ")
            << (suspendScenario ? "suspend closes the window and exits while the fake worker is blocked"
                                : "successful power result closes the window and exits")
            << "; requests=" << powerRequests.load()
            << "; worker_blocked=" << workerBlocked.load() << '\n';
  // Returning from main must terminate the blocked worker as well. CTest observes
  // the process termination, so a lingering UI loop or worker cannot pass.
  return passed ? 0 : 1;
}
