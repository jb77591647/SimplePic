// 管线回归测试：处理完成后 running() 必须归位（清空按钮的开关条件）
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../src/pipeline/pipeline.h"
#include <cstdio>
#include <chrono>

using namespace sp;

int main() {
    // 生成两个测试 PNG（1x1）
    const unsigned char png[] = {
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
        0x89,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,0x78,0xDA,0x63,0xFC,0xCF,0xC0,0xF0,
        0x1F,0x00,0x05,0x00,0x02,0x8B,0x6C,0x2A,0x38,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,
        0x44,0xAE,0x42,0x60,0x82 };
    const wchar_t* dir = L"C:\\Users\\bin\\AppData\\Local\\Temp\\sp_pipe_test";
    CreateDirectoryW(dir, nullptr);
    std::vector<FileItem> files;
    for (int i = 0; i < 2; ++i) {
        wchar_t p[256]; swprintf(p, 256, L"%s\\t%d.png", dir, i);
        HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w; WriteFile(h, png, sizeof png, &w, nullptr); CloseHandle(h);
        FileItem f; f.path = p; files.push_back(f);
    }

    Pipeline pipe;
    Params p; p.module_id = 1; // 证件照
    pipe.start(std::move(files), {}, p, std::wstring(dir) + L"\\out",
               [](int, ItemState, const ItemResult&) {});

    // 等待管线收尾，最多 10 秒
    auto t0 = std::chrono::steady_clock::now();
    while (pipe.running()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10)) break;
        Sleep(100);
    }
    bool ok = !pipe.running();
    printf(ok ? "PIPELINE_OK: running() == false after drain\n"
              : "PIPELINE_FAIL: running() still true after 10s\n");
    return ok ? 0 : 1;
}
