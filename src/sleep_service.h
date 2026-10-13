#pragma once

#include <windows.h>

namespace mouneru {

enum class PowerAction { Sleep, Shutdown };

struct PowerResult {
  bool succeeded;
  DWORD errorCode;
};

[[nodiscard]] PowerResult RequestSystemPower(PowerAction action) noexcept;

}  // namespace mouneru
