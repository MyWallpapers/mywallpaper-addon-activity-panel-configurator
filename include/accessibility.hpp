#pragma once
#include "model.hpp"
#include <memory>
#include <mutex>
#include <oleacc.h>
#include <windows.h>
namespace activity {
inline constexpr UINT accessibleAction = WM_APP + 2, accessibleFocus = WM_APP + 3;
struct AccessibleState {
    std::mutex mutex;
    HWND window = nullptr;
    std::string name;
    std::vector<std::string> labels;
    std::vector<Rect> locations;
    int focus = -1;
    uint32_t revision = 0;
};
LRESULT AccessibleObject(WPARAM request, const std::shared_ptr<AccessibleState> &state);
} // namespace activity
