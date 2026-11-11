#include "ops.h"
#include "../codec/jpeg_meta.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <mutex>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace sp {

// 水印常见 8 色（与 UI 色块顺序一致：黑/白/红/橙/黄/绿/蓝/紫）
static const WmColor kWmColors[8] = {
    {0x1F,0x29,0x37}, {0xFF,0xFF,0xFF}, {0xDC,0x26,0x26}, {0xF9,0x73,0x16},
    {0xF7,0xC9,0x48}, {0x16,0xA3,0x4A}, {0x25,0x63,0xEB}, {0x7C,0x3A,0xED},
};
WmColor wm_color_by_index(int idx) {
    return kWmColors[std::max(0, std::min(7, idx))];
}

static std::wstring ext_lower(const std::wstring& p) {
    size_t dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    std::wstring e = p.substr(dot + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

static bool is_jpeg(const std::wstring& p) {
    std::wstring e = ext_lower(p);
    return e == L"jpg" || e == L"jpeg";
}

static std::wstring out_name(const std::wstring& src, const std::wstring& ext) {
    std::filesystem::path p(src);
    return p.stem().wstring() + ext;
}

// ---------------- 压缩到目标体积 ----------------

void smart_compress_plan(int q, double& k, int& eff_q) {
    q = std::max(1, std::min(95, q));
    if (q >= 40) { k = 1.0; eff_q = q; return; }
    k = std::max(0.10, (double)q / 40.0);
    eff_q = 40;
}

std::vector<uint8_t> compress_to_size(const Image& img, long long target_bytes,
                                      int min_q, int max_q, long long* final_size,
                                      int* final_q, bool* hit_target) {
    *hit_target = false;
    *final_q = max_q;
    std::vector<uint8_t> best = encode_jpg(img, max_q);
    *final_size = (long long)best.size();
    if ((long long)best.size() <= target_bytes) { *hit_target = true; return best; }

    int lo = min_q, hi = max_q;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        auto data = encode_jpg(img, mid);
        long long sz = (long long)data.size();
        if (sz <= target_bytes) {
            *final_q = mid; *final_size = sz; best = std::move(data); *hit_target = true;
            lo = mid + 1;              // 尽量贴近目标：尝试更高质量
        } else {
            hi = mid - 1;
            if (sz < *final_size) { *final_size = sz; best = std::move(data); *final_q = mid; }
        }
    }
    return best;
}

// ---------------- 文字水印 ----------------

// GDI 渲染文字到白字黑底 32bpp DIB，取亮度作 8bit 覆盖遮罩（带抗锯齿边缘）
bool render_text_mask(const std::wstring& text, int px_size, int angle_deg,
                      int& mw, int& mh, std::vector<uint8_t>& mask) {
    if (text.empty() || px_size <= 0) return false;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    if (!sdc || !mdc) { if (sdc) ReleaseDC(nullptr, sdc); if (mdc) DeleteDC(mdc); return false; }
    int esc = (angle_deg % 360) * 10;
    HFONT f = CreateFontW(-px_size, 0, esc, esc, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH, L"微软雅黑");
    HFONT of = (HFONT)SelectObject(mdc, f);
    SIZE ext = {};
    GetTextExtentPoint32W(mdc, text.c_str(), (int)text.size(), &ext);
    double rad = angle_deg * 3.14159265358979 / 180.0;
    double cs = cos(rad), sn = sin(rad);
    // 画布按旋转后包围盒放大，四周留边
    int W = (int)ceil(ext.cx * fabs(cs) + ext.cy * fabs(sn)) + 24;
    int H = (int)ceil(ext.cx * fabs(sn) + ext.cy * fabs(cs)) + 24;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HBITMAP ob = (HBITMAP)SelectObject(mdc, bmp);
        HBRUSH bkb = CreateSolidBrush(RGB(0, 0, 0));
        RECT rc = {0, 0, W, H};
        FillRect(mdc, &rc, bkb);
        DeleteObject(bkb);
        SetTextColor(mdc, RGB(255, 255, 255));
        SetBkMode(mdc, TRANSPARENT);
        // 起点计算使文本包围盒居中：屏幕系 y 向下，escapement 逆时针 → d=(cos,-sin)，字面下方向 q=(sin,cos)
        int ox = (int)(W / 2.0 - ext.cx / 2.0 * cs - ext.cy / 2.0 * sn);
        int oy = (int)(H / 2.0 + ext.cx / 2.0 * sn - ext.cy / 2.0 * cs);
        TextOutW(mdc, ox, oy, text.c_str(), (int)text.size());
        GdiFlush();
        mask.assign((size_t)W * H, 0);
        const uint8_t* src8 = (const uint8_t*)bits;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                mask[(size_t)y * W + x] = src8[((size_t)y * W + x) * 4]; // B 通道（白字黑底，亮度即覆盖度）
        mw = W; mh = H;
        ok = true;
        SelectObject(mdc, ob);
    }
    SelectObject(mdc, of);
    DeleteObject(f);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    return ok;
}

// 遮罩混合：a = 遮罩亮度 × 不透明度，逐像素 src*(1-a)+color*a
void blend_watermark(Image& img, const std::vector<uint8_t>& mask, int mw, int mh,
                     int cr, int cg, int cb, int alpha_pct, int layout, int spacing) {
    if (!img.ok() || mask.empty() || mw <= 0 || mh <= 0) return;
    int alpha = std::max(0, std::min(100, alpha_pct));
    if (alpha == 0) return;
    auto stamp = [&](int ox, int oy) {
        int x0 = std::max(0, ox), y0 = std::max(0, oy);
        int x1 = std::min(img.w, ox + mw), y1 = std::min(img.h, oy + mh);
        for (int y = y0; y < y1; ++y) {
            const uint8_t* m = mask.data() + (size_t)(y - oy) * mw;
            uint8_t* p = img.row(y) + (size_t)x0 * 4;
            for (int x = x0; x < x1; ++x, p += 4) {
                int a = m[x - ox] * alpha / 255;
                if (!a) continue;
                // Image 为 RGBA（stb 解码顺序）：p[0]=R/p[1]=G/p[2]=B
                // （此前误按 BGRA 写入 p[0]=cb/p[2]=cr，红蓝通道颠倒，红色水印显示为蓝色）
                p[0] = (uint8_t)((p[0] * (255 - a) + cr * a) / 255);
                p[1] = (uint8_t)((p[1] * (255 - a) + cg * a) / 255);
                p[2] = (uint8_t)((p[2] * (255 - a) + cb * a) / 255);
            }
        }
    };
    if (layout == 1) {
        stamp((img.w - mw) / 2, (img.h - mh) / 2);
    } else if (layout == 2) {
        stamp(img.w - mw - img.w / 20, img.h - mh - img.h / 20);
    } else {
        int stepx = mw + std::max(10, spacing), stepy = mh + std::max(10, spacing);
        // 平铺起点带负偏移，图案视觉居中且覆盖到边缘
        int ox0 = ((img.w % stepx) - stepx) / 2 - stepx;
        int oy0 = ((img.h % stepy) - stepy) / 2 - stepy;
        for (int oy = oy0; oy < img.h; oy += stepy)
            for (int ox = ox0; ox < img.w; ox += stepx)
                stamp(ox, oy);
    }
}

// RGBA 图像任意角度旋转：双线性采样，画布扩到旋转后包围盒，出界像素 alpha=0（透明）
static Image rotate_rgba_bilinear(const Image& src, int angle_deg) {
    int a = ((angle_deg % 360) + 360) % 360;
    if (a == 0 || !src.ok()) return src;
    double rad = a * 3.14159265358979 / 180.0;
    double cs = std::cos(rad), sn = std::sin(rad);
    int W = std::max(1, (int)std::ceil(src.w * std::fabs(cs) + src.h * std::fabs(sn)));
    int H = std::max(1, (int)std::ceil(src.w * std::fabs(sn) + src.h * std::fabs(cs)));
    Image out(W, H);
    out.px.assign((size_t)W * H * 4, 0); // 透明底
    double cx0 = (src.w - 1) / 2.0, cy0 = (src.h - 1) / 2.0;
    double cx1 = (W - 1) / 2.0, cy1 = (H - 1) / 2.0;
    for (int y = 0; y < H; ++y) {
        uint8_t* o = out.row(y);
        for (int x = 0; x < W; ++x, o += 4) {
            double dx = x - cx1, dy = y - cy1;
            double sx = cs * dx + sn * dy + cx0; // 逆映射回源坐标
            double sy = -sn * dx + cs * dy + cy0;
            if (sx < -0.5 || sy < -0.5 || sx > src.w - 0.5 || sy > src.h - 0.5) continue;
            int x0 = (int)std::floor(sx), y0 = (int)std::floor(sy);
            double fx = sx - x0, fy = sy - y0;
            int xa = std::max(0, x0), xb = std::min(src.w - 1, x0 + 1);
            int ya = std::max(0, y0), yb = std::min(src.h - 1, y0 + 1);
            const uint8_t* p00 = src.row(ya) + (size_t)xa * 4;
            const uint8_t* p01 = src.row(ya) + (size_t)xb * 4;
            const uint8_t* p10 = src.row(yb) + (size_t)xa * 4;
            const uint8_t* p11 = src.row(yb) + (size_t)xb * 4;
            for (int c = 0; c < 4; ++c) {
                double v = p00[c] * (1 - fx) * (1 - fy) + p01[c] * fx * (1 - fy)
                         + p10[c] * (1 - fx) * fy + p11[c] * fx * fy;
                o[c] = (uint8_t)std::min(255.0, std::max(0.0, v + 0.5));
            }
        }
    }
    return out;
}

// 图片水印：wm 含 alpha 通道，按宿主图宽 scale% 缩放（可旋转）后混合
// 布局：0=平铺 1=居中单个 2=右下角 3=左上角 4=右上角 5=左下角（边距=图宽/20）
void blend_image_watermark(Image& img, const Image& wm,
                           int alpha_pct, int layout, int spacing, int scale_pct, int angle_deg) {
    if (!img.ok() || !wm.ok()) return;
    int alpha = std::max(0, std::min(100, alpha_pct));
    if (alpha == 0) return;
    int tw = std::max(1, (int)llround((double)img.w * std::max(1, std::min(100, scale_pct)) / 100.0));
    int th = std::max(1, wm.h * tw / wm.w);
    if (th > img.h) { th = img.h; tw = std::max(1, wm.w * th / wm.h); }
    Image s(tw, th);
    resize_image(wm, s);
    if (angle_deg) s = rotate_rgba_bilinear(s, angle_deg); // 旋转后尺寸变化
    tw = s.w; th = s.h;
    auto stamp = [&](int ox, int oy) {
        int x0 = std::max(0, ox), y0 = std::max(0, oy);
        int x1 = std::min(img.w, ox + tw), y1 = std::min(img.h, oy + th);
        for (int y = y0; y < y1; ++y) {
            const uint8_t* m = s.row(y - oy);
            uint8_t* p = img.row(y) + (size_t)x0 * 4;
            for (int x = x0; x < x1; ++x, p += 4) {
                int a = m[(x - ox) * 4 + 3] * alpha / 100;
                if (!a) continue;
                p[0] = (uint8_t)((p[0] * (255 - a) + m[(x - ox) * 4 + 0] * a) / 255);
                p[1] = (uint8_t)((p[1] * (255 - a) + m[(x - ox) * 4 + 1] * a) / 255);
                p[2] = (uint8_t)((p[2] * (255 - a) + m[(x - ox) * 4 + 2] * a) / 255);
            }
        }
    };
    int mx = img.w / 20, my = img.h / 20; // 四角边距
    if (layout == 1) {
        stamp((img.w - tw) / 2, (img.h - th) / 2);
    } else if (layout == 2) {
        stamp(img.w - tw - mx, img.h - th - my);
    } else if (layout == 3) { // 左上角
        stamp(mx, my);
    } else if (layout == 4) { // 右上角
        stamp(img.w - tw - mx, my);
    } else if (layout == 5) { // 左下角
        stamp(mx, img.h - th - my);
    } else {
        int stepx = tw + std::max(10, spacing), stepy = th + std::max(10, spacing);
        int ox0 = ((img.w % stepx) - stepx) / 2 - stepx;
        int oy0 = ((img.h % stepy) - stepy) / 2 - stepy;
        for (int oy = oy0; oy < img.h; oy += stepy)
            for (int ox = ox0; ox < img.w; ox += stepx)
                stamp(ox, oy);
    }
}

// ---------------- 长图拼接 ----------------

ItemResult stitch_files(const std::vector<std::wstring>& paths, const std::wstring& out_path,
                        int mode, int fmt, const std::function<bool(int, int)>& progress) {
    ItemResult r;
    int n = (int)paths.size();
    if (n < 2) { r.error = L"拼接至少需要 2 张图片"; return r; }
    int use = (mode == 2) ? std::min(n, 9) : n; // 九宫格只取前 9 张
    std::vector<Image> imgs;
    for (int i = 0; i < use; ++i) {
        Image im;
        std::string err;
        if (!load_image(paths[i], im, &err)) {
            r.error = L"解码失败: " + std::filesystem::path(paths[i]).filename().wstring();
            return r;
        }
        if (progress && !progress(i + 1, use)) { r.error = L"已取消"; return r; }
        imgs.push_back(std::move(im));
    }
    long long W, H;
    if (mode == 2) {
        // 九宫格：格尺寸取各图宽高的最小值，每张居中裁剪到格子
        int cw = imgs[0].w, ch = imgs[0].h;
        for (auto& im : imgs) { cw = std::min(cw, im.w); ch = std::min(ch, im.h); }
        if (cw <= 0 || ch <= 0) { r.error = L"图片尺寸无效"; return r; }
        W = (long long)cw * 3;
        H = (long long)ch * 3;
        Image out((int)W, (int)H); // 白底，空格留白
        for (int i = 0; i < (int)imgs.size(); ++i) {
            Image cell = fit_center_crop(imgs[i], cw, ch, VAnchor::Center);
            int gx = (i % 3) * cw, gy = (i / 3) * ch;
            for (int yy = 0; yy < ch; ++yy)
                memcpy(out.row(gy + yy) + (size_t)gx * 4, cell.row(yy), (size_t)cw * 4);
        }
        if (!save_image(out_path, out, fmt == 0 ? Fmt::PNG : Fmt::JPG, 95)) { r.error = L"写入失败"; return r; }
        r.ok = true; r.out_path = out_path;
        std::error_code ec;
        r.out_bytes = (long long)std::filesystem::file_size(std::filesystem::path(out_path), ec);
        return r;
    }
    if (mode == 0) {
        W = 0; for (auto& im : imgs) W = std::max<long long>(W, im.w);
        H = 0; for (auto& im : imgs) H += im.h;
    } else {
        H = 0; for (auto& im : imgs) H = std::max<long long>(H, im.h);
        W = 0; for (auto& im : imgs) W += im.w;
    }
    if (W > 32767 * 4 || H > 32767 * 4 || (double)W * H > 2e9) { r.error = L"拼接结果尺寸过大"; return r; }
    Image out((int)W, (int)H); // 构造即白色底
    if (mode == 0) {
        int y = 0;
        for (auto& im : imgs) {
            int x = ((int)W - im.w) / 2; // 窄图左右居中，白底补齐
            for (int yy = 0; yy < im.h; ++yy)
                memcpy(out.row(y + yy) + (size_t)x * 4, im.row(yy), (size_t)im.w * 4);
            y += im.h;
        }
    } else {
        int x = 0;
        for (auto& im : imgs) {
            int y = ((int)H - im.h) / 2;
            for (int yy = 0; yy < im.h; ++yy)
                memcpy(out.row(y + yy) + (size_t)x * 4, im.row(yy), (size_t)im.w * 4);
            x += im.w;
        }
    }
    if (!save_image(out_path, out, fmt == 0 ? Fmt::PNG : Fmt::JPG, 95)) {
        r.error = L"写入失败";
        return r;
    }
    r.ok = true;
    r.out_path = out_path;
    std::error_code ec;
    r.out_bytes = (long long)std::filesystem::file_size(std::filesystem::path(out_path), ec);
    return r;
}

// ---------------- 最小 PDF 写出器（多页，每页内嵌 JPEG DCTDecode） ----------------

struct PdfPageImg { std::vector<uint8_t> jpeg; int w, h; };

// 对象编号：1=Catalog 2=Pages，随后每页 3 个对象（Page/Image/Contents）
static bool write_pdf_multi(const std::wstring& path, const std::vector<PdfPageImg>& pages) {
    if (pages.empty()) return false;
    int N = (int)pages.size();
    std::string d = "%PDF-1.4\n";
    std::vector<uint32_t> off((size_t)N * 3 + 3, 0); // obj 1..(2+3N)
    auto emit = [&](const std::string& s) { d += s; };
    auto begin_obj = [&](int i) { off[i] = (uint32_t)d.size(); };
    char buf[256];

    begin_obj(1);
    emit("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    begin_obj(2);
    {
        std::string kids;
        for (int i = 0; i < N; ++i) kids += std::to_string(3 + 3 * i) + " 0 R ";
        snprintf(buf, sizeof(buf), "2 0 obj\n<< /Type /Pages /Kids [ %s] /Count %d >>\nendobj\n",
                 kids.c_str(), N);
        emit(buf);
    }
    for (int i = 0; i < N; ++i) {
        int po = 3 + 3 * i, io = 4 + 3 * i, co = 5 + 3 * i;
        const PdfPageImg& pg = pages[(size_t)i];
        begin_obj(po);
        snprintf(buf, sizeof(buf),
                 "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
                 "/Resources << /XObject << /Im1 %d 0 R >> /ProcSet [/PDF /ImageC] >> "
                 "/Contents %d 0 R >>\nendobj\n", po, pg.w, pg.h, io, co);
        emit(buf);
        begin_obj(io);
        snprintf(buf, sizeof(buf),
                 "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
                 "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length %zu >>\nstream\n",
                 io, pg.w, pg.h, pg.jpeg.size());
        emit(buf);
        d.insert(d.end(), pg.jpeg.begin(), pg.jpeg.end());
        emit("\nendstream\nendobj\n");
        std::string content = "q\n" + std::to_string(pg.w) + " 0 0 " + std::to_string(pg.h)
                            + " 0 0 cm\n/Im1 Do\nQ\n";
        begin_obj(co);
        snprintf(buf, sizeof(buf), "%d 0 obj\n<< /Length %zu >>\nstream\n%sendstream\nendobj\n",
                 co, content.size(), content.c_str());
        emit(buf);
    }
    int total_objs = 2 + 3 * N;
    uint32_t xref = (uint32_t)d.size();
    emit("xref\n0 " + std::to_string(total_objs + 1) + "\n0000000000 65535 f \n");
    for (int i = 1; i <= total_objs; ++i)
        snprintf(buf, sizeof(buf), "%010u 00000 n \n", off[i]), emit(buf);
    snprintf(buf, sizeof(buf), "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%u\n%%%%EOF\n",
             total_objs + 1, xref);
    emit(buf);

    std::vector<uint8_t> out(d.begin(), d.end());
    return write_file(path, out);
}

static bool write_pdf(const std::wstring& path, const std::vector<uint8_t>& jpeg, int w, int h) {
    std::vector<PdfPageImg> one;
    one.push_back({jpeg, w, h});
    return write_pdf_multi(path, one);
}

// ---------------- 多图转 PDF 合并注册表 ----------------
// UI 在 start_processing 时 pdf_merge_begin（列表顺序=页序）；worker 在 PDF 转换时
// 登记分片（并发安全）；完成哨兵（所有 worker 收尾后）pdf_merge_finish 合成单文件。
static std::mutex g_pdfm_mu;
static std::vector<std::wstring> g_pdfm_order; // 页序（UI 列表顺序）
static std::wstring g_pdfm_dst;                // 合并输出路径
struct PdfMergePart { std::wstring src; std::vector<uint8_t> jpg; int w, h; };
static std::vector<PdfMergePart> g_pdfm_parts; // 已完成编码的分片（乱序到达）

void pdf_merge_begin(const std::vector<std::wstring>& ordered_paths, const std::wstring& dst) {
    std::lock_guard<std::mutex> lk(g_pdfm_mu);
    g_pdfm_order = ordered_paths;
    g_pdfm_dst = dst;
    g_pdfm_parts.clear();
}

long long pdf_merge_finish() {
    std::vector<PdfPageImg> pages;
    std::wstring dst;
    {
        std::lock_guard<std::mutex> lk(g_pdfm_mu);
        if (g_pdfm_parts.empty()) { // 无分片：非合并运行或全部失败
            g_pdfm_order.clear(); g_pdfm_dst.clear();
            return -1;
        }
        // 按 UI 列表顺序组页；缺失的（失败/取消）跳过
        for (auto& o : g_pdfm_order)
            for (auto& pt : g_pdfm_parts)
                if (pt.src == o) { pages.push_back({std::move(pt.jpg), pt.w, pt.h}); break; }
        dst = g_pdfm_dst;
        g_pdfm_order.clear(); g_pdfm_dst.clear();
        g_pdfm_parts.clear();
    }
    if (pages.empty() || dst.empty()) return -1;
    if (!write_pdf_multi(dst, pages)) return -1;
    std::error_code ec;
    long long bytes = (long long)std::filesystem::file_size(std::filesystem::path(dst), ec);
    return ec ? -1 : bytes;
}

// ---------------- 图标导出 ----------------

static bool write_ico(const std::wstring& path, const Image& src, const int* sizes, int n) {
    struct Entry { std::vector<uint8_t> png; uint8_t w, h; };
    std::vector<Entry> entries;
    for (int i = 0; i < n; ++i) {
        int s = sizes[i];
        Image scaled;
        Image tmp = src;
        scaled.w = s; scaled.h = s;
        scaled.px.assign((size_t)s * s * 4, 255);
        resize_image(tmp, scaled);
        auto png = encode_png(scaled);
        if (png.empty()) return false;
        entries.push_back({std::move(png), (uint8_t)(s >= 256 ? 0 : s), (uint8_t)(s >= 256 ? 0 : s)});
    }
    // ICONDIR + entries
    std::vector<uint8_t> out;
    uint16_t count = (uint16_t)entries.size();
    out.insert(out.end(), {(uint8_t)0, (uint8_t)0, (uint8_t)1, (uint8_t)0}); // type=icon
    out.push_back((uint8_t)(count & 0xFF)); out.push_back((uint8_t)(count >> 8));
    size_t dir_size = 6 + (size_t)count * 16;
    uint32_t offset = (uint32_t)dir_size;
    for (auto& e : entries) {
        out.push_back(e.w); out.push_back(e.h);
        out.push_back(0);              // 色板 0
        out.push_back(0);              // 保留
        out.push_back(1); out.push_back(0);  // planes
        out.push_back(32); out.push_back(0); // bpp
        uint32_t sz = (uint32_t)e.png.size();
        out.push_back((uint8_t)(sz)); out.push_back((uint8_t)(sz >> 8));
        out.push_back((uint8_t)(sz >> 16)); out.push_back((uint8_t)(sz >> 24));
        out.push_back((uint8_t)(offset)); out.push_back((uint8_t)(offset >> 8));
        out.push_back((uint8_t)(offset >> 16)); out.push_back((uint8_t)(offset >> 24));
        offset += sz;
    }
    for (auto& e : entries) out.insert(out.end(), e.png.begin(), e.png.end());
    return write_file(path, out);
}

// ---------------- 单文件处理入口 ----------------

ItemResult run_op(const std::wstring& src, const Params& p, const std::wstring& out_dir) {
    ItemResult r;
    std::filesystem::path fs_src(src);
    std::filesystem::path fs_out(out_dir);

    auto finish = [&](ItemResult res, Fmt fmt_used, const Image* saved_img) -> ItemResult {
        (void)fmt_used; (void)saved_img;
        if (res.ok) {
            std::error_code ec;
            res.out_bytes = (long long)std::filesystem::file_size(std::filesystem::path(res.out_path), ec);
        }
        return res;
    };

    Image img;
    std::string err;
    if (!load_image(src, img, &err)) {
        r.error = L"解码失败: " + std::wstring(err.begin(), err.end());
        return r;
    }

    bool strip = p.strip_meta;

    switch (p.module_id) {

    case MOD_ID_PHOTO: {
        static const struct { int w, h; } kIdSpecs[] = {
            {295, 413},  // 一寸
            {260, 378},  // 小一寸
            {413, 579},  // 二寸
            {413, 531},  // 小二寸
            {390, 567},  // 护照
        };
        int tw, th;
        if (p.id_photo >= 0 && p.id_photo <= 4) {
            tw = kIdSpecs[p.id_photo].w;
            th = kIdSpecs[p.id_photo].h;
        } else { // 自定义
            tw = p.id_custom_w;
            th = p.id_custom_h;
        }
        if (tw <= 0 || th <= 0 || tw > 10000 || th > 10000) {
            r.error = L"自定义宽高无效";
            return r;
        }
        // 证件照两段式裁剪：溢出部分顶部保 25%、底部裁 75%
        // （居中裁会削头顶留白；纯顶对齐则无法压掉过量留白、贴近头位规范）
        Image out = fit_center_crop(img, tw, th, VAnchor::Split25);
        std::wstring dst = (fs_out / out_name(src, L".jpg")).wstring();
        auto data = encode_jpg(out, 95);
        // 证件照写 DPI 300
        data = jpeg_set_dpi(data, 300);
        if (!write_file(dst, data)) { r.error = L"写入失败"; return r; }
        r.ok = true; r.out_path = dst;
        return finish(std::move(r), Fmt::JPG, &out);
    }

    case MOD_COMPRESS_TARGET: {
        long long target = (long long)p.target_kb * 1000; // 用 1000 进制贴近报名系统习惯
        long long src_size = 0;
        { std::error_code ec; src_size = (long long)std::filesystem::file_size(fs_src, ec); }

        // 原图已达标：跳过 或 填充
        if (is_jpeg(src) && src_size <= target) {
            std::wstring dst = (fs_out / fs_src.filename()).wstring();
            if (!p.fill_if_smaller) {
                std::filesystem::copy_file(fs_src, std::filesystem::path(dst), std::filesystem::copy_options::overwrite_existing);
                r.ok = true; r.out_path = dst;
                r.error = L"原图小于目标，已跳过压缩";
                return finish(std::move(r), Fmt::JPG, nullptr);
            }
            std::vector<uint8_t> raw;
            read_file(src, raw);
            // 先剥离元数据再填充，避免填充无效
            auto stripped = jpeg_strip(raw, true);
            long long extra = target - (long long)stripped.size();
            auto filled = jpeg_fill_com(stripped, std::max<size_t>(extra, 1));
            write_file(dst, filled);
            r.ok = true; r.out_path = dst;
            return finish(std::move(r), Fmt::JPG, nullptr);
        }

        long long final_size = 0; int final_q = 0; bool hit = false;
        auto data = compress_to_size(img, target, 40, 95, &final_size, &final_q, &hit);
        if (strip) data = jpeg_strip(data, true);
        if ((long long)data.size() > target) {
            // 质量到 40 仍超标：按实测字节比例自适应迭代降分辨率（JPEG 字节≈像素线性，
            // 取 sqrt(目标/实测) 估算所需边长比例，留 15% 余量），循环直到达标或到下限
            double k = 1.0;
            for (int iter = 0; iter < 10 && (long long)data.size() > target; ++iter) {
                double ratio = std::sqrt((double)target / (double)data.size()) * 0.85;
                k = std::max(0.05, k * ratio);
                Image scaled;
                scaled.w = std::max(1, (int)llround(img.w * k));
                scaled.h = std::max(1, (int)llround(img.h * k));
                if (scaled.w == img.w && scaled.h == img.h) break; // 已到下限，不再空转
                scaled.px.assign((size_t)scaled.w * scaled.h * 4, 255);
                resize_image(img, scaled);
                long long s2; int q2; bool h2;
                data = compress_to_size(scaled, target, 40, 95, &s2, &q2, &h2);
                if (strip) data = jpeg_strip(data, true);
                final_size = s2;
            }
        }
        // 仍不达标且允许填充：填充到目标（保底达标场景）
        if ((long long)data.size() > target && p.fill_if_smaller) {
            r.error = L"无法压到目标（已取最接近值）";
        }
        // 非jpeg 源压缩后统一输出 .jpg
        std::wstring dst = (fs_out / (is_jpeg(src) ? fs_src.filename().wstring()
                                                   : out_name(src, L".jpg"))).wstring();
        write_file(std::filesystem::path(dst), data);
        r.ok = (long long)data.size() <= target;
        if (!r.ok && r.error.empty()) r.error = L"已输出最接近目标的版本";
        r.out_path = dst;
        return finish(std::move(r), Fmt::JPG, nullptr);
    }

    case MOD_COMPRESS_LIVE: {
        // 智能压缩：按滑块质量编码；q<40 时保质量降分辨率（smart_compress_plan）
        int q = std::max(1, std::min(95, p.live_q));
        double k; int eq;
        smart_compress_plan(q, k, eq);
        const Image* work = &img;
        Image scaled;
        if (k < 1.0) {
            scaled.w = std::max(1, (int)llround(img.w * k));
            scaled.h = std::max(1, (int)llround(img.h * k));
            scaled.px.assign((size_t)scaled.w * scaled.h * 4, 255);
            resize_image(img, scaled);
            work = &scaled;
        }
        auto data = encode_jpg(*work, eq);
        if (strip) data = jpeg_strip(data, true);
        std::wstring dst = (fs_out / (is_jpeg(src) ? fs_src.filename().wstring()
                                                   : out_name(src, L".jpg"))).wstring();
        write_file(std::filesystem::path(dst), data);
        r.ok = true; r.out_path = dst;
        return finish(std::move(r), Fmt::JPG, nullptr);
    }

    case MOD_WATERMARK: {
        int mw = 0, mh = 0;
        std::vector<uint8_t> mask;
        if (!render_text_mask(p.wm_text, std::max(6, p.wm_font_size), p.wm_angle, mw, mh, mask)) {
            r.error = L"水印文字渲染失败";
            return r;
        }
        WmColor wc = wm_color_by_index(p.wm_color);
        blend_watermark(img, mask, mw, mh, wc.r, wc.g, wc.b, p.wm_alpha, p.wm_layout, p.wm_spacing);
        bool png = ext_lower(src) == L"png";
        std::wstring ext = png ? L".png" : L".jpg";
        std::wstring dst = (fs_out / (fs_src.stem().wstring() + ext)).wstring();
        if (png) {
            save_image(dst, img, Fmt::PNG, 0);
        } else {
            auto data = encode_jpg(img, 95);
            if (strip) data = jpeg_strip(data, true);
            write_file(std::filesystem::path(dst), data);
        }
        r.ok = true; r.out_path = dst;
        return finish(std::move(r), png ? Fmt::PNG : Fmt::JPG, &img);
    }

    case MOD_WATERMARK_IMG: {
        if (p.wm_image_path.empty()) {
            r.error = L"未选择水印图片";
            return r;
        }
        Image wmim;
        if (!load_image(p.wm_image_path, wmim)) {
            r.error = L"水印图片加载失败";
            return r;
        }
        blend_image_watermark(img, wmim, p.wm_img_alpha, p.wm_img_layout, p.wm_img_spacing, p.wm_img_scale, p.wm_img_angle);
        bool png = ext_lower(src) == L"png";
        std::wstring ext = png ? L".png" : L".jpg";
        std::wstring dst = (fs_out / (fs_src.stem().wstring() + ext)).wstring();
        if (png) {
            save_image(dst, img, Fmt::PNG, 0);
        } else {
            auto data = encode_jpg(img, 95);
            if (strip) data = jpeg_strip(data, true);
            write_file(std::filesystem::path(dst), data);
        }
        r.ok = true; r.out_path = dst;
        return finish(std::move(r), png ? Fmt::PNG : Fmt::JPG, &img);
    }

    case MOD_FILL_SIZE: {
        long long target = (long long)p.fill_target_kb * 1000;
        std::error_code ec;
        long long cur = (long long)std::filesystem::file_size(fs_src, ec);
        std::wstring e = ext_lower(src);
        std::vector<uint8_t> raw;
        if (!read_file(src, raw)) { r.error = L"读取失败"; return r; }
        std::vector<uint8_t> data;
        if (e == L"png") data = png_fill_text(raw, std::max<size_t>(0, (size_t)(target - cur)));
        else             data = jpeg_fill_com(raw, std::max<size_t>(0, (size_t)(target - cur)));
        std::wstring dst = (fs_out / fs_src.filename()).wstring();
        write_file(std::filesystem::path(dst), data);
        r.ok = true; r.out_path = dst;
        return finish(std::move(r), Fmt::JPG, nullptr);
    }

    case MOD_SET_DPI: {
        std::vector<uint8_t> raw;
        if (!read_file(src, raw)) { r.error = L"读取失败"; return r; }
        std::vector<uint8_t> data;
        std::wstring dst;
        if (is_jpeg(src)) {
            data = jpeg_set_dpi(raw, p.dpi);
            dst = (fs_out / fs_src.filename()).wstring();
        } else if (ext_lower(src) == L"png") {
            // PNG：stb 重编码后写 pHYs 由 save 流程处理；阶段 1 先重编码 + 手写 pHYs
            // 简化：仅支持 JPEG 字节直改，PNG 走重编码路径
            Image out = img;
            data = encode_png(out);
            data = png_set_dpi(data, p.dpi);
            dst = (fs_out / fs_src.filename()).wstring();
        } else {
            r.error = L"仅支持 JPG/PNG 修改 DPI";
            return r;
        }
        write_file(std::filesystem::path(dst), data);
        r.ok = true; r.out_path = dst;
        return finish(std::move(r), Fmt::JPG, nullptr);
    }

    case MOD_CENTER_CROP: {
        Image out;
        static const int ratios[][2] = {{0,0},{1,1},{4,3},{3,4},{16,9},{9,16},{3,2},{2,3}};
        if (p.crop_ratio_idx >= 1 && p.crop_ratio_idx <= 7) {
            // 比例预设：取该比例的最大居中窗口（比例裁剪与缩放无关，几何等价）
            int a = ratios[p.crop_ratio_idx][0], b = ratios[p.crop_ratio_idx][1];
            int tw = std::min(img.w, img.h * a / b), th = tw * b / a;
            out = Image(tw, th);
            int cx = (img.w - tw) / 2, cy = (img.h - th) / 2;
            for (int y = 0; y < th; ++y)
                memcpy(out.row(y), img.row(cy + y) + cx * 4, (size_t)tw * 4);
        } else {
            // 自定义：输出宽×高为固定尺寸 —— 等比缩放铺满目标（小图放大、大图缩小），
            // 再居中裁掉溢出，输出恰好 crop_w × crop_h（不变形）
            if (p.crop_w > 0 && p.crop_h > 0)
                out = fit_center_crop(img, p.crop_w, p.crop_h, VAnchor::Center);
            else
                out = img; // 参数异常：原图直出
        }
        std::wstring ext = p.out_fmt == 1 ? L".png" : L".jpg";
        std::wstring dst = (fs_out / (fs_src.stem().wstring() + ext)).wstring();
        save_image(dst, out, p.out_fmt == 1 ? Fmt::PNG : Fmt::JPG, p.jpg_q);
        r.ok = true; r.out_path = dst;
        return finish(std::move(r), Fmt::JPG, &out);
    }

    case MOD_FORMAT_CONVERT: {
        if (p.rotate_deg) rotate_image(img, img, p.rotate_deg);
        if (p.out_fmt == 3) {
            // PDF：像素编码为 JPEG 内嵌（DCTDecode），页面尺寸 = 像素尺寸（72dpi 基准）
            auto jpg = encode_jpg(img, 95);
            {
                // 合并会话激活（UI 在 start 时 pdf_merge_begin 且多图）：登记分片，
                // 不逐张写文件；完成哨兵时 pdf_merge_finish 统一合成单页序 PDF
                std::lock_guard<std::mutex> lk(g_pdfm_mu);
                if (!g_pdfm_order.empty()) {
                    g_pdfm_parts.push_back({src, std::move(jpg), img.w, img.h});
                    r.ok = true;
                    r.out_path = g_pdfm_dst;
                    r.error = L"已并入合并 PDF";
                    return r; // 分片未落盘，不 finish()（避免对不存在的单文件取 size）
                }
            }
            std::wstring dst = (fs_out / (fs_src.stem().wstring() + L".pdf")).wstring();
            if (!write_pdf(dst, jpg, img.w, img.h)) { r.error = L"PDF 写入失败"; return r; }
            r.ok = true; r.out_path = dst;
            return finish(std::move(r), Fmt::PDF, &img);
        }
        Fmt fmt; std::wstring ext;
        switch (p.out_fmt) {
        case 1: fmt = Fmt::PNG; ext = L".png"; break;
        case 2: fmt = Fmt::BMP; ext = L".bmp"; break;
        case 4: fmt = Fmt::TIFF; ext = L".tif"; break;
        case 5: fmt = Fmt::GIF; ext = L".gif"; break;
        case 6: fmt = Fmt::WEBP; ext = L".webp"; break;
        default: fmt = Fmt::JPG; ext = L".jpg"; break;
        }
        std::wstring dst = (fs_out / (fs_src.stem().wstring() + ext)).wstring();
        save_image(dst, img, fmt, p.jpg_q);
        r.ok = true; r.out_path = dst;
        return finish(std::move(r), fmt, &img);
    }

    case MOD_ROTATE: {
        // 仅旋转角度，输出保持原图片格式（JPG/PNG/BMP/TIFF/GIF/WebP 可原样编码；
        // PDF/HEIC 等解码后无法原格式回写，明确报错而不是悄悄换格式）
        if (!p.rotate_deg) { r.error = L"未选择旋转角度"; return r; }
        rotate_image(img, img, p.rotate_deg);
        std::wstring e = ext_lower(src);
        Fmt fmt; bool known = true;
        if (e == L"jpg" || e == L"jpeg") fmt = Fmt::JPG;
        else if (e == L"png") fmt = Fmt::PNG;
        else if (e == L"bmp") fmt = Fmt::BMP;
        else if (e == L"tif" || e == L"tiff") fmt = Fmt::TIFF;
        else if (e == L"gif") fmt = Fmt::GIF;
        else if (e == L"webp") fmt = Fmt::WEBP;
        else known = false;
        if (!known) { r.error = L"该格式不支持原格式旋转输出（仅支持 JPG/PNG/BMP/TIFF/GIF/WebP）"; return r; }
        // 保留原扩展名（.jpeg/.tif 等写法原样延续）
        std::wstring dst = (fs_out / (fs_src.stem().wstring() + fs_src.extension().wstring())).wstring();
        save_image(dst, img, fmt, p.jpg_q);
        r.ok = true; r.out_path = dst;
        return finish(std::move(r), fmt, &img);
    }

    case MOD_FLIP: {
        Image out;
        flip_image(img, out, p.flip == 1);
        if (p.rotate_deg) rotate_image(out, out, p.rotate_deg);
        std::wstring ext = p.out_fmt == 1 ? L".png" : (ext_lower(src) == L"png" ? L".png" : L".jpg");
        std::wstring dst = (fs_out / (fs_src.stem().wstring() + ext)).wstring();
        save_image(dst, out, ext == L".png" ? Fmt::PNG : Fmt::JPG, p.jpg_q);
        r.ok = true; r.out_path = dst;
        return finish(std::move(r), Fmt::JPG, &out);
    }

    case MOD_STRIP_META: {
        std::vector<uint8_t> raw;
        if (!read_file(src, raw)) { r.error = L"读取失败"; return r; }
        std::vector<uint8_t> data;
        std::wstring e = ext_lower(src);
        if (is_jpeg(src)) data = jpeg_strip(raw, true);
        else if (e == L"png") data = png_strip_meta(raw);
        else { r.error = L"仅支持 JPG/PNG 清除元数据"; return r; }
        std::wstring dst = (fs_out / fs_src.filename()).wstring();
        write_file(std::filesystem::path(dst), data);
        r.ok = true; r.out_path = dst;
        return finish(std::move(r), Fmt::JPG, nullptr);
    }

    case MOD_ICON_EXPORT: {
        static const int sizes[] = {16, 32, 48, 64, 128, 256};
        std::wstring dst = (fs_out / (fs_src.stem().wstring() + L".ico")).wstring();
        if (!write_ico(dst, img, sizes, 6)) { r.error = L"ICO 写入失败"; return r; }
        r.ok = true; r.out_path = dst;
        return finish(std::move(r), Fmt::ICO, nullptr);
    }

    default:
        r.error = L"该模块在阶段 2 实现";
        return r;
    }
}

} // namespace sp
