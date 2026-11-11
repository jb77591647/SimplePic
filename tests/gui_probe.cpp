// 模拟主程序进程环境复现 PDF 渲染失败：
// /SUBSYSTEM:WINDOWS + 主线程 OleInitialize(STA, 拖放宿主) + 消息泵 +
// 工作线程按主程序节奏连续 5 次 load_image（预览 probe / 预览解码 / 实测队列 / 管线）
// 结果写 exe 同目录 gui_probe.log
#include "codec/codec.h"
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <thread>
#include <string>

using namespace sp;

static void logf(const char* tag, const char* msg) {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    std::wstring p = dir;
    size_t s = p.find_last_of(L'\\');
    if (s != std::wstring::npos) p = p.substr(0, s + 1);
    p += L"gui_probe.log";
    FILE* f = nullptr;
    _wfopen_s(&f, p.c_str(), L"a");
    if (!f) return;
    fprintf(f, "[%s] %s\n", tag, msg ? msg : "");
    fclose(f);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    std::wstring logp = dir;
    { size_t s = logp.find_last_of(L'\\'); if (s != std::wstring::npos) logp = logp.substr(0, s + 1); }
    logp += L"gui_probe.log";
    DeleteFileW(logp.c_str());

    HRESULT ole = OleInitialize(nullptr);
    { std::string m = "ole=0x" + std::to_string((unsigned long)ole); logf("main", m.c_str()); }

    // 哑窗口 + 消息泵（GUI 宿主最低条件）
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"GuiProbeHost"; wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    HWND w = CreateWindowW(L"GuiProbeHost", L"", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100,
                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(w, SW_HIDE);

    std::wstring pdf = logp; // 同目录
    { size_t s = pdf.find_last_of(L'\\'); pdf = pdf.substr(0, s + 1); pdf += L"test_real.pdf"; }

    std::thread worker([pdf] {
        for (int i = 0; i < 5; ++i) {
            Image img;
            std::string err;
            bool ok = load_image(pdf, img, &err);
            std::string m = "run" + std::to_string(i) + " ok=" + std::to_string(ok)
                          + (ok ? (" size=" + std::to_string(img.w) + "x" + std::to_string(img.h))
                                : (" err=" + err));
            logf("worker", m.c_str());
            Sleep(300);
        }
        logf("worker", "done");
    });

    DWORD start = GetTickCount();
    MSG msg;
    while (GetTickCount() - start < 20000) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(30);
    }
    if (worker.joinable()) worker.join(); // 不 join 也行，日志已足够
    logf("main", "exit");
    OleUninitialize();
    return 0;
}
