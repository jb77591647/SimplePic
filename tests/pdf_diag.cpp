// WinRT PDF 渲染诊断：独立调用 Windows.Data.Pdf，打印每一步 HRESULT
#include <windows.h>
#include <roapi.h>
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <cstdio>
#include <vector>

using namespace winrt::Windows::Data::Pdf;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: pdf_diag.exe <file.pdf>\n"); return 2; }
    HRESULT co = RoInitialize(RO_INIT_MULTITHREADED);
    wprintf(L"RoInitialize = 0x%08X\n", co);
    try {
        auto file = StorageFile::GetFileFromPathAsync(argv[1]).get();
        wprintf(L"GetFileFromPathAsync OK\n");
        auto doc = PdfDocument::LoadFromFileAsync(file).get();
        wprintf(L"LoadFromFileAsync OK, pages=%u\n", doc.PageCount());
        auto page = doc.GetPage(0);
        auto sz = page.Size();
        wprintf(L"page size = %.1f x %.1f DIP\n", sz.Width, sz.Height);
        PdfPageRenderOptions opt;
        opt.DestinationWidth((uint32_t)(sz.Width * 2));
        opt.DestinationHeight((uint32_t)(sz.Height * 2));
        auto ms = InMemoryRandomAccessStream();
        page.RenderToStreamAsync(ms, opt).get();
        wprintf(L"RenderToStreamAsync OK, %llu bytes\n", ms.Size());
        wprintf(L"ALL OK\n");
    } catch (winrt::hresult_error const& e) {
        wprintf(L"hresult_error 0x%08X: %s\n", (unsigned)e.code(), e.message().c_str());
    } catch (...) {
        wprintf(L"unknown exception\n");
    }
    if (co == S_OK || co == S_FALSE) RoUninitialize();
    return 0;
}
