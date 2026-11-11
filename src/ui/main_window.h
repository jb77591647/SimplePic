#pragma once
#include <windows.h>
#include "pipeline/pipeline.h"

namespace sp {

HWND create_main_window(HINSTANCE hInst);
Params current_params(); // 由 main_window.cpp 维护

} // namespace sp
