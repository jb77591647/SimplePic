// PDF 解码回归测试：走 sp::load_image（Windows.Data.Pdf 渲染 + DCT 回退）
// 用法：pdf_test.exe [真实PDF路径 ...]；无参数时测试 tests/out 下的两个内置样例
#include "codec/codec.h"
#include <windows.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <filesystem>
#include <vector>
#include <thread>
#include <cstring>
#include <cstdlib>

using namespace sp;

// 并发模式：--par N file.pdf  → N 个线程同时 load_image，模拟主程序
// probe_image(预览线程) 与 load_image(管线线程) 并发渲染同一 PDF 的场景
static int run_parallel(const std::wstring& p, int n) {
    std::vector<std::thread> ts;
    std::vector<std::string> errs(n);
    std::vector<int> oks(n, 0);
    for (int k = 0; k < n; ++k)
        ts.emplace_back([&, k] {
            Image img;
            if (load_image(p, img, &errs[k])) oks[k] = 1;
        });
    for (auto& t : ts) t.join();
    int failed = 0;
    for (int k = 0; k < n; ++k) {
        wprintf(L"[T%d] %s %hs\n", k, oks[k] ? L"PASS" : L"FAIL",
                oks[k] ? "" : errs[k].c_str());
        if (!oks[k]) ++failed;
    }
    return failed;
}

int main(int argc, char** argv) {
    _setmode(_fileno(stdout), _O_U16TEXT); // 重定向下 wprintf 必须设置模式
    namespace fs = std::filesystem;
    // 并发模式：--par N <pdf路径>
    if (argc >= 3 && strcmp(argv[1], "--par") == 0) {
        int n = atoi(argv[2]);
        int nu = MultiByteToWideChar(CP_UTF8, 0, argv[3], -1, nullptr, 0);
        std::wstring w(nu, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, argv[3], -1, w.data(), nu);
        w.pop_back();
        return run_parallel(w, n);
    }
    // 支持命令行传入真实 PDF 路径：pdf_test.exe [file1.pdf file2.pdf ...]
    std::vector<std::wstring> paths;
    for (int i = 1; i < argc; ++i) {
        int n = MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, w.data(), n);
        w.pop_back();
        paths.push_back(w);
    }
    if (paths.empty()) {
        fs::path base = fs::path(__FILE__).parent_path() / "out";
        paths.push_back((base / L"vector_text.pdf").wstring());
        paths.push_back((base / L"dct_image.pdf").wstring());
    }
    int failed = 0;
    for (const auto& p : paths) {
        Image img;
        std::string err;
        if (!load_image(p, img, &err)) {
            wprintf(L"[FAIL] %s: %hs\n", p.c_str(), err.c_str());
            ++failed;
            continue;
        }
        auto jpg = encode_jpg(img, 90);
        fs::path outp = fs::path(p).parent_path() /
                        (fs::path(p).stem().wstring() + L"_out.jpg");
        bool ok = write_file(outp.wstring(), jpg);
        wprintf(L"[%s] %s -> %dx%d, out %zu bytes (write=%d)\n",
                ok ? L"PASS" : L"WFAIL", p.c_str(), img.w, img.h, jpg.size(), ok);
        if (img.w <= 0 || img.h <= 0 || jpg.empty()) ++failed;
    }
    return failed ? 1 : 0;
}
