// Integrate the real Win32 controls in an isolated test window.
// The power service is replaced at link time. This executable cannot sleep or shut down the PC.
#include "../src/main.cpp"
#include <atomic>
#include <iostream>
#include <filesystem>

namespace {
std::atomic<int> sleepRequests{};
std::atomic<int> shutdownRequests{};
std::atomic<bool> holdPowerRequest{};
int failures{};
int dialogAnswer = IDCANCEL;
void Expect(bool condition, const char* description) {
  if (!condition) { std::cerr << "FAILED: " << description << '\n'; ++failures; }
}
bool Shown(int id) { return (GetWindowLongPtrW(Control(id), GWL_STYLE) & WS_VISIBLE) != 0; }
std::wstring Text(int id) {
  wchar_t buffer[256]{};
  GetWindowTextW(Control(id), buffer, static_cast<int>(std::size(buffer)));
  return buffer;
}
LRESULT CALLBACK AnswerOwnDialog(int code, WPARAM wParam, LPARAM lParam) {
  if (code == HCBT_ACTIVATE) {
    const HWND dialog = reinterpret_cast<HWND>(wParam);
    wchar_t type[32]{};
    GetClassNameW(dialog, type, 32);
    if (std::wcscmp(type, L"#32770") == 0 && GetWindow(dialog, GW_OWNER) == g_app.window)
      PostMessageW(dialog, WM_COMMAND, dialogAnswer, 0);
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}
void CheckLayout() {
  RECT client{};
  GetClientRect(g_app.window, &client);
  for (const auto& c : g_app.layouts) {
    RECT bounds{};
    GetWindowRect(c.window, &bounds);
    MapWindowPoints(nullptr, g_app.window, reinterpret_cast<POINT*>(&bounds), 2);
    Expect(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right &&
           bounds.bottom <= client.bottom, "every control fits in the compact client area");
  }
  Expect(client.right == Scale(g_app.window, kClientWidth) && client.bottom == Scale(g_app.window, kClientHeight),
         "client area matches the compact logical size");
}
void CheckNumberFits() {
  HDC dc = GetDC(Control(IdMinutes));
  const auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(Control(IdMinutes), WM_GETFONT, 0, 0)));
  SIZE text{};
  const auto value = Text(IdMinutes);
  GetTextExtentPoint32W(dc, value.c_str(), static_cast<int>(value.size()), &text);
  RECT rect{};
  GetClientRect(Control(IdMinutes), &rect);
  Expect(text.cx + Scale(g_app.window, 6) <= rect.right, "four-digit minute values fit without horizontal clipping");
  SelectObject(dc, old);
  ReleaseDC(Control(IdMinutes), dc);
}
void PressEnter(bool repeated) {
  MSG key{};
  key.hwnd = GetFocus();
  key.message = WM_KEYDOWN;
  key.wParam = VK_RETURN;
  key.lParam = 1 | (0x1c << 16) | (repeated ? (static_cast<LPARAM>(1) << 30) : 0);
  if (!IsDialogMessageW(g_app.window, &key)) {
    TranslateMessage(&key);
    DispatchMessageW(&key);
  }
}
void TestHeldEnter() {
  for (int id : {IdMinutes, IdHour, IdSchedule, IdSleepNow}) {
    HandleCommand(id == IdHour ? IdModeClock : IdModeMinutes);
    SetFocus(Control(id));
    PressEnter(false);
    const auto expected = id == IdSleepNow ? mouneru::TimerState::ImmediateCountdown
                                          : mouneru::TimerState::Scheduled;
    Expect(g_app.timer.state() == expected && GetFocus() == Control(IdCancel),
           "first Enter starts the selected timer and focuses cancellation");
    PressEnter(true);
    Expect(g_app.timer.state() == expected, "holding Enter cannot cancel the timer it just started");
    SendMessageW(GetFocus(), WM_KEYUP, VK_RETURN,
                 1 | (0x1c << 16) | (static_cast<LPARAM>(3) << 30));
    SetFocus(Control(IdCancel));
    PressEnter(false);
    Expect(g_app.timer.state() == mouneru::TimerState::Idle,
           "a separate Enter press can still deliberately cancel the timer");
  }
  HandleCommand(IdModeMinutes);
}
void WaitForPowerResult() {
  const auto deadline = steady_clock::now() + seconds{2};
  while (g_app.powerRequestInFlight && steady_clock::now() < deadline) {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    Sleep(1);
  }
}
void TestPowerSelection() {
  Expect(Text(IdFooter) == L"アプリを終了すると予約は取り消されます", "requested footer text is displayed");
  Expect(SendMessageW(Control(IdPowerSleep), BM_GETCHECK, 0, 0) == BST_CHECKED,
         "sleep is the default radio selection");
  SendMessageW(Control(IdPowerShutdown), BM_CLICK, 0, 0);
  Expect(g_app.selectedPower == mouneru::PowerAction::Shutdown &&
         Text(IdSchedule).find(L"シャットダウン") != std::wstring::npos &&
         Text(IdSleepNow).find(L"シャットダウン") != std::wstring::npos &&
         Text(IdEstimate).find(L"シャットダウン") != std::wstring::npos,
         "radio click updates the preview and both action labels");
  SendMessageW(Control(IdPowerShutdown), WM_KEYDOWN, VK_LEFT, 0);
  Expect(g_app.selectedPower == mouneru::PowerAction::Sleep && GetFocus() == Control(IdPowerSleep),
         "arrow key selects sleep and moves focus");
  SendMessageW(Control(IdPowerSleep), WM_KEYDOWN, VK_RIGHT, 0);
  Expect(g_app.selectedPower == mouneru::PowerAction::Shutdown,
         "arrow key selects shutdown");
  ScheduleFromInputs();
  Expect(g_app.reservedPower == mouneru::PowerAction::Shutdown && !Shown(IdPowerShutdown) &&
         Text(IdActiveTarget).find(L"シャットダウン") != std::wstring::npos,
         "scheduled operation is captured and shown on countdown");
  const auto originalTarget = g_app.timer.target();
  HandleCommand(IdPowerSleep);
  Expect(g_app.selectedPower == mouneru::PowerAction::Shutdown,
         "hidden power controls cannot change a reservation");
  HandleCommand(IdChange);
  HandleCommand(IdPowerSleep);
  dialogAnswer = IDCANCEL;
  ScheduleFromInputs();
  Expect(g_app.timer.target() == originalTarget && g_app.reservedPower == mouneru::PowerAction::Shutdown,
         "declining replacement preserves the original operation");
  HandleCommand(IdReturn);
  HandleCommand(IdChange);
  Expect(g_app.selectedPower == mouneru::PowerAction::Shutdown,
         "reopening edit restores the reserved operation");
  HandleCommand(IdPowerSleep);
  dialogAnswer = IDOK;
  ScheduleFromInputs();
  Expect(g_app.reservedPower == mouneru::PowerAction::Sleep,
         "confirmed replacement changes the reserved operation");
  HandleCommand(IdCancel);
  HandleCommand(IdPowerShutdown);
  HandleCommand(IdSleepNow);
  Expect(Text(IdCancel).find(L"シャットダウン") != std::wstring::npos,
         "immediate shutdown names the operation on cancellation");
  SendMessageW(Control(IdCancel), WM_KEYDOWN, VK_ESCAPE, 0);
  Expect(g_app.timer.state() == mouneru::TimerState::Idle && shutdownRequests == 0,
         "Escape cancels immediate shutdown before any OS request");
  ScheduleFromInputs();
  HandleCommand(IdChange);
  HandleCommand(IdPowerSleep);
  Expect(g_app.timer.Tick(g_app.timer.target()) == mouneru::TimerAction::RequestSleep,
         "original reservation can expire while its replacement is being edited");
  ExecutePower();
  WaitForPowerResult();
  Expect(shutdownRequests == 1 && sleepRequests == 0 &&
         g_app.timer.state() == mouneru::TimerState::Idle &&
         Text(IdError).find(L"シャットダウン") != std::wstring::npos,
         "execution uses the reserved shutdown rather than the unconfirmed editor choice");
  HandleCommand(IdPowerSleep);
  ClearErrors();
  dialogAnswer = IDCANCEL;
}
void TestSuspendDuringShutdown() {
  HandleCommand(IdPowerShutdown);
  HandleCommand(IdSleepNow);
  const int initialRequests = shutdownRequests;
  holdPowerRequest = true;
  Expect(g_app.timer.Tick(g_app.timer.target()) == mouneru::TimerAction::RequestSleep,
         "shutdown timer reaches execution before suspend notification");
  ExecutePower();
  SendMessageW(g_app.window, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
  const bool stayedOpen = IsWindow(g_app.window) != FALSE;
  Expect(stayedOpen && g_app.powerRequestInFlight,
         "external suspend cannot close an in-flight shutdown request");
  holdPowerRequest = false;
  WaitForPowerResult();
  Expect(stayedOpen && shutdownRequests == initialRequests + 1 &&
         g_app.timer.state() == mouneru::TimerState::Idle &&
         Text(IdError).find(L"シャットダウン") != std::wstring::npos,
         "shutdown failure remains visible after unrelated suspend notification");
  HandleCommand(IdPowerSleep);
  ClearErrors();
}
void SendDialogKey(UINT message, WPARAM key, bool shift = false) {
  BYTE previous[256]{}, keyboard[256]{};
  GetKeyboardState(previous);
  std::copy(std::begin(previous), std::end(previous), std::begin(keyboard));
  keyboard[VK_SHIFT] = shift ? 0x80 : 0;
  SetKeyboardState(keyboard);
  MSG msg{};
  msg.hwnd = GetFocus();
  msg.message = message;
  msg.wParam = key;
  msg.lParam = 1;
  if (!IsDialogMessageW(g_app.window, &msg)) DispatchMessageW(&msg);
  SetKeyboardState(previous);
}
void TestRadioKeyboardNavigation() {
  HandleCommand(IdPowerSleep);
  SetFocus(Control(IdPreset60));
  SendDialogKey(WM_KEYDOWN, VK_TAB);
  Expect(GetFocus() == Control(IdPowerSleep), "Tab from presets reaches the selected radio");
  SendDialogKey(WM_KEYDOWN, VK_RIGHT);
  Expect(g_app.selectedPower == mouneru::PowerAction::Shutdown &&
         GetFocus() == Control(IdPowerShutdown), "dialog message path handles radio arrows");
  SendDialogKey(WM_KEYDOWN, VK_TAB);
  Expect(GetFocus() == Control(IdSchedule), "Tab exits the radio group to reservation");
  SendDialogKey(WM_KEYDOWN, VK_TAB, true);
  Expect(GetFocus() == Control(IdPowerShutdown), "Shift+Tab returns to the selected radio");
  SetFocus(Control(IdPowerSleep));
  SendDialogKey(WM_KEYDOWN, VK_SPACE);
  SendDialogKey(WM_KEYUP, VK_SPACE);
  Expect(g_app.selectedPower == mouneru::PowerAction::Sleep,
         "Space activates the focused radio through the dialog message path");
  SendDialogKey(WM_SYSCHAR, L'd');
  Expect(g_app.selectedPower == mouneru::PowerAction::Shutdown,
         "Alt+D activates the shutdown radio through the dialog message path");
  Expect(g_app.timer.state() == mouneru::TimerState::Idle,
         "keyboard power selection alone never starts a reservation");
  HandleCommand(IdPowerSleep);
}
void TestRapidStepperClicks() {
  SetControlText(Control(IdMinutes), L"30");
  const LPARAM point = MAKELPARAM(10, 10);
  for (int id : {IdIncrease, IdDecrease}) {
    const int start = *ReadInteger(Control(IdMinutes));
    for (int pair = 0; pair < 20; ++pair) {
      // Windows classifies the second fast press as a double-click message.
      SendMessageW(Control(id), WM_LBUTTONDOWN, MK_LBUTTON, point);
      SendMessageW(Control(id), WM_LBUTTONUP, 0, point);
      SendMessageW(Control(id), WM_LBUTTONDBLCLK, MK_LBUTTON, point);
      SendMessageW(Control(id), WM_LBUTTONUP, 0, point);
      Expect(ReadInteger(Control(IdMinutes)) == start + (pair + 1) * (id == IdIncrease ? 10 : -10),
             "every fast physical stepper click changes the minute value");
      Expect(!GetUpdateRect(Control(IdMinutes), nullptr, FALSE),
             "the minute value is painted before the next queued click");
    }
  }
  SetControlText(Control(IdMinutes), L"30");
  ValidateRect(g_app.window, nullptr);
  HandleCommand(IdIncrease);
  Expect(!GetUpdateRect(g_app.window, nullptr, FALSE), "a minute change does not erase the whole window");
  SetControlText(Control(IdMinutes), L"30");
}
std::vector<BYTE> RenderButtonPixels(UINT state) {
  RECT rect{};
  GetClientRect(Control(IdPreset15), &rect);
  HDC target = GetDC(Control(IdPreset15)), memory = CreateCompatibleDC(target);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = rect.right;
  info.bmiHeader.biHeight = -rect.bottom;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* pixels{};
  HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  const auto old = SelectObject(memory, bitmap);
  DRAWITEMSTRUCT item{};
  item.CtlType = ODT_BUTTON;
  item.CtlID = IdPreset15;
  item.itemAction = ODA_DRAWENTIRE;
  item.itemState = state;
  item.hwndItem = Control(IdPreset15);
  item.hDC = memory;
  item.rcItem = rect;
  DrawButton(item);
  GdiFlush();
  const auto bytes = static_cast<BYTE*>(pixels);
  std::vector<BYTE> result(bytes, bytes + rect.right * rect.bottom * 4);
  SelectObject(memory, old);
  DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(Control(IdPreset15), target);
  return result;
}
void TestMouseFocusCues() {
  SendMessageW(g_app.window, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  const auto ordinary = RenderButtonPixels(0);
  Expect(ordinary == RenderButtonPixels(ODS_FOCUS | ODS_NOFOCUSRECT),
         "mouse focus does not draw a focus rectangle when Windows hides focus cues");
  Expect(ordinary != RenderButtonPixels(ODS_FOCUS), "keyboard focus remains visibly identifiable");
  SendMessageW(g_app.window, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
  Expect(ordinary == RenderButtonPixels(ODS_FOCUS), "window-level hidden focus cues are respected");
}
void TestHoverInvalidation() {
  const bool visible = IsWindowVisible(g_app.window) != FALSE;
  if (!visible) ShowWindow(g_app.window, SW_SHOWNOACTIVATE);
  for (const auto& layout : g_app.layouts) {
    HWND control = layout.window;
    wchar_t type[32]{};
    GetClassNameW(control, type, static_cast<int>(std::size(type)));
    const bool button = std::wcscmp(type, L"BUTTON") == 0;
    if (!IsWindowVisible(control) || (!button && std::wcscmp(type, L"EDIT") != 0)) continue;
    SendMessageW(control, WM_MOUSELEAVE, 0, 0);
    ValidateRect(control, nullptr);
    SendMessageW(control, WM_MOUSEMOVE, 0, MAKELPARAM(5, 5));
    if (button) Expect(GetUpdateRect(control, nullptr, FALSE), "entering a button repaints its hover appearance");
    ValidateRect(control, nullptr);
    for (int i = 6; i < 20; ++i) SendMessageW(control, WM_MOUSEMOVE, 0, MAKELPARAM(i, 5));
    Expect(!GetUpdateRect(control, nullptr, FALSE), "moving inside a control does not repeatedly invalidate it");
    SendMessageW(control, WM_MOUSELEAVE, 0, 0);
    if (button) Expect(GetUpdateRect(control, nullptr, FALSE), "leaving a button repaints its normal appearance");
    ValidateRect(control, nullptr);
  }
  if (!visible) ShowWindow(g_app.window, SW_HIDE);
}
void SaveRender(const std::filesystem::path& file) {
  RECT client{};
  GetClientRect(g_app.window, &client);
  HDC dc = GetDC(g_app.window), memory = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
  const auto old = SelectObject(memory, bitmap);
  ShowWindow(g_app.window, SW_SHOWNOACTIVATE);
  RedrawWindow(g_app.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
  PrintWindow(g_app.window, memory, PW_CLIENTONLY | 2);
  ShowWindow(g_app.window, SW_HIDE);
  SelectObject(memory, old);
  Gdiplus::Bitmap image(bitmap, nullptr);
  UINT count{}, size{};
  Gdiplus::GetImageEncodersSize(&count, &size);
  std::vector<BYTE> buffer(size);
  auto encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
  Gdiplus::GetImageEncoders(count, size, encoders);
  bool saved = false;
  for (UINT i = 0; i < count; ++i) if (std::wcscmp(encoders[i].MimeType, L"image/png") == 0)
    saved = image.Save(file.c_str(), &encoders[i].Clsid) == Gdiplus::Ok;
  Expect(saved, "native client render saved");
  DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(g_app.window, dc);
}
} // namespace
namespace mouneru {
PowerResult RequestSystemPower(PowerAction action) noexcept {
  if (action == PowerAction::Shutdown) ++shutdownRequests;
  else ++sleepRequests;
  while (holdPowerRequest) Sleep(1);
  return {false, ERROR_ACCESS_DENIED};
}
}
int wmain(int argc, wchar_t** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  Gdiplus::GdiplusStartupInput input;
  ULONG_PTR token{};
  if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return 1;
  HWND window = CreateAppWindow(GetModuleHandleW(nullptr));
  if (!window) { std::cerr << "Cannot create test window\n"; return 1; }
  KillTimer(window, kTickTimer); // Tests advance the timer explicitly; no real-time execution.
  HHOOK hook = SetWindowsHookExW(WH_CBT, AnswerOwnDialog, nullptr, GetCurrentThreadId());
  Expect(hook != nullptr, "own-thread dialog test hook available");
  const std::filesystem::path renderDir = argc == 2 ? argv[1] : L"";
  if (!renderDir.empty()) std::filesystem::create_directories(renderDir);
  auto snapshot = [&](const wchar_t* name) { if (!renderDir.empty()) SaveRender(renderDir / name); };
  CheckLayout();
  Expect(Shown(IdMinutes) && !Shown(IdHour) && !Shown(IdCancel), "initial screen only shows minute editor");
  Expect(ReadInteger(Control(IdMinutes)) == 30, "default duration is 30 minutes");
  snapshot(L"quiet-minutes.png");
  ShowWindow(window, SW_SHOWNOACTIVATE);
  TestPowerSelection();
  TestSuspendDuringShutdown();
  TestRadioKeyboardNavigation();
  HandleCommand(IdPowerShutdown);
  snapshot(L"power-shutdown.png");
  ScheduleFromInputs();
  snapshot(L"power-shutdown-scheduled.png");
  HandleCommand(IdCancel);
  HandleCommand(IdSleepNow);
  snapshot(L"power-shutdown-immediate.png");
  HandleCommand(IdCancel);
  HandleCommand(IdPowerSleep);
  TestHeldEnter();
  TestRapidStepperClicks();
  TestMouseFocusCues();
  TestHoverInvalidation();
  SendMessageW(Control(IdPreset15), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
  SendMessageW(Control(IdPreset15), WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
  Expect(ReadInteger(Control(IdMinutes)) == 15, "mouse click selects the preset");
  snapshot(L"quiet-preset-mouse.png");
  SetFocus(Control(IdModeMinutes));
  MSG arrow{};
  arrow.hwnd = Control(IdModeMinutes);
  arrow.message = WM_KEYDOWN;
  arrow.wParam = VK_RIGHT;
  if (!IsDialogMessageW(window, &arrow)) DispatchMessageW(&arrow);
  Expect(g_app.mode == InputMode::Clock, "mode arrow keys work through the production dialog message path");
  MSG tab{};
  tab.hwnd = Control(IdModeClock);
  tab.message = WM_KEYDOWN;
  tab.wParam = VK_TAB;
  if (!IsDialogMessageW(window, &tab)) DispatchMessageW(&tab);
  Expect(GetFocus() == Control(IdHour), "Tab leaves the mode selector and reaches the visible clock input");
  TestHoverInvalidation();
  HandleCommand(IdModeMinutes);
  ShowWindow(window, SW_HIDE);
  HandleCommand(IdPreset15);
  Expect(g_app.timer.state() == mouneru::TimerState::Idle, "preset chooses time without starting sleep");
  Expect(ReadInteger(Control(IdMinutes)) == 15 && Selected(IdPreset15), "preset selection is reflected");
  SetControlText(Control(IdMinutes), L"0");
  ScheduleFromInputs();
  Expect(g_app.timer.state() == mouneru::TimerState::Idle && !Text(IdInputError).empty(), "invalid minutes do not schedule");
  SetControlText(Control(IdMinutes), L"1441");
  Expect(!InputTarget(), "values over 1440 are rejected");
  SetControlText(Control(IdMinutes), L"1440");
  RefreshUi();
  Expect(InputTarget().has_value(), "1440 minutes are supported");
  CheckNumberFits();
  snapshot(L"quiet-max-minutes.png");
  HandleCommand(IdIncrease);
  Expect(ReadInteger(Control(IdMinutes)) == 1440, "increase respects upper bound");
  SetControlText(Control(IdMinutes), L"1");
  HandleCommand(IdDecrease);
  Expect(ReadInteger(Control(IdMinutes)) == 1, "decrease respects lower bound");
  HandleCommand(IdModeClock);
  Expect(!Shown(IdMinutes) && Shown(IdHour) && Shown(IdMinute), "clock tab replaces minute editor");
  SetControlText(Control(IdHour), L"24");
  ScheduleFromInputs();
  Expect(!Text(IdInputError).empty() && !InputTarget(), "invalid clock hours are rejected");
  SetControlText(Control(IdHour), L"23");
  SetControlText(Control(IdMinute), L"00");
  RefreshUi();
  snapshot(L"quiet-clock.png");
  const auto target = InputTarget();
  Expect(target && *target > system_clock::now(), "clock target is always in the future");
  HandleCommand(IdModeMinutes);
  HandleCommand(IdPreset30);
  SendMessageW(Control(IdMinutes), WM_KEYDOWN, VK_RETURN, 0);
  Expect(g_app.timer.state() == mouneru::TimerState::Scheduled && Shown(IdCancel) &&
         !Shown(IdMinutes) && Shown(IdCountdown), "Enter schedules and switches to countdown");
  snapshot(L"quiet-scheduled.png");
  TestHoverInvalidation();
  const auto oldTarget = g_app.timer.target();
  HandleCommand(IdChange);
  Expect(g_app.editing && Shown(IdReturn) && !Shown(IdSleepNow), "editing keeps old timer active and offers return");
  TestHoverInvalidation();
  HandleCommand(IdPreset60);
  dialogAnswer = IDCANCEL;
  ScheduleFromInputs();
  Expect(g_app.timer.target() == oldTarget && g_app.editing, "declining replacement preserves old reservation");
  HandleCommand(IdModeClock);
  StartReviewedReservation(system_clock::now() - seconds{1});
  Expect(g_app.timer.target() == oldTarget && g_app.editing && !Text(IdInputError).empty(),
         "a clock time expiring during confirmation preserves old reservation and reports error");
  const auto confirmedClock = system_clock::now() + minutes{90};
  StartReviewedReservation(confirmedClock);
  Expect(g_app.timer.target() == confirmedClock && !g_app.editing,
         "clock replacement uses exactly the absolute time that was reviewed");
  HandleCommand(IdChange);
  HandleCommand(IdModeMinutes);
  dialogAnswer = IDOK;
  ScheduleFromInputs();
  Expect(g_app.timer.target() > oldTarget && !g_app.editing, "confirming replacement uses new target");
  HandleCommand(IdChange);
  HandleCommand(IdReturn);
  Expect(!g_app.editing && Shown(IdCountdown), "return leaves reservation active");
  HandleCommand(IdCancel);
  Expect(g_app.timer.state() == mouneru::TimerState::Idle && Shown(IdMinutes), "cancel restores editor");
  HandleCommand(IdSleepNow);
  Expect(g_app.timer.state() == mouneru::TimerState::ImmediateCountdown && !Shown(IdChange), "immediate countdown only offers cancellation");
  snapshot(L"quiet-immediate.png");
  TestHoverInvalidation();
  SendMessageW(Control(IdCancel), WM_KEYDOWN, VK_ESCAPE, 0);
  Expect(g_app.timer.state() == mouneru::TimerState::Idle && sleepRequests == 0, "Escape cancels without requesting sleep");
  HandleCommand(IdSleepNow);
  Expect(g_app.timer.Tick(g_app.timer.target()) == mouneru::TimerAction::RequestSleep, "immediate timer reaches execution");
  RefreshUi();
  Expect(!IsWindowEnabled(Control(IdCancel)), "executing cannot be cancelled");
  ExecutePower(); // Linked stub; this does not call any OS power API.
  WaitForPowerResult();
  Expect(sleepRequests == 1 && g_app.timer.state() == mouneru::TimerState::Idle &&
         !Text(IdError).empty(), "failed sleep returns to usable editor with error");
  ClearErrors();
  HandleCommand(IdPreset30);
  Relayout();
  CheckLayout();
  ScheduleFromInputs();
  dialogAnswer = IDCANCEL;
  SendMessageW(window, WM_CLOSE, 0, 0);
  Expect(IsWindow(window) && g_app.timer.HasActiveTimer(), "declining exit preserves window and reservation");
  dialogAnswer = IDOK;
  SendMessageW(window, WM_CLOSE, 0, 0);
  Expect(!IsWindow(window), "confirmed exit closes the window");
  UnhookWindowsHookEx(hook);
  Gdiplus::GdiplusShutdown(token);
  if (SUCCEEDED(com)) CoUninitialize();
  if (failures) return 1;
  std::cout << "All Quiet UI integration tests passed; OS power requests were stubbed.\n";
  return 0;
}
