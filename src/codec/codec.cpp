#include "codec.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STBIR_DEFAULT_FILTER_DOWNSAMPLE STBIR_FILTER_MITCHELL
#define STBIR_DEFAULT_FILTER_UPSAMPLE   STBIR_FILTER_CATMULLROM
#include "stb_image_resize2.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <roapi.h> // RoInitialize / RO_INIT_MULTITHREADED（Windows.Data.Pdf 用）
#include <cstring>
#include <wincodec.h>
#include <shlwapi.h>
#include <webp/decode.h>
#include <webp/encode.h>
#pragma comment(lib, "windowscodecs.lib")

// C++/WinRT：Windows.Data.Pdf 整页渲染（PDF 全格式支持，Win10+ 系统自带）
#include <unknwn.h> // WIN32_LEAN_AND_MEAN 下需显式引入（winrt/base 依赖）
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

namespace sp {

// 前置声明（probe_image 在下方使用）
static std::wstring ext_lower(const std::wstring& p);
static bool pdf_extract_dct(const std::vector<uint8_t>& buf, std::vector<uint8_t>& jpg);
static bool pdf_render_page_png(const std::vector<uint8_t>& buf, std::vector<uint8_t>& png, HRESULT* hr_out);
static bool wic_probe_memory(const uint8_t* data, size_t size, int& w, int& h);

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring widen(const std::string& s8) {
    if (s8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s8.c_str(), (int)s8.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s8.c_str(), (int)s8.size(), w.data(), n);
    return w;
}

bool read_file(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = TRUE;
    size_t off = 0;
    while (off < out.size()) {
        DWORD to_read = (DWORD)std::min<size_t>(out.size() - off, 1u << 30);
        if (!ReadFile(h, out.data() + off, to_read, &got, nullptr) || got == 0) { ok = FALSE; break; }
        off += got;
    }
    CloseHandle(h);
    return ok && off == out.size();
}

bool write_file(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = TRUE;
    size_t off = 0;
    while (off < data.size()) {
        DWORD to_write = (DWORD)std::min<size_t>(data.size() - off, 1u << 30);
        if (!WriteFile(h, data.data() + off, to_write, &put, nullptr) || put != to_write) { ok = FALSE; break; }
        off += put;
    }
    CloseHandle(h);
    return ok;
}

bool probe_image(const std::wstring& path, int& w, int& h) {
    std::vector<uint8_t> buf;
    if (!read_file(path, buf)) return false;
    std::wstring e = ext_lower(path);
    int comp = 0;
    // PDF：优先系统 PDF 引擎整页渲染（全格式），失败再提取内嵌 JPEG 读头
    if (e == L"pdf") {
        std::vector<uint8_t> png;
        if (pdf_render_page_png(buf, png, nullptr) &&
            stbi_info_from_memory(png.data(), (int)png.size(), &w, &h, &comp))
            return true;
        std::vector<uint8_t> jpg;
        if (!pdf_extract_dct(buf, jpg)) return false;
        buf = std::move(jpg);
    }
    // WebP：libwebp 读头
    if (e == L"webp")
        return WebPGetInfo(buf.data(), buf.size(), &w, &h) != 0;
    if (stbi_info_from_memory(buf.data(), (int)buf.size(), &w, &h, &comp)) return true;
    // stb 不支持的格式（TIFF/HEIC/HEIF/AVIF 等）走 WIC
    return wic_probe_memory(buf.data(), buf.size(), w, h);
}

static std::wstring ext_lower(const std::wstring& p); // 定义在下方（PDF/保存路径共用）

// PDF 中提取内嵌 DCTDecode（JPEG）图像：扫描所有 /DCTDecode 流，取最大的一个
// 仅支持 JPEG 位图的 PDF（相机/扫描仪/本工具导出的 PDF 都是 DCTDecode）；
// FlateDecode 位图 PDF 不支持。SMask 透明通道忽略（输出不透明底图）
static bool pdf_extract_dct(const std::vector<uint8_t>& buf, std::vector<uint8_t>& jpg) {
    if (buf.size() < 16) return false;
    auto starts_with = [&](size_t pos, const char* s) {
        size_t n = strlen(s);
        return pos + n <= buf.size() && memcmp(buf.data() + pos, s, n) == 0;
    };
    static const char kKey[] = "/DCTDecode";
    static const char kStream[] = "stream";
    bool found = false;
    size_t pos = 0;
    while (true) {
        // 找下一个 /DCTDecode
        auto it = std::search(buf.begin() + pos, buf.end(), kKey, kKey + strlen(kKey));
        if (it == buf.end()) break;
        size_t p = (size_t)(it - buf.begin()) + strlen(kKey);
        pos = p;
        // 向后找 stream 关键字（对象字典其余部分不含二进制，安全）
        auto s_it = std::search(buf.begin() + p, buf.end(), kStream, kStream + 6);
        if (s_it == buf.end()) break;
        size_t sp = (size_t)(s_it - buf.begin()) + 6;
        if (sp < buf.size() && buf[sp] == '\r') ++sp;
        if (sp < buf.size() && buf[sp] == '\n') ++sp;
        // JPEG 以 FF D8 开头（容忍流头多余空白）
        while (sp + 1 < buf.size() && !(buf[sp] == 0xFF && buf[sp + 1] == 0xD8)) {
            if (starts_with(sp, "endstream")) break;
            ++sp;
        }
        if (!(sp + 1 < buf.size() && buf[sp] == 0xFF && buf[sp + 1] == 0xD8)) continue;
        // 从 sp 起找 "endstream" 关键字定界
        size_t end_p = sp;
        while (end_p + 9 <= buf.size() && memcmp(buf.data() + end_p, "endstream", 9) != 0) ++end_p;
        if (end_p + 9 > buf.size()) break;
        // 在 [sp, end_p) 内找最后一个 FF D9 作为流结尾
        size_t last_eoi = 0;
        for (size_t i = sp; i + 1 < end_p; ++i)
            if (buf[i] == 0xFF && buf[i + 1] == 0xD9) last_eoi = i + 2;
        if (last_eoi == 0) continue;
        size_t sz = last_eoi - sp;
        if (sz > jpg.size()) {
            jpg.assign(buf.begin() + sp, buf.begin() + last_eoi);
            found = true;
        }
        pos = end_p + 9;
    }
    return found && !jpg.empty();
}

// ---------- PDF 整页渲染（Windows.Data.Pdf，Win10+ 系统自带 PDF 引擎） ----------
// 支持全部 PDF：矢量内容 / FlateDecode 位图 / DCTDecode 位图 / 对象流压缩等。
// 渲染第 1 页到内存 PNG（RenderToStreamAsync 默认 PNG 编码器），再走既有 PNG 解码。
// 失败返回 false 并透出 HRESULT，上层回退 DCTDecode 内嵌 JPEG 提取。
// 尺寸策略：按页面 DIP 尺寸（1/96 inch）放大到长边约 2400px（放大 1~3.125 倍）。
//
// 线程模型（关键）：PdfDocument/PdfPage 是非敏捷（non-agile）WinRT 对象，
// 异步完成回调必须封送回创建它的 apartment。在管线工作线程上直接
// RoInitialize(MTA) + 阻塞 .get() 会因回调无法完成而失败/死锁（小程序
// 主线程直调碰不到，真实管线必踩）。因此改为：
//   1) 专用 STA 线程执行整段 WinRT 逻辑；
//   2) 所有 IAsyncOperation 用「消息泵 + 轮询 Status」等待，绝不阻塞 .get()；
//   3) 主调线程只 WaitForSingleObject 等线程结束（STA 内部自带泵，不会卡）。
// 入口仍走 LoadFromStreamAsync 内存流 —— GetFileFromPathAsync 在受限令牌
// 进程会报 0x800700A1 ERROR_BAD_PATHNAME，不可依赖。
struct PdfRenderCtx {
    const std::vector<uint8_t>* buf = nullptr;
    std::vector<uint8_t> png;
    HRESULT hr = E_FAIL;
    bool ok = false;
};

// 等待 WinRT 异步完成：泵消息 + 轮询状态，返回失败 HRESULT（成功 S_OK）
template <typename A>
static HRESULT pdf_wait_async(A const& op) {
    using winrt::Windows::Foundation::AsyncStatus;
    AsyncStatus st = op.Status();
    while (st == AsyncStatus::Started) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(2);
        st = op.Status();
    }
    if (st == AsyncStatus::Error)
        return (HRESULT)op.ErrorCode(); // IAsyncInfo::ErrorCode 携带真实错误码
    return st == AsyncStatus::Completed ? S_OK : E_ABORT;
}

// PDF 渲染阶段级日志：写 exe 同目录 pdf_err.log，现场失败时提供 hr 定位
static void pdf_log(const char* tag, HRESULT hr) {
    char line[160];
    sprintf_s(line, sizeof(line), "%s hr=0x%08lX\n", tag, (unsigned long)hr);
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    std::wstring p = std::wstring(dir);
    size_t sl = p.find_last_of(L'\\');
    if (sl != std::wstring::npos) p = p.substr(0, sl + 1);
    p += L"pdf_err.log";
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD wr = 0;
    WriteFile(h, line, (DWORD)strlen(line), &wr, nullptr);
    CloseHandle(h);
}

static DWORD WINAPI pdf_render_thread(LPVOID p) {
    PdfRenderCtx* c = (PdfRenderCtx*)p;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co)) { pdf_log("coinit STA", co); c->hr = co; return 0; }
    try {
        using namespace winrt::Windows::Data::Pdf;
        using namespace winrt::Windows::Storage;
        using namespace winrt::Windows::Storage::Streams;
        auto src = InMemoryRandomAccessStream();
        {
            DataWriter wr(src);
            wr.WriteBytes(winrt::array_view<const uint8_t>(c->buf->data(), c->buf->data() + c->buf->size()));
            HRESULT h1 = pdf_wait_async(wr.StoreAsync());
            HRESULT h2 = pdf_wait_async(wr.FlushAsync());
            wr.DetachStream(); // 关键：DataWriter 析构默认会关闭底层流，先解除绑定
            if (FAILED(h1)) { pdf_log("writer.store", h1); throw winrt::hresult_error(h1); }
            if (FAILED(h2)) { pdf_log("writer.flush", h2); throw winrt::hresult_error(h2); }
        }
        src.Seek(0);
        auto load = PdfDocument::LoadFromStreamAsync(src);
        HRESULT hl = pdf_wait_async(load);
        if (FAILED(hl)) { pdf_log("doc.load", hl); throw winrt::hresult_error(hl); }
        auto doc = load.GetResults();
        if (doc.PageCount() < 1) { pdf_log("pagecount", E_FAIL); throw winrt::hresult_error(E_FAIL); }
        auto page = doc.GetPage(0);
        auto psz = page.Size(); // DIP（1/96 inch）
        float k = 1.f;
        float longEdge = std::max(psz.Width, psz.Height);
        if (longEdge > 0)
            k = std::min(std::max(2400.f / longEdge, 1.f), 3.125f);
        PdfPageRenderOptions opt;
        opt.DestinationWidth((uint32_t)std::max(1.f, std::round(psz.Width * k)));
        opt.DestinationHeight((uint32_t)std::max(1.f, std::round(psz.Height * k)));
        auto ms = InMemoryRandomAccessStream();
        HRESULT hr = pdf_wait_async(page.RenderToStreamAsync(ms, opt));
        if (FAILED(hr)) { pdf_log("page.render", hr); throw winrt::hresult_error(hr); }
        uint64_t total = ms.Size();
        if (total == 0 || total > 512ull * 1024 * 1024) { pdf_log("render.empty", (HRESULT)total); throw winrt::hresult_error(E_FAIL); }
        ms.Seek(0);
        DataReader dr(ms);
        auto ld = dr.LoadAsync((uint32_t)total);
        HRESULT hl2 = pdf_wait_async(ld);
        if (FAILED(hl2)) { pdf_log("reader.load", hl2); throw winrt::hresult_error(hl2); }
        c->png.resize((size_t)total);
        dr.ReadBytes(winrt::array_view<uint8_t>(c->png.data(), c->png.data() + c->png.size()));
        c->ok = !c->png.empty();
        if (c->ok) c->hr = S_OK;
    } catch (winrt::hresult_error const& e) {
        c->hr = (HRESULT)e.code().value;
        c->ok = false;
    } catch (...) {
        pdf_log("render.unknown_exc", E_FAIL);
        c->hr = E_FAIL;
        c->ok = false;
    }
    CoUninitialize();
    return 0;
}

static bool pdf_render_page_png(const std::vector<uint8_t>& buf, std::vector<uint8_t>& png, HRESULT* hr_out) {
    png.clear();
    PdfRenderCtx ctx;
    ctx.buf = &buf;
    HANDLE th = CreateThread(nullptr, 0, pdf_render_thread, &ctx, 0, nullptr);
    if (!th) { if (hr_out) *hr_out = HRESULT_FROM_WIN32(GetLastError()); return false; }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    if (hr_out) *hr_out = ctx.hr;
    if (ctx.ok) png.swap(ctx.png);
    return ctx.ok;
}

// WIC 解码：覆盖 stb 不支持的 TIFF/HEIC/HEIF/AVIF 等（依赖系统编解码器；
// HEIC 需系统装有 HEIF 图像扩展，未装则解码失败）
static bool wic_decode_memory(const uint8_t* data, size_t size, Image& out) {
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    IStream* stream = nullptr;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&f))) || !f) break;
        stream = SHCreateMemStream(data, (UINT)size);
        if (!stream) break;
        if (FAILED(f->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &frame))) break;
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        if (!w || !h || w > 65535 || h > 65535) break;
        if (FAILED(f->CreateFormatConverter(&conv))) break;
        if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom))) break;
        out = Image((int)w, (int)h);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)out.px.size(), out.px.data()))) { out = Image(); break; }
        ok = true;
    } while (false);
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (stream) stream->Release();
    if (f) f->Release();
    if (co == S_OK || co == S_FALSE) CoUninitialize();
    return ok;
}

// WIC 仅探测尺寸（不解码像素）
static bool wic_probe_memory(const uint8_t* data, size_t size, int& w, int& h) {
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IStream* stream = nullptr;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&f))) || !f) break;
        stream = SHCreateMemStream(data, (UINT)size);
        if (!stream) break;
        if (FAILED(f->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &frame))) break;
        UINT uw = 0, uh = 0;
        if (FAILED(frame->GetSize(&uw, &uh)) || !uw || !uh) break;
        w = (int)uw; h = (int)uh;
        ok = true;
    } while (false);
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (stream) stream->Release();
    if (f) f->Release();
    if (co == S_OK || co == S_FALSE) CoUninitialize();
    return ok;
}

bool load_image(const std::wstring& path, Image& out, std::string* err) {
    std::vector<uint8_t> buf;
    if (!read_file(path, buf)) { if (err) *err = "open failed"; return false; }
    std::wstring e = ext_lower(path);
    // PDF：优先系统 PDF 引擎整页渲染（支持矢量/FlateDecode 等全部 PDF），
    // 渲染结果为 PNG 直接解码；失败回退 DCTDecode 内嵌 JPEG 提取
    if (e == L"pdf") {
        std::vector<uint8_t> png;
        HRESULT phr = E_FAIL;
        if (pdf_render_page_png(buf, png, &phr)) {
            int w2 = 0, h2 = 0, c2 = 0;
            uint8_t* d2 = stbi_load_from_memory(png.data(), (int)png.size(), &w2, &h2, &c2, 4);
            if (d2) {
                out.w = w2; out.h = h2;
                out.px.assign(d2, d2 + (size_t)w2 * h2 * 4);
                stbi_image_free(d2);
                return true;
            }
        }
        std::vector<uint8_t> jpg;
        if (!pdf_extract_dct(buf, jpg)) {
            char m[96];
            sprintf_s(m, sizeof(m), "pdf: render failed (hr=0x%08lX) and no DCTDecode image", (unsigned long)phr);
            if (err) *err = m;
            return false;
        }
        buf = std::move(jpg);
    }
    // WebP：libwebp 解码（含无损/有损/扩展格式）
    if (e == L"webp") {
        int w = 0, h = 0;
        if (!WebPGetInfo(buf.data(), buf.size(), &w, &h)) { if (err) *err = "webp header"; return false; }
        uint8_t* rgb = WebPDecodeRGBA(buf.data(), buf.size(), &w, &h);
        if (!rgb) { if (err) *err = "webp decode"; return false; }
        out.w = w; out.h = h;
        out.px.assign(rgb, rgb + (size_t)w * h * 4);
        WebPFree(rgb);
        return true;
    }
    int w, h, comp;
    uint8_t* data = stbi_load_from_memory(buf.data(), (int)buf.size(), &w, &h, &comp, 4);
    if (!data) {
        // stb 不支持的格式（TIFF/HEIC/HEIF/AVIF 等）走 WIC
        if (wic_decode_memory(buf.data(), buf.size(), out)) return true;
        if (err) *err = "decode failed (stb+wic)";
        return false;
    }
    out.w = w; out.h = h;
    out.px.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return true;
}

// 内存解码（RGBA）：用于嵌入资源的 LOGO 等小图（仅走 stb，不含 PDF/WIC 扩展路径）
bool load_image_from_memory(const uint8_t* data, size_t len, Image& out) {
    int w, h, comp;
    uint8_t* d = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!d) return false;
    out.w = w; out.h = h;
    out.px.assign(d, d + (size_t)w * h * 4);
    stbi_image_free(d);
    return true;
}

std::vector<uint8_t> encode_jpg(const Image& img, int quality) {
    std::vector<uint8_t> out;
    if (!img.ok()) return out;
    auto cb = [](void* ctx, void* data, int size) {
        auto* v = (std::vector<uint8_t>*)ctx;
        v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
    };
    // stb 要求 3ch 输入：此处直接喂 RGBA？stbi_write_jpg 支持 comp=4，写出的 JPEG 是 YUV444，兼容
    stbi_write_jpg_to_func(cb, &out, img.w, img.h, 4, img.px.data(), quality);
    return out;
}

std::vector<uint8_t> encode_png(const Image& img) {
    std::vector<uint8_t> out;
    if (!img.ok()) return out;
    auto cb = [](void* ctx, void* data, int size) {
        auto* v = (std::vector<uint8_t>*)ctx;
        v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
    };
    stbi_write_png_to_func(cb, &out, img.w, img.h, 4, img.px.data(), img.w * 4);
    return out;
}

std::vector<uint8_t> encode_bmp(const Image& img) {
    std::vector<uint8_t> out;
    if (!img.ok()) return out;
    auto cb = [](void* ctx, void* data, int size) {
        auto* v = (std::vector<uint8_t>*)ctx;
        v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
    };
    stbi_write_bmp_to_func(cb, &out, img.w, img.h, 4, img.px.data());
    return out;
}

std::vector<uint8_t> encode_tga(const Image& img) {
    std::vector<uint8_t> out;
    if (!img.ok()) return out;
    auto cb = [](void* ctx, void* data, int size) {
        auto* v = (std::vector<uint8_t>*)ctx;
        v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
    };
    stbi_write_tga_to_func(cb, &out, img.w, img.h, 4, img.px.data());
    return out;
}

// 基线 TIFF（小端）：RGB 8bit 无压缩，单 IFD，整图一条 strip
std::vector<uint8_t> encode_tiff(const Image& img) {
    std::vector<uint8_t> out;
    if (!img.ok()) return out;
    auto u16 = [&](std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); };
    auto u32 = [&](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
        v.push_back((x >> 16) & 0xFF); v.push_back((x >> 24) & 0xFF);
    };
    const int nTags = 9;
    size_t ifd_off = 8;
    size_t ext_off = ifd_off + 2 + nTags * 12 + 4;       // BitsPerSample 外部值（3×SHORT）
    uint32_t data_off = (uint32_t)(ext_off + 8);          // 像素数据起点（8=外部值区，含 2B 对齐余量）
    uint32_t pix_bytes = (uint32_t)img.w * img.h * 3;
    out.reserve((size_t)data_off + pix_bytes);
    out.insert(out.end(), {'I', 'I', 0x2A, 0x00});
    u32(out, (uint32_t)ifd_off);
    u16(out, (uint16_t)nTags);
    auto tag = [&](uint16_t id, uint16_t type, uint32_t cnt, uint32_t val) {
        u16(out, id); u16(out, type); u32(out, cnt); u32(out, val);
    };
    tag(256, 4, 1, (uint32_t)img.w);            // ImageWidth
    tag(257, 4, 1, (uint32_t)img.h);            // ImageLength
    tag(258, 3, 3, (uint32_t)ext_off);          // BitsPerSample = [8,8,8]
    tag(259, 3, 1, 1);                          // Compression = 无
    tag(262, 3, 1, 2);                          // Photometric = RGB
    tag(273, 4, 1, data_off);                   // StripOffsets
    tag(277, 3, 1, 3);                          // SamplesPerPixel = 3
    tag(278, 4, 1, (uint32_t)img.h);            // RowsPerStrip = 整图
    tag(279, 4, 1, pix_bytes);                  // StripByteCounts
    u32(out, 0);                                // 下一 IFD = 无
    u16(out, 8); u16(out, 8); u16(out, 8);      // BitsPerSample 外部值
    out.resize(data_off, 0);                    // 对齐补零
    for (int y = 0; y < img.h; ++y) {           // RGBA → RGB
        const uint8_t* r = img.row(y);
        for (int x = 0; x < img.w; ++x) {
            out.push_back(r[x * 4 + 0]);
            out.push_back(r[x * 4 + 1]);
            out.push_back(r[x * 4 + 2]);
        }
    }
    return out;
}

// GIF89a：单帧静态 GIF，固定 256 色调色板（6×6×6 立方 216 色 + 40 级灰度）+ 标准 LZW
std::vector<uint8_t> encode_gif(const Image& img) {
    std::vector<uint8_t> out;
    if (!img.ok()) return out;
    // 全局色表：0..215 = RGB6 立方；216..255 = 灰度
    uint8_t pal[256 * 3];
    for (int i = 0; i < 216; ++i) {
        pal[i * 3 + 0] = (uint8_t)((i / 36) * 51);
        pal[i * 3 + 1] = (uint8_t)(((i / 6) % 6) * 51);
        pal[i * 3 + 2] = (uint8_t)((i % 6) * 51);
    }
    for (int i = 0; i < 40; ++i) {
        uint8_t v = (uint8_t)llround(i * 255.0 / 39.0);
        pal[(216 + i) * 3 + 0] = pal[(216 + i) * 3 + 1] = pal[(216 + i) * 3 + 2] = v;
    }
    auto pal_index = [](int r, int g, int b) -> uint8_t {
        int mx = std::max({r, g, b}), mn = std::min({r, g, b});
        if (mx - mn < 24) // 近灰：走灰度表（低色度时比立方色更准）
            return (uint8_t)(216 + std::min(39, (mx * 39 + 127) / 255));
        int r6 = std::min(5, r * 6 / 256), g6 = std::min(5, g * 6 / 256), b6 = std::min(5, b * 6 / 256);
        return (uint8_t)(r6 * 36 + g6 * 6 + b6);
    };
    out.insert(out.end(), {'G', 'I', 'F', '8', '9', 'a'});
    auto u16le = [&](uint16_t x) { out.push_back(x & 0xFF); out.push_back((x >> 8) & 0xFF); };
    u16le((uint16_t)img.w); u16le((uint16_t)img.h);
    out.push_back(0xF7); // GCT 存在，256 色
    out.push_back(0x00); out.push_back(0x00); // 背景索引 / 宽高比
    out.insert(out.end(), pal, pal + sizeof pal);
    out.push_back(0x2C); // 图像描述符
    u16le(0); u16le(0); u16le((uint16_t)img.w); u16le((uint16_t)img.h);
    out.push_back(0x00);
    out.push_back(0x08); // LZW 最小码长 8
    // LZW 编码（GIF 变体：清码 256 / EOI 257，码长 9→12）
    std::vector<uint8_t> lzw;
    int cur = 0, nbits = 0;
    auto put = [&](int code, int size) {
        cur |= code << nbits;
        nbits += size;
        while (nbits >= 8) { lzw.push_back((uint8_t)(cur & 0xFF)); cur >>= 8; nbits -= 8; }
    };
    std::unordered_map<uint32_t, int> dict; // key = prefix<<8 | byte
    int next = 258, csize = 9;
    put(256, csize); // 清码
    const uint8_t* px = img.px.data();
    size_t n = (size_t)img.w * img.h;
    int prefix = px[0];
    for (size_t i = 1; i < n; ++i) {
        uint32_t key = ((uint32_t)prefix << 8) | px[i];
        auto it = dict.find(key);
        if (it != dict.end()) {
            prefix = it->second;
        } else {
            put(prefix, csize);
            if (next < 4096) {
                dict[key] = next++;
                if (next == (1 << csize) && csize < 12) ++csize; // 与解码端一致：avail 达 2^csize 升码长
            } else { // 字典满：清码重来
                put(256, csize);
                dict.clear();
                next = 258; csize = 9;
            }
            prefix = px[i];
        }
    }
    put(prefix, csize);
    put(257, csize); // EOI
    if (nbits > 0) lzw.push_back((uint8_t)(cur & 0xFF));
    for (size_t i = 0; i < lzw.size(); i += 255) { // 子块打包（≤255B）
        size_t c = std::min<size_t>(255, lzw.size() - i);
        out.push_back((uint8_t)c);
        out.insert(out.end(), lzw.begin() + i, lzw.begin() + i + c);
    }
    out.push_back(0x00); // 子块结束
    out.push_back(0x3B); // 结束符
    return out;
}

// WebP：libwebp 有损编码（quality 0~100，与 JPEG 质量刻度一致）
std::vector<uint8_t> encode_webp(const Image& img, int quality) {
    std::vector<uint8_t> out;
    if (!img.ok()) return out;
    uint8_t* data = nullptr;
    size_t n = WebPEncodeRGBA(img.px.data(), img.w, img.h, img.w * 4,
                              (float)std::max(1, std::min(100, quality)), &data);
    if (n && data) {
        out.assign(data, data + n);
        WebPFree(data);
    }
    return out;
}

static std::wstring ext_lower(const std::wstring& p) {
    size_t dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    std::wstring e = p.substr(dot + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

bool save_image(const std::wstring& path, const Image& img, Fmt fmt, int jpg_quality) {
    std::vector<uint8_t> data;
    switch (fmt) {
    case Fmt::JPG: data = encode_jpg(img, jpg_quality); break;
    case Fmt::PNG: data = encode_png(img); break;
    case Fmt::BMP: data = encode_bmp(img); break;
    case Fmt::TIFF: data = encode_tiff(img); break;
    case Fmt::GIF: data = encode_gif(img); break;
    case Fmt::TGA: data = encode_tga(img); break;
    case Fmt::WEBP: data = encode_webp(img, jpg_quality > 0 ? jpg_quality : 90); break;
    default: return false;
    }
    if (data.empty()) return false;
    return write_file(path, data);
}

void resize_image(const Image& src, Image& dst) {
    dst = Image(dst.w, dst.h);
    if (!src.ok()) return;
    stbir_resize_uint8_srgb(src.px.data(), src.w, src.h, src.w * 4,
                            dst.px.data(), dst.w, dst.h, dst.w * 4,
                            STBIR_4CHANNEL);
}

void rotate_image(const Image& src, Image& dst, int deg) {
    // 原地安全：dst 可能与 src 是同一对象（如 rotate_image(img, img, deg)），
    // dst = Image(...) 会先释放原缓冲，src 随即变成全零 → 输出全空白。
    // 先把 src 拷贝一份再读，杜绝别名问题。
    const Image s = src;
    if (deg == 180) {
        dst = Image(s.w, s.h);
        for (int y = 0; y < s.h; ++y)
            for (int x = 0; x < s.w; ++x)
                memcpy(dst.row(y) + x * 4, s.row(s.h - 1 - y) + (s.w - 1 - x) * 4, 4);
    } else if (deg == 90) { // 顺时针
        dst = Image(s.h, s.w);
        for (int y = 0; y < s.h; ++y)
            for (int x = 0; x < s.w; ++x)
                memcpy(dst.row(x) + (s.h - 1 - y) * 4, s.row(y) + x * 4, 4);
    } else if (deg == 270 || deg == -90) { // 逆时针
        dst = Image(s.h, s.w);
        for (int y = 0; y < s.h; ++y)
            for (int x = 0; x < s.w; ++x)
                memcpy(dst.row(s.w - 1 - x) + y * 4, s.row(y) + x * 4, 4);
    } else {
        dst = s;
    }
}

void flip_image(const Image& src, Image& dst, bool horizontal) {
    dst = Image(src.w, src.h);
    for (int y = 0; y < src.h; ++y) {
        if (horizontal) {
            for (int x = 0; x < src.w; ++x)
                memcpy(dst.row(y) + x * 4, src.row(y) + (src.w - 1 - x) * 4, 4);
        } else {
            memcpy(dst.row(y), src.row(src.h - 1 - y), (size_t)src.w * 4);
        }
    }
}

Image fit_center_crop(const Image& src, int tw, int th, VAnchor va) {
    Image out;
    if (!src.ok() || tw <= 0 || th <= 0) return out;
    double sr = (double)src.w / src.h, tr = (double)tw / th;
    Image scaled;
    if (sr > tr) {
        // 源更宽：等比缩放到 高度=th，宽度溢出（左右裁）
        int w2 = (int)llround((double)th * src.w / src.h);
        scaled = Image(std::max(tw, w2 < 1 ? 1 : w2), th);
    } else {
        // 源更高：等比缩放到 宽度=tw，高度溢出（上下裁）
        int h2 = (int)llround((double)tw * src.h / src.w);
        scaled = Image(tw, std::max(th, h2 < 1 ? 1 : h2));
    }
    scaled.px.assign((size_t)scaled.w * scaled.h * 4, 255);
    resize_image(src, scaled);
    int cx = (scaled.w - tw) / 2;
    // 垂直锚点：Top=顶部对齐；Split25=顶部保 25% 溢出、底部裁 75%；Center=居中
    int ov = scaled.h - th; // 高度溢出量（可为 0）
    int cy = (va == VAnchor::Top) ? 0
           : (va == VAnchor::Split25) ? ov / 4
           : ov / 2;
    cx = std::max(0, std::min(cx, scaled.w - tw));
    cy = std::max(0, std::min(cy, scaled.h - th));
    out = Image(tw, th);
    for (int y = 0; y < th; ++y) {
        int sy = cy + y;
        if (sy >= scaled.h) sy = scaled.h - 1;
        const uint8_t* s = scaled.row(sy) + cx * 4;
        uint8_t* d = out.row(y);
        size_t row_bytes = (size_t)tw * 4;
        if (cx + tw <= scaled.w) memcpy(d, s, row_bytes);
        else { size_t avail = (size_t)(scaled.w - cx) * 4; memcpy(d, s, avail); memset(d + avail, 255, row_bytes - avail); }
    }
    return out;
}

} // namespace sp
