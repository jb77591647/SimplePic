#include <windows.h>
#include <string>
#include <algorithm>
#include "ui/main_window.h"

// 单实例：同一位置（exe 全路径相同）只允许运行一个实例。
// 互斥量名取 exe 全路径小写后的 FNV-1a 哈希——拷贝到其他目录的副本互不冲突；
// 重复启动时把已有实例窗口拉到前台并退出本次启动。
static bool acquire_single_instance() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    std::transform(p.begin(), p.end(), p.begin(), ::towlower);
    DWORD h = 2166136261u;
    for (wchar_t c : p) {
        h ^= (DWORD)c;
        h *= 16777619u;
    }
    wchar_t name[64];
    swprintf(name, 64, L"SimplePic_Single_%08X", h);
    HANDLE m = CreateMutexW(nullptr, TRUE, name);
    if (!m) return true; // 创建失败不阻止运行（异常环境兜底）
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        HWND prev = FindWindowW(L"SimplePicMain", nullptr);
        if (prev) { // 已有实例：置顶还原，避免"点没反应"的困惑
            if (IsIconic(prev)) ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
        }
        return false;
    }
    return true; // 互斥量持有至进程退出，内核回收
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    (void)nCmdShow;
    if (!acquire_single_instance()) return 0;
    HWND hwnd = sp::create_main_window(hInst);
    if (!hwnd) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    // CRT 静态析构在此环境会挂起（后台线程已全退出，窗口已销毁）——直接由内核收尾
    ExitProcess((UINT)msg.wParam);
}
