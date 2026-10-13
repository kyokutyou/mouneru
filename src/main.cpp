#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <initguid.h>
#include <oleacc.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <ctime>
#include <cwchar>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "sleep_service.h"
#include "timer_manager.h"

namespace {
using namespace std::chrono;
constexpr wchar_t kWindowClass[] = L"MouNeruQuietWindow";
constexpr wchar_t kHoverProperty[] = L"MouNeru.ButtonHovered";
constexpr int kClientWidth = 380, kClientHeight = 432;
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickIntervalMs = 250, kPowerResultMessage = WM_APP + 1;
enum ControlId : int {
  IdBrand = 100, IdBrandHint, IdModeMinutes, IdModeClock, IdDecrease, IdMinutes,
  IdMinutesUnit, IdIncrease, IdHour, IdColon, IdMinute, IdEstimate, IdInputError,
  IdPreset15, IdPreset30, IdPreset60, IdSchedule, IdSleepNow, IdReturn,
  IdActiveHeading, IdCountdown, IdActiveTarget, IdCancel, IdChange, IdFooter, IdError, IdLogo,
  IdPowerSleep, IdPowerShutdown,
};
enum class InputMode { Minutes, Clock };
enum class FontRole { Body, Small, Brand, Number, Clock, Countdown };
enum class Region { Always, Editor, Minutes, Clock, Active };
struct ControlLayout {
  HWND window;
  int x, y, width, height;
  FontRole font;
  Region region;
};
struct Palette {
  COLORREF paper, ink, muted, line, accent, selected, track, error;
  bool highContrast;
};
struct AppState {
  mouneru::TimerManager timer;
  HWND window{};
  HFONT fonts[6]{};
  HBRUSH paperBrush{};
  InputMode mode{InputMode::Minutes};
  mouneru::PowerAction selectedPower{mouneru::PowerAction::Sleep};
  mouneru::PowerAction reservedPower{mouneru::PowerAction::Sleep};
  struct WorkerRequest { HWND window; mouneru::PowerAction action; } workerRequest{};
  bool editing{}, powerRequestInFlight{}, refreshing{}, confirmationOpen{};
  bool selectionReady{}, selections[5]{};
  std::vector<ControlLayout> layouts;
};
AppState g_app;

std::wstring PowerName(mouneru::PowerAction action) {
  return action == mouneru::PowerAction::Shutdown ? L"シャットダウン" : L"スリープ";
}

int Scale(HWND window, int value) {
  return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}
Palette Colors() {
  HIGHCONTRASTW contrast{sizeof(contrast)};
  const bool hc = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                  (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
  if (hc) return {GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT),
      GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOWTEXT),
      GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_HIGHLIGHT),
      GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT), true};
  return {RGB(250, 250, 245), RGB(46, 59, 52), RGB(96, 112, 98), RGB(221, 227, 214),
          RGB(63, 87, 73), RGB(231, 237, 228), RGB(236, 239, 231), RGB(163, 55, 43), false};
}
HWND Control(int id) { return GetDlgItem(g_app.window, id); }
void SetControlText(HWND control, const std::wstring& text) {
  const int length = GetWindowTextLengthW(control);
  std::wstring current(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(control, current.data(), length + 1);
  current.resize(static_cast<size_t>(length));
  if (current != text) SetWindowTextW(control, text.c_str());
}
void SetAccessibleName(HWND control, const wchar_t* name) {
  IAccPropServices* services = nullptr;
  if (control && SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr,
      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&services)))) {
    (void)services->SetHwndPropStr(control, static_cast<DWORD>(OBJID_CLIENT),
        static_cast<DWORD>(CHILDID_SELF), PROPID_ACC_NAME, name);
    services->Release();
  }
}
void RefreshUi();
void ScheduleFromInputs();
void HandleCommand(int id);

LRESULT CALLBACK ControlProc(HWND control, UINT message, WPARAM wParam,
                             LPARAM lParam, UINT_PTR, DWORD_PTR isButton) {
  if (message == WM_LBUTTONDBLCLK &&
      (GetDlgCtrlID(control) == IdIncrease || GetDlgCtrlID(control) == IdDecrease)) {
    // Treat every stepper press equally, including the second fast press.
    return DefSubclassProc(control, WM_LBUTTONDOWN, wParam, lParam);
  }
  if (isButton && message == WM_ERASEBKGND &&
      (GetWindowLongPtrW(control, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW) return TRUE;
  if (isButton && message == WM_MOUSEMOVE && !GetPropW(control, kHoverProperty)) {
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
    if (TrackMouseEvent(&tracking)) {
      SetPropW(control, kHoverProperty, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
      InvalidateRect(control, nullptr, FALSE);
    }
  } else if (isButton && message == WM_MOUSELEAVE) {
    if (RemovePropW(control, kHoverProperty)) InvalidateRect(control, nullptr, FALSE);
  } else if (isButton && (message == WM_SETFOCUS || message == WM_KILLFOCUS)) {
    InvalidateRect(control, nullptr, FALSE);
  } else if (message == WM_GETDLGCODE) {
    const int id = GetDlgCtrlID(control);
    LRESULT flags = DefSubclassProc(control, message, wParam, lParam);
    if (id == IdModeMinutes || id == IdModeClock || id == IdPowerSleep || id == IdPowerShutdown)
      flags |= DLGC_WANTARROWS;
    const auto key = reinterpret_cast<const MSG*>(lParam);
    if (key && key->message == WM_KEYDOWN && (key->wParam == VK_RETURN || key->wParam == VK_ESCAPE))
      flags |= DLGC_WANTMESSAGE;
    return flags;
  } else if (message == WM_KEYDOWN) {
    if (wParam == VK_ESCAPE) {
      if (g_app.timer.state() == mouneru::TimerState::ImmediateCountdown) HandleCommand(IdCancel);
      else if (g_app.editing) HandleCommand(IdReturn);
      return 0;
    }
    if (wParam == VK_RETURN) {
      // A held key must not activate the Cancel button after focus moves there.
      if (lParam & (static_cast<LPARAM>(1) << 30)) return 0;
      const int id = GetDlgCtrlID(control);
      HandleCommand(id == IdMinutes || id == IdHour || id == IdMinute ? IdSchedule : id);
      return 0;
    }
    if ((GetDlgCtrlID(control) == IdModeMinutes || GetDlgCtrlID(control) == IdModeClock) &&
        (wParam == VK_LEFT || wParam == VK_RIGHT)) {
      HandleCommand(g_app.mode == InputMode::Minutes ? IdModeClock : IdModeMinutes);
      SetFocus(Control(g_app.mode == InputMode::Minutes ? IdModeMinutes : IdModeClock));
      return 0;
    }
    if ((GetDlgCtrlID(control) == IdPowerSleep || GetDlgCtrlID(control) == IdPowerShutdown) &&
        (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN)) {
      const int id = g_app.selectedPower == mouneru::PowerAction::Sleep ? IdPowerShutdown : IdPowerSleep;
      HandleCommand(id);
      SetFocus(Control(id));
      return 0;
    }
  } else if (message == WM_NCDESTROY) {
    RemovePropW(control, kHoverProperty);
    RemoveWindowSubclass(control, ControlProc, 1);
  }
  return DefSubclassProc(control, message, wParam, lParam);
}
HWND AddControl(const wchar_t* type, const wchar_t* text, DWORD style, int id,
                int x, int y, int width, int height, FontRole font, Region region) {
  HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
      Scale(g_app.window, x), Scale(g_app.window, y), Scale(g_app.window, width),
      Scale(g_app.window, height), g_app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  if (control) {
    g_app.layouts.push_back({control, x, y, width, height, font, region});
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fonts[static_cast<int>(font)]), TRUE);
    if ((style & WS_TABSTOP) || std::wcscmp(type, L"BUTTON") == 0) SetWindowSubclass(control, ControlProc, 1,
        std::wcscmp(type, L"BUTTON") == 0 ? 1 : 0);
  }
  return control;
}
void RecreateFonts() {
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) return;
  constexpr int sizes[]{10, 8, 11, 36, 28, 34};
  HFONT replacements[6]{};
  for (int i = 0; i < 6; ++i) {
    LOGFONTW face = metrics.lfMessageFont;
    wcscpy_s(face.lfFaceName, L"Segoe UI Variable Text");
    face.lfHeight = -MulDiv(sizes[i], static_cast<int>(GetDpiForWindow(g_app.window)), 72);
    face.lfWeight = i == static_cast<int>(FontRole::Brand) ? FW_SEMIBOLD
                    : i >= static_cast<int>(FontRole::Number) ? FW_LIGHT : FW_NORMAL;
    face.lfQuality = CLEARTYPE_QUALITY;
    replacements[i] = CreateFontIndirectW(&face);
    if (!replacements[i]) {
      for (HFONT replacement : replacements) if (replacement) DeleteObject(replacement);
      return;
    }
  }
  for (int i = 0; i < 6; ++i) {
    HFONT old = g_app.fonts[i];
    g_app.fonts[i] = replacements[i];
    for (const auto& layout : g_app.layouts) if (static_cast<int>(layout.font) == i)
      SendMessageW(layout.window, WM_SETFONT, reinterpret_cast<WPARAM>(replacements[i]), TRUE);
    if (old) DeleteObject(old);
  }
}
void Relayout() {
  RecreateFonts();
  for (const auto& layout : g_app.layouts) SetWindowPos(layout.window, nullptr,
      Scale(g_app.window, layout.x), Scale(g_app.window, layout.y),
      Scale(g_app.window, layout.width), Scale(g_app.window, layout.height), SWP_NOACTIVATE | SWP_NOZORDER);
  InvalidateRect(g_app.window, nullptr, TRUE);
}
void RecreateBrush() {
  if (g_app.paperBrush) DeleteObject(g_app.paperBrush);
  g_app.paperBrush = CreateSolidBrush(Colors().paper);
}
void RoundedRect(HDC dc, const RECT& bounds, int radius, COLORREF fill, COLORREF line) {
  using namespace Gdiplus;
  Graphics graphics(dc);
  graphics.SetSmoothingMode(SmoothingModeAntiAlias);
  const REAL x = static_cast<REAL>(bounds.left) + .5f, y = static_cast<REAL>(bounds.top) + .5f;
  const REAL w = static_cast<REAL>(bounds.right - bounds.left) - 1;
  const REAL h = static_cast<REAL>(bounds.bottom - bounds.top) - 1;
  const REAL r = static_cast<REAL>(std::min<LONG>(radius, (bounds.bottom - bounds.top) / 2));
  GraphicsPath path;
  path.AddArc(x, y, r * 2, r * 2, 180, 90);
  path.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
  path.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
  path.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
  path.CloseFigure();
  SolidBrush brush(Color(255, GetRValue(fill), GetGValue(fill), GetBValue(fill)));
  Pen pen(Color(255, GetRValue(line), GetGValue(line), GetBValue(line)), 1);
  graphics.FillPath(&brush, &path);
  graphics.DrawPath(&pen, &path);
}
std::optional<int> ReadInteger(HWND edit) {
  wchar_t buffer[32]{};
  GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
  if (!buffer[0]) return std::nullopt;
  wchar_t* end = nullptr;
  const long value = std::wcstol(buffer, &end, 10);
  if (end == buffer || *end != L'\0' || value < INT_MIN || value > INT_MAX) return std::nullopt;
  return static_cast<int>(value);
}
bool Selected(int id) {
  if (id == IdModeMinutes) return g_app.mode == InputMode::Minutes;
  if (id == IdModeClock) return g_app.mode == InputMode::Clock;
  const auto value = ReadInteger(Control(IdMinutes));
  return g_app.mode == InputMode::Minutes && value &&
      ((id == IdPreset15 && *value == 15) || (id == IdPreset30 && *value == 30) ||
       (id == IdPreset60 && *value == 60));
}
void DrawButtonContent(const DRAWITEMSTRUCT& item) {
  const auto p = Colors();
  const int id = static_cast<int>(item.CtlID);
  const bool disabled = (item.itemState & ODS_DISABLED) != 0, selected = Selected(id);
  const bool primary = id == IdSchedule || id == IdCancel;
  const bool textOnly = id == IdSleepNow || id == IdReturn || id == IdChange;
  const bool tab = id == IdModeMinutes || id == IdModeClock;
  const bool hot = GetPropW(item.hwndItem, kHoverProperty) && !disabled;
  COLORREF fill = primary ? p.accent : selected ? p.selected : p.paper;
  COLORREF line = primary ? fill : selected ? p.accent : p.line;
  COLORREF text = primary ? RGB(255, 255, 255) : selected ? p.accent : p.muted;
  if (tab) { fill = selected ? p.paper : p.track; line = fill; text = selected ? p.ink : p.muted; }
  if (hot && !primary && !selected && !textOnly && !tab) fill = p.selected;
  if (p.highContrast) {
    fill = primary || selected ? p.accent : p.paper;
    line = p.ink;
    text = primary || selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : p.ink;
  }
  if (disabled) text = p.highContrast ? GetSysColor(COLOR_GRAYTEXT) : p.muted;
  FillRect(item.hDC, &item.rcItem, g_app.paperBrush);
  if (!textOnly) RoundedRect(item.hDC, item.rcItem,
      Scale(item.hwndItem, tab ? 5 : primary ? 8 : 16), fill, line);
  const UINT uiState = static_cast<UINT>(SendMessageW(g_app.window, WM_QUERYUISTATE, 0, 0));
  if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT) &&
      !(uiState & UISF_HIDEFOCUS)) {
    RECT focus = item.rcItem;
    InflateRect(&focus, -Scale(item.hwndItem, 3), -Scale(item.hwndItem, 3));
    // Keep keyboard focus visible with a ring that follows the button shape.
    RoundedRect(item.hDC, focus, Scale(item.hwndItem, tab ? 3 : primary ? 5 : 13),
        textOnly ? p.paper : fill, primary ? text : p.accent);
  }
  wchar_t label[128]{};
  GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
  // Keep the compact face uncluttered; the native label still supplies Alt shortcuts.
  if (wchar_t* shortcut = std::wcsrchr(label, L'&'); shortcut && shortcut > label && shortcut[-1] == L'(') {
    wchar_t* suffix = shortcut - 1;
    if (suffix > label && suffix[-1] == L' ') --suffix;
    *suffix = L'\0';
  }
  RECT bounds = item.rcItem;
  if (item.itemState & ODS_SELECTED) OffsetRect(&bounds, 0, Scale(item.hwndItem, 1));
  SetBkMode(item.hDC, TRANSPARENT);
  SetTextColor(item.hDC, text);
  const auto oldFont = SelectObject(item.hDC,
      reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)));
  UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
  if (uiState & UISF_HIDEACCEL) flags |= DT_HIDEPREFIX;
  DrawTextW(item.hDC, label, -1, &bounds, flags);
  SelectObject(item.hDC, oldFont);
}
void DrawButton(const DRAWITEMSTRUCT& item) {
  const int width = item.rcItem.right - item.rcItem.left;
  const int height = item.rcItem.bottom - item.rcItem.top;
  if (width <= 0 || height <= 0) return;
  HDC memory = CreateCompatibleDC(item.hDC);
  HBITMAP bitmap = memory ? CreateCompatibleBitmap(item.hDC, width, height) : nullptr;
  if (!bitmap) {
    if (memory) DeleteDC(memory);
    DrawButtonContent(item);
    return;
  }
  const auto previous = SelectObject(memory, bitmap);
  DRAWITEMSTRUCT buffered = item;
  buffered.hDC = memory;
  buffered.rcItem = {0, 0, width, height};
  DrawButtonContent(buffered);
  // Present the completed button in one copy, without an exposed blank frame.
  BitBlt(item.hDC, item.rcItem.left, item.rcItem.top, width, height, memory, 0, 0, SRCCOPY);
  SelectObject(memory, previous);
  DeleteObject(bitmap);
  DeleteDC(memory);
}
std::wstring FormatTarget(system_clock::time_point target) {
  const auto nowTime = system_clock::to_time_t(system_clock::now());
  const auto value = system_clock::to_time_t(target);
  std::tm nowLocal{}, local{};
  localtime_s(&nowLocal, &nowTime);
  localtime_s(&local, &value);
  wchar_t buffer[80]{};
  const bool today = nowLocal.tm_year == local.tm_year && nowLocal.tm_yday == local.tm_yday;
  std::wcsftime(buffer, std::size(buffer), L"%m/%d %H:%M", &local);
  return std::wstring(today ? L"今日 " : L"明日 ") + buffer;
}
std::wstring FormatRemaining(system_clock::duration duration, bool immediate) {
  const auto total = std::max<long long>(0, ceil<seconds>(duration).count());
  if (immediate) return std::to_wstring(total);
  wchar_t buffer[80]{};
  if (total >= 3600) swprintf_s(buffer, L"%02lld:%02lld:%02lld", total / 3600,
                               (total % 3600) / 60, total % 60);
  else swprintf_s(buffer, L"%02lld:%02lld", total / 60, total % 60);
  return buffer;
}
std::optional<system_clock::time_point> LocalTarget(int hour, int minute) {
  const std::time_t nowTime = std::time(nullptr);
  std::tm nowLocal{};
  localtime_s(&nowLocal, &nowTime);
  const auto localNow = local_days{year{nowLocal.tm_year + 1900} /
      month{static_cast<unsigned>(nowLocal.tm_mon + 1)} /
      day{static_cast<unsigned>(nowLocal.tm_mday)}} + hours{nowLocal.tm_hour} +
      minutes{nowLocal.tm_min} + seconds{nowLocal.tm_sec};
  const auto target = mouneru::TimerManager::NextLocalClockTime(hour, minute, localNow);
  const auto targetDay = floor<days>(target);
  const year_month_day ymd{targetDay};
  const hh_mm_ss clock{target - targetDay};
  std::tm targetLocal{};
  targetLocal.tm_year = static_cast<int>(ymd.year()) - 1900;
  targetLocal.tm_mon = static_cast<int>(static_cast<unsigned>(ymd.month())) - 1;
  targetLocal.tm_mday = static_cast<int>(static_cast<unsigned>(ymd.day()));
  targetLocal.tm_hour = static_cast<int>(clock.hours().count());
  targetLocal.tm_min = static_cast<int>(clock.minutes().count());
  targetLocal.tm_isdst = -1;
  const auto expected = targetLocal;
  const std::time_t converted = std::mktime(&targetLocal);
  if (converted == static_cast<std::time_t>(-1)) return std::nullopt;
  std::tm verified{};
  localtime_s(&verified, &converted);
  if (verified.tm_year != expected.tm_year || verified.tm_mon != expected.tm_mon ||
      verified.tm_mday != expected.tm_mday || verified.tm_hour != expected.tm_hour ||
      verified.tm_min != expected.tm_min) return std::nullopt;
  return system_clock::from_time_t(converted);
}
std::optional<system_clock::time_point> InputTarget() {
  if (g_app.mode == InputMode::Minutes) {
    const auto value = ReadInteger(Control(IdMinutes));
    if (!value || *value < 1 || *value > 1440) return std::nullopt;
    return system_clock::now() + minutes{*value};
  }
  const auto hour = ReadInteger(Control(IdHour)), minute = ReadInteger(Control(IdMinute));
  if (!hour || !minute || *hour < 0 || *hour > 23 || *minute < 0 || *minute > 59) return std::nullopt;
  return LocalTarget(*hour, *minute);
}
void SetShown(HWND control, bool shown) {
  if (((GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0) != shown)
    ShowWindow(control, shown ? SW_SHOW : SW_HIDE);
}
bool EditorVisible() {
  return g_app.timer.state() == mouneru::TimerState::Idle ||
         (g_app.editing && g_app.timer.state() == mouneru::TimerState::Scheduled);
}
void RefreshUi() {
  if (g_app.refreshing || !g_app.window) return;
  g_app.refreshing = true;
  const auto state = g_app.timer.state();
  const bool immediate = state == mouneru::TimerState::ImmediateCountdown;
  const bool executing = state == mouneru::TimerState::Executing;
  if (immediate || executing) g_app.editing = false;
  const bool editor = EditorVisible();
  for (const auto& layout : g_app.layouts) {
    bool shown = layout.region == Region::Always || (layout.region == Region::Editor && editor) ||
        (layout.region == Region::Minutes && editor && g_app.mode == InputMode::Minutes) ||
        (layout.region == Region::Clock && editor && g_app.mode == InputMode::Clock) ||
        (layout.region == Region::Active && !editor);
    const int id = GetDlgCtrlID(layout.window);
    if (id == IdSleepNow) shown = editor && !g_app.editing;
    if (id == IdReturn) shown = editor && g_app.editing;
    if (id == IdChange) shown = !editor && state == mouneru::TimerState::Scheduled;
    SetShown(layout.window, shown);
    const bool enabled = !executing || layout.region == Region::Always;
    if ((IsWindowEnabled(layout.window) != FALSE) != enabled) EnableWindow(layout.window, enabled);
  }
  const auto selectedName = PowerName(g_app.selectedPower);
  const auto reservedName = PowerName(g_app.reservedPower);
  SetControlText(Control(IdSchedule), g_app.editing ? L"予約を変更する (&R)" : L"この時間に" + selectedName + L" (&R)");
  SetControlText(Control(IdSleepNow), L"今すぐ" + selectedName + L"（5秒後）(&S)");
  for (int id : {IdPowerSleep, IdPowerShutdown}) {
    const bool checked = (id == IdPowerSleep) == (g_app.selectedPower == mouneru::PowerAction::Sleep);
    const LRESULT expected = checked ? BST_CHECKED : BST_UNCHECKED;
    if (SendMessageW(Control(id), BM_GETCHECK, 0, 0) != expected)
      SendMessageW(Control(id), BM_SETCHECK, static_cast<WPARAM>(expected), 0);
  }
  if (editor) {
    const auto value = ReadInteger(Control(IdMinutes));
    const HFONT numberFont = g_app.fonts[static_cast<int>(value && *value >= 1000 ? FontRole::Clock : FontRole::Number)];
    if (reinterpret_cast<HFONT>(SendMessageW(Control(IdMinutes), WM_GETFONT, 0, 0)) != numberFont)
      SendMessageW(Control(IdMinutes), WM_SETFONT, reinterpret_cast<WPARAM>(numberFont), TRUE);
    const auto target = InputTarget();
    SetControlText(Control(IdEstimate), target ? FormatTarget(*target) + L" に" + selectedName : L"時間を入力してください");
  } else {
    SetControlText(Control(IdActiveHeading), executing ? reservedName + L"を実行しています…"
        : immediate ? reservedName + L"まで、あと" : reservedName + L"を予約しました");
    SetControlText(Control(IdCountdown), executing ? L"…"
        : FormatRemaining(g_app.timer.target() - system_clock::now(), immediate));
    SetControlText(Control(IdActiveTarget), executing ? L"Windowsに" + reservedName + L"を要求しています"
        : immediate ? L"秒後に" + reservedName + L"します\r\n今なら取り消せます。"
        : FormatTarget(g_app.timer.target()) + L" に" + reservedName + L"\r\nそれまで、ゆっくりどうぞ。");
    SetControlText(Control(IdCancel), immediate ? reservedName + L"を取り消す (&C)" : L"予約を取り消す (&C)");
  }
  int selectionIndex = 0;
  for (int id : {IdModeMinutes, IdModeClock, IdPreset15, IdPreset30, IdPreset60}) {
    const bool selected = Selected(id);
    if (g_app.selectionReady && g_app.selections[selectionIndex] == selected) {
      ++selectionIndex;
      continue;
    }
    g_app.selections[selectionIndex++] = selected;
    wchar_t name[80]{};
    GetWindowTextW(Control(id), name, static_cast<int>(std::size(name)));
    SetAccessibleName(Control(id), (std::wstring(name) + (selected ? L"、選択中" : L"")).c_str());
    InvalidateRect(Control(id), nullptr, FALSE);
  }
  g_app.selectionReady = true;
  HWND focus = GetFocus();
  if (focus && GetParent(focus) == g_app.window &&
      (!(GetWindowLongPtrW(focus, GWL_STYLE) & WS_VISIBLE) || !IsWindowEnabled(focus)))
    SetFocus(Control(editor ? IdSchedule : IdCancel));
  g_app.refreshing = false;
}
void ClearErrors() {
  SetControlText(Control(IdInputError), L"");
  SetControlText(Control(IdError), L"");
}
bool ConfirmReplacement(system_clock::time_point target, bool immediate) {
  if (!g_app.timer.HasActiveTimer()) return true;
  if (g_app.confirmationOpen) return false;
  const std::wstring text = L"現在の予約：" + FormatTarget(g_app.timer.target()) + L" に" + PowerName(g_app.reservedPower) + L"\n新しい予約：" +
      (immediate ? std::wstring(L"5秒後") : FormatTarget(target)) + L" に" + PowerName(g_app.selectedPower) + L"\n\n現在の予約を置き換えますか？";
  g_app.confirmationOpen = true;
  const int answer = MessageBoxW(g_app.window, text.c_str(), L"予約の変更",
                                MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
  g_app.confirmationOpen = false;
  return answer == IDOK && IsWindow(g_app.window) && g_app.timer.state() != mouneru::TimerState::Executing;
}
void StartReviewedReservation(system_clock::time_point reviewedTarget) {
  const auto target = g_app.mode == InputMode::Minutes ? InputTarget()
      : std::optional<system_clock::time_point>{reviewedTarget};
  if (!target || *target <= system_clock::now()) {
    SetControlText(Control(IdInputError), L"確認中に時刻を過ぎました。選び直してください。");
    SetFocus(Control(g_app.mode == InputMode::Minutes ? IdMinutes : IdHour));
    return;
  }
  if (g_app.timer.ScheduleAt(*target, system_clock::now())) {
    g_app.reservedPower = g_app.selectedPower;
    g_app.editing = false;
    ClearErrors();
    RefreshUi();
    InvalidateRect(g_app.window, nullptr, TRUE);
    SetFocus(Control(IdCancel));
  }
}
void ScheduleFromInputs() {
  if (!EditorVisible() || g_app.confirmationOpen) return;
  auto target = InputTarget();
  if (!target) {
    SetControlText(Control(IdInputError), g_app.mode == InputMode::Minutes
        ? L"1～1,440の整数で入力してください。" : L"0～23時、0～59分で入力してください。");
    const auto h = ReadInteger(Control(IdHour));
    SetFocus(Control(g_app.mode == InputMode::Minutes ? IdMinutes
        : !h || *h < 0 || *h > 23 ? IdHour : IdMinute));
    return;
  }
  if (!ConfirmReplacement(*target, false)) return;
  // Keep the absolute clock time that was reviewed; only relative delays restart now.
  StartReviewedReservation(*target);
}
void HandleCommand(int id) {
  if (g_app.timer.state() == mouneru::TimerState::Executing || g_app.confirmationOpen) return;
  const bool wasEditorVisible = EditorVisible();
  const InputMode previousMode = g_app.mode;
  switch (id) {
    case IdPowerSleep:
    case IdPowerShutdown:
      if (!EditorVisible()) return;
      g_app.selectedPower = id == IdPowerSleep ? mouneru::PowerAction::Sleep : mouneru::PowerAction::Shutdown;
      ClearErrors();
      break;
    case IdModeMinutes:
    case IdModeClock:
      if (!EditorVisible()) return;
      g_app.mode = id == IdModeMinutes ? InputMode::Minutes : InputMode::Clock;
      ClearErrors();
      break;
    case IdPreset15:
    case IdPreset30:
    case IdPreset60:
      if (!EditorVisible()) return;
      g_app.mode = InputMode::Minutes;
      SetControlText(Control(IdMinutes), id == IdPreset15 ? L"15" : id == IdPreset30 ? L"30" : L"60");
      ClearErrors();
      break;
    case IdDecrease:
    case IdIncrease: {
      if (!EditorVisible()) return;
      const int value = std::clamp(ReadInteger(Control(IdMinutes)).value_or(30), 1, 1440);
      SetControlText(Control(IdMinutes), std::to_wstring(std::clamp(value + (id == IdIncrease ? 5 : -5), 1, 1440)));
      ClearErrors();
      break;
    }
    case IdSchedule: ScheduleFromInputs(); return;
    case IdSleepNow:
      if (!EditorVisible()) return;
      if (!ConfirmReplacement(system_clock::now() + seconds{5}, true)) return;
      if (!g_app.timer.StartImmediate(system_clock::now())) return;
      g_app.reservedPower = g_app.selectedPower;
      g_app.editing = false;
      ClearErrors();
      break;
    case IdCancel:
      g_app.timer.Cancel();
      g_app.editing = false;
      ClearErrors();
      break;
    case IdChange:
      if (g_app.timer.state() != mouneru::TimerState::Scheduled) return;
      g_app.editing = true;
      g_app.selectedPower = g_app.reservedPower;
      ClearErrors();
      break;
    case IdReturn: g_app.editing = false; ClearErrors(); break;
    default: return;
  }
  RefreshUi();
  if (wasEditorVisible != EditorVisible() || previousMode != g_app.mode)
    InvalidateRect(g_app.window, nullptr, TRUE);
  if (id == IdIncrease || id == IdDecrease || id == IdPreset15 || id == IdPreset30 || id == IdPreset60)
    UpdateWindow(Control(IdMinutes));
  if (id == IdSleepNow) SetFocus(Control(IdCancel));
  if (id == IdChange) SetFocus(Control(g_app.mode == InputMode::Minutes ? IdMinutes : IdHour));
}
DWORD WINAPI PowerWorkerProc(LPVOID parameter) {
  const auto request = *static_cast<const AppState::WorkerRequest*>(parameter);
  const auto result = mouneru::RequestSystemPower(request.action);
  PostMessageW(request.window, kPowerResultMessage, result.succeeded ? TRUE : FALSE, static_cast<LPARAM>(result.errorCode));
  return 0;
}
void ExecutePower() {
  if (g_app.powerRequestInFlight) return;
  g_app.powerRequestInFlight = true;
  RefreshUi();
  g_app.workerRequest = {g_app.window, g_app.reservedPower};
  const HANDLE worker = CreateThread(nullptr, 0, PowerWorkerProc, &g_app.workerRequest, 0, nullptr);
  if (!worker) {
    g_app.powerRequestInFlight = false;
    g_app.timer.CompleteExecution();
    SetControlText(Control(IdError), PowerName(g_app.reservedPower) + L"処理を開始できませんでした。");
    RefreshUi();
    return;
  }
  CloseHandle(worker);
}
bool CreateUi(HWND window) {
  g_app.window = window;
  g_app.refreshing = true;
  RecreateFonts();
  RecreateBrush();
  const DWORD button = BS_OWNERDRAW | WS_TABSTOP;
  const DWORD edit = ES_NUMBER | ES_CENTER | ES_AUTOHSCROLL | WS_TABSTOP;
  AddControl(L"STATIC", L"mouneru", SS_LEFT, IdBrand, 54, 20, 130, 22, FontRole::Brand, Region::Always);
  AddControl(L"STATIC", L"もう寝る", SS_RIGHT, IdBrandHint, 240, 23, 112, 18, FontRole::Small, Region::Always);
  AddControl(L"BUTTON", L"何分後に (&M)", button | WS_GROUP, IdModeMinutes, 32, 62, 154, 26, FontRole::Small, Region::Editor);
  AddControl(L"BUTTON", L"時刻を指定 (&T)", button, IdModeClock, 194, 62, 154, 26, FontRole::Small, Region::Editor);
  AddControl(L"BUTTON", L"−", button, IdDecrease, 77, 127, 28, 28, FontRole::Body, Region::Minutes);
  AddControl(L"EDIT", L"30", edit, IdMinutes, 117, 109, 94, 62, FontRole::Number, Region::Minutes);
  AddControl(L"STATIC", L"分後", SS_CENTER, IdMinutesUnit, 216, 134, 40, 22, FontRole::Body, Region::Minutes);
  AddControl(L"BUTTON", L"＋", button, IdIncrease, 273, 127, 28, 28, FontRole::Body, Region::Minutes);
  AddControl(L"EDIT", L"23", edit, IdHour, 96, 114, 78, 54, FontRole::Clock, Region::Clock);
  AddControl(L"STATIC", L":", SS_CENTER, IdColon, 178, 114, 24, 54, FontRole::Clock, Region::Clock);
  AddControl(L"EDIT", L"00", edit, IdMinute, 206, 114, 78, 54, FontRole::Clock, Region::Clock);
  AddControl(L"STATIC", L"", SS_CENTER, IdEstimate, 28, 180, 324, 19, FontRole::Small, Region::Editor);
  AddControl(L"STATIC", L"", SS_CENTER, IdInputError, 28, 206, 324, 18, FontRole::Small, Region::Editor);
  AddControl(L"BUTTON", L"15分", button, IdPreset15, 28, 232, 102, 32, FontRole::Body, Region::Editor);
  AddControl(L"BUTTON", L"30分", button, IdPreset30, 139, 232, 102, 32, FontRole::Body, Region::Editor);
  AddControl(L"BUTTON", L"60分", button, IdPreset60, 250, 232, 102, 32, FontRole::Body, Region::Editor);
  AddControl(L"BUTTON", L"スリープ (&L)", BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, IdPowerSleep, 78, 274, 100, 28, FontRole::Small, Region::Editor);
  AddControl(L"BUTTON", L"シャットダウン (&D)", BS_AUTORADIOBUTTON, IdPowerShutdown, 190, 274, 132, 28, FontRole::Small, Region::Editor);
  AddControl(L"BUTTON", L"この時間にスリープ (&R)", button | WS_GROUP, IdSchedule, 28, 314, 324, 42, FontRole::Body, Region::Editor);
  AddControl(L"BUTTON", L"今すぐスリープ（5秒後）(&S)", button, IdSleepNow, 28, 366, 324, 28, FontRole::Small, Region::Editor);
  AddControl(L"BUTTON", L"予約は継続中 · 残り時間に戻る (&B)", button, IdReturn, 28, 366, 324, 28, FontRole::Small, Region::Editor);
  AddControl(L"STATIC", L"", SS_CENTER, IdActiveHeading, 28, 86, 324, 22, FontRole::Body, Region::Active);
  AddControl(L"STATIC", L"", SS_CENTER, IdCountdown, 28, 126, 324, 67, FontRole::Countdown, Region::Active);
  AddControl(L"STATIC", L"", SS_CENTER, IdActiveTarget, 28, 211, 324, 44, FontRole::Body, Region::Active);
  AddControl(L"BUTTON", L"予約を取り消す (&C)", button, IdCancel, 28, 280, 324, 42, FontRole::Body, Region::Active);
  AddControl(L"BUTTON", L"時間を変更する (&E)", button, IdChange, 28, 332, 324, 28, FontRole::Small, Region::Active);
  AddControl(L"STATIC", L"アプリを終了すると予約は取り消されます", SS_CENTER, IdFooter, 16, 410, 348, 16, FontRole::Small, Region::Always);
  AddControl(L"STATIC", L"", SS_CENTER, IdError, 28, 42, 324, 16, FontRole::Small, Region::Always);
  AddControl(L"STATIC", L"", SS_OWNERDRAW, IdLogo, 28, 20, 20, 22, FontRole::Brand, Region::Always);
  SetAccessibleName(Control(IdDecrease), L"5分減らす");
  SetAccessibleName(Control(IdIncrease), L"5分増やす");
  SetAccessibleName(Control(IdMinutes), L"実行までの分数、1から1440");
  SetAccessibleName(Control(IdHour), L"予約する時、0から23、24時間表記");
  SetAccessibleName(Control(IdMinute), L"予約する分、0から59");
  SendMessageW(Control(IdMinutes), EM_SETLIMITTEXT, 5, 0);
  SendMessageW(Control(IdHour), EM_SETLIMITTEXT, 2, 0);
  SendMessageW(Control(IdMinute), EM_SETLIMITTEXT, 2, 0);
  g_app.refreshing = false;
  RefreshUi();
  return g_app.layouts.size() == 29 && g_app.fonts[0] && g_app.paperBrush;
}
void DrawLogo(const DRAWITEMSTRUCT& item) {
  const auto p = Colors();
  using namespace Gdiplus;
    FillRect(item.hDC, &item.rcItem, g_app.paperBrush);
    Graphics graphics(item.hDC);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush ink(Color(255, GetRValue(p.ink), GetGValue(p.ink), GetBValue(p.ink)));
    SolidBrush paper(Color(255, GetRValue(p.paper), GetGValue(p.paper), GetBValue(p.paper)));
    const REAL x = static_cast<REAL>(Scale(g_app.window, 1)), y = static_cast<REAL>(Scale(g_app.window, 1));
    const REAL size = static_cast<REAL>(Scale(g_app.window, 19));
    graphics.FillEllipse(&ink, x, y, size, size);
    graphics.FillEllipse(&paper, x + size * .35f, y - size * .17f, size * .88f, size * .88f);
}
void PaintDecoration(HDC dc) {
  const auto p = Colors();
  if (!EditorVisible()) return;
  RECT tabs{Scale(g_app.window, 28), Scale(g_app.window, 58), Scale(g_app.window, 352), Scale(g_app.window, 92)};
  RoundedRect(dc, tabs, Scale(g_app.window, 8), p.track, p.highContrast ? p.line : p.track);
  HPEN pen = CreatePen(PS_SOLID, 1, p.line);
  const auto old = SelectObject(dc, pen);
  for (int x : g_app.mode == InputMode::Minutes ? std::vector<int>{117} : std::vector<int>{96, 206}) {
    MoveToEx(dc, Scale(g_app.window, x), Scale(g_app.window, 173), nullptr);
    LineTo(dc, Scale(g_app.window, x + (g_app.mode == InputMode::Minutes ? 94 : 78)), Scale(g_app.window, 173));
  }
  SelectObject(dc, old);
  DeleteObject(pen);
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE:
      if (!CreateUi(window) || !SetTimer(window, kTickTimer, kTickIntervalMs, nullptr)) return -1;
      return 0;
    case WM_DPICHANGED: {
      const auto suggested = reinterpret_cast<const RECT*>(lParam);
      SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
          suggested->bottom - suggested->top, SWP_NOACTIVATE | SWP_NOZORDER);
      Relayout();
      return 0;
    }
    case WM_TIMER:
      if (wParam == kTickTimer) {
        if (g_app.timer.Tick(system_clock::now()) == mouneru::TimerAction::RequestSleep) ExecutePower();
        else RefreshUi();
      }
      return 0;
    case kPowerResultMessage:
      g_app.powerRequestInFlight = false;
      if (wParam != FALSE) DestroyWindow(window);
      else {
        g_app.timer.CompleteExecution();
        g_app.editing = false;
        SetControlText(Control(IdError), PowerName(g_app.reservedPower) + L"できませんでした（エラー " +
            std::to_wstring(static_cast<DWORD>(lParam)) + L"）");
        RefreshUi();
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    case WM_POWERBROADCAST:
      if (wParam == PBT_APMSUSPEND && g_app.powerRequestInFlight &&
          g_app.reservedPower == mouneru::PowerAction::Sleep) DestroyWindow(window);
      return TRUE;
    case WM_DRAWITEM:
      if (wParam == IdLogo) DrawLogo(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
      else DrawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
      return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT: {
      const auto p = Colors();
      HDC dc = reinterpret_cast<HDC>(wParam);
      const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
      SetTextColor(dc, id == IdInputError || id == IdError ? p.error
          : id == IdBrand || id == IdMinutes || id == IdHour || id == IdMinute || id == IdCountdown ? p.ink : p.muted);
      SetBkColor(dc, p.paper);
      return reinterpret_cast<INT_PTR>(g_app.paperBrush);
    }
    case WM_ERASEBKGND: {
      RECT client{};
      GetClientRect(window, &client);
      FillRect(reinterpret_cast<HDC>(wParam), &client, g_app.paperBrush);
      return TRUE;
    }
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      PaintDecoration(dc);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_PRINTCLIENT:
      PaintDecoration(reinterpret_cast<HDC>(wParam));
      return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
      RecreateBrush();
      Relayout();
      for (const auto& layout : g_app.layouts) InvalidateRect(layout.window, nullptr, TRUE);
      return 0;
    case WM_COMMAND:
      if (HIWORD(wParam) == EN_CHANGE && !g_app.refreshing) {
        SetControlText(Control(IdInputError), L"");
        RefreshUi();
      } else if (HIWORD(wParam) == BN_CLICKED) HandleCommand(LOWORD(wParam));
      return 0;
    case WM_CLOSE:
      if (g_app.powerRequestInFlight || g_app.confirmationOpen) return 0;
      if (g_app.timer.HasActiveTimer()) {
        g_app.confirmationOpen = true;
        const std::wstring text = L"予約を取り消して終了しますか？\n\n実行予定：" + FormatTarget(g_app.timer.target());
        const int answer = MessageBoxW(window, text.c_str(), L"もう寝るを終了",
                                      MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
        g_app.confirmationOpen = false;
        if (answer != IDOK || g_app.powerRequestInFlight || !IsWindow(window)) return 0;
      }
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      KillTimer(window, kTickTimer);
      for (HFONT& font : g_app.fonts) { if (font) DeleteObject(font); font = nullptr; }
      if (g_app.paperBrush) DeleteObject(g_app.paperBrush);
      g_app.paperBrush = nullptr;
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}
HWND CreateAppWindow(HINSTANCE instance) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
  wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
  wc.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&wc)) return nullptr;
  constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
  const UINT dpi = GetDpiForSystem();
  RECT size{0, 0, MulDiv(kClientWidth, static_cast<int>(dpi), 96), MulDiv(kClientHeight, static_cast<int>(dpi), 96)};
  AdjustWindowRectExForDpi(&size, style, FALSE, 0, dpi);
  HWND window = CreateWindowExW(0, kWindowClass, L"もう寝る · mouneru", style, CW_USEDEFAULT, CW_USEDEFAULT,
      size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, nullptr);
  if (window) {
    constexpr DWORD round = 2;
    (void)DwmSetWindowAttribute(window, 33, &round, sizeof(round));
    const auto p = Colors();
    (void)DwmSetWindowAttribute(window, 35, &p.paper, sizeof(p.paper));
    (void)DwmSetWindowAttribute(window, 36, &p.ink, sizeof(p.ink));
  }
  return window;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  Gdiplus::GdiplusStartupInput gdiplusInput;
  ULONG_PTR gdiplusToken{};
  if (!InitCommonControlsEx(&controls) || Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
    if (SUCCEEDED(com)) CoUninitialize();
    return 1;
  }
  HWND window = CreateAppWindow(instance);
  if (!window) {
    Gdiplus::GdiplusShutdown(gdiplusToken);
    if (SUCCEEDED(com)) CoUninitialize();
    return 1;
  }
  ShowWindow(window, showCommand);
  UpdateWindow(window);
  MSG message{};
  int result{};
  while ((result = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  Gdiplus::GdiplusShutdown(gdiplusToken);
  if (SUCCEEDED(com)) CoUninitialize();
  UnregisterClassW(kWindowClass, instance);
  return result < 0 ? 1 : static_cast<int>(message.wParam);
}
