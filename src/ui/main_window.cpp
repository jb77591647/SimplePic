#include "main_window.h"
#include "../ops/ops.h"
#include "../codec/codec.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <commctrl.h>
#include <filesystem>
#include <system_error>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <deque>
#include <set>
#include <mutex>
#include <map>
#include <condition_variable>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "msimg32.lib") // AlphaBlend（品牌卡 LOGO 位图）
#pragma comment(lib, "advapi32.lib") // 注册表（右键菜单设置）

namespace sp {

// ---------- 调色板（浅色定稿 v6） ----------
static const COLORREF C_PURPLE     = RGB(0x53, 0x4A, 0xB7);
static const COLORREF C_PURPLE_BG  = RGB(0xE7, 0xE4, 0xF7);
static const COLORREF C_SIDE_BG    = RGB(0xF1, 0xF2, 0xF6); // 页面背景（侧栏+工作区+面板底）
static const COLORREF C_SIDE_SEP   = RGB(0xE8, 0xE7, 0xE2);
static const COLORREF C_PANEL_BG   = RGB(0xF1, 0xF2, 0xF6); // 参数面板底=页面背景
static const COLORREF C_TOOL_STRIP = RGB(0xEE, 0xED, 0xF8);
static const COLORREF C_CARD_BG    = RGB(0xF8, 0xF9, 0xFD); // 卡片统一色
static const COLORREF C_CARD       = RGB(0xF8, 0xF9, 0xFD); // 卡片统一色（别名）
static const COLORREF C_CARD_BD    = RGB(0xE7, 0xE5, 0xDE);
static const COLORREF C_GREEN_BG   = RGB(0xEA, 0xF7, 0xEF);
static const COLORREF C_GREEN      = RGB(0x16, 0xA3, 0x4A);
static const COLORREF C_RED        = RGB(0xDC, 0x26, 0x26);
static const COLORREF C_AMBER      = RGB(0xB4, 0x5F, 0x06);
static const COLORREF C_GRAY       = RGB(0x9C, 0xA3, 0xAF);
static const COLORREF C_TEXT       = RGB(0x1F, 0x29, 0x37);
static const COLORREF C_TEXT2      = RGB(0x6B, 0x72, 0x80);
static const COLORREF C_WHITE      = RGB(0xFF, 0xFF, 0xFF);

static COLORREF group_color(int gi) {
    static const COLORREF cs[] = {
        RGB(0x25, 0x63, 0xEB), // 证件照相关 蓝
        RGB(0xD9, 0x77, 0x06), // 体积压缩 琥珀
        RGB(0x0D, 0x94, 0x88), // 编辑转换 青绿
        RGB(0xD8, 0x5A, 0x30), // 其他 珊瑚橙
        RGB(0x6B, 0x72, 0x80), // 系统 灰
    };
    return cs[gi % 5];
}

// ---------- 常量 ----------
enum {
    IDC_SIDEBAR = 100,
    IDC_BTN_ADD, IDC_BTN_ADDDIR, IDC_BTN_CLEAR,
    IDC_LIST,
    IDC_BTN_RUN, IDC_BTN_RUN_SEL, IDC_PROGRESS, IDC_STATUS,
    IDC_PARAM_PANEL = 1200,
    IDC_PARAM_BASE = 1000, // 动态参数控件基址
    IDC_PARAM_SCROLL = 700, // 参数区滚动条（SCROLLBAR 子控件）
};

// 文件列表显示方式
enum { VM_LIST = 0, VM_THUMB_S, VM_THUMB_M, VM_THUMB_L };

static const UINT WM_SP_ITEM = WM_APP + 1; // wParam=idx, lParam=ItemState
static const UINT WM_SP_STITCH_DONE = WM_APP + 2; // lParam=ItemResult*（拼接结果）
static const UINT WM_SP_PREVIEW_DECODED = WM_APP + 3; // lParam=PreviewDecodeResult*（预览解码完成）
static const UINT WM_SP_LIVE_EST = WM_APP + 4;        // lParam=LiveEstResult*（智能压缩逐文件实测完成）
static const UINT WM_SP_THUMB_READY = WM_APP + 5;     // wParam=gen, lParam=ThumbResult*（缩略图异步解码完成）

// 缩略图异步解码：后台线程解码 → UI 线程收结果并按（路径，尺寸）缓存。
// 切换 小/中/大、增删文件不再在 UI 线程同步解码原图（此前卡顿的根因）
struct ThumbResult { std::wstring path; int sz; Image img; }; // img 已缩放并贴边为 sz×sz
struct ThumbJob { int gen; std::wstring path; int sz; };
static std::map<std::wstring, std::map<int, HBITMAP>> g_thumb_cache; // 路径 → 尺寸 → DIB（会话级缓存）
static std::deque<ThumbJob> g_thumb_jobs;
static std::mutex g_thumb_mtx;
static std::condition_variable g_thumb_cv;
static std::thread g_thumb_worker;
static std::atomic<int> g_thumb_gen{0}; // 视图世代：切换/重建 +1，旧世代解码结果作废
static bool g_thumb_quit = false;
static std::atomic<bool> g_stitch_running{false}; // 拼接专用线程执行中

struct ModuleDef {
    const wchar_t* group;
    const wchar_t* name;
    const wchar_t* desc;
    int id;
    bool phase2;
};

static const ModuleDef kModules[] = {
    {L"体积压缩", L"压缩到指定大小",  L"二分搜索质量 · 目标体积精确达标",              MOD_COMPRESS_TARGET, false},
    {L"体积压缩", L"智能压缩",        L"滑块实时预览体积 · 确认后批量执行",            MOD_COMPRESS_LIVE, false},
    {L"体积压缩", L"批量增加体积",    L"元数据填充 · 像素零改动",                      MOD_FILL_SIZE, false},
    {L"证件照相关", L"证件照转换",    L"标准规格缩放居中裁剪 · JPG q95 300DPI",        MOD_ID_PHOTO, false},
    {L"证件照相关", L"修改 DPI",      L"直改元数据 · 宽高像素值不变",                  MOD_SET_DPI, false},
    {L"编辑转换", L"居中裁剪",        L"中心截取 · 8 种预设比例 + 自定义",             MOD_CENTER_CROP, false},
    {L"编辑转换", L"格式转换",        L"多格式输出 · EXIF 方向自动纠正",               MOD_FORMAT_CONVERT, false},
    {L"编辑转换", L"旋转",            L"保持原格式 · 仅旋转角度",                      MOD_ROTATE, false},
    {L"编辑转换", L"翻转镜像",        L"水平 / 垂直镜像",                              MOD_FLIP, false},
    {L"编辑转换", L"文字水印",        L"字体/颜色/角度/间距 · 平铺绘制",               MOD_WATERMARK, false},
    {L"编辑转换", L"图片水印",        L"水印图叠加 · 大小/透明度/布局/间距",           MOD_WATERMARK_IMG, false},
    {L"其他",     L"长图拼接",        L"纵向 / 横向 / 九宫格拼接多张图片",             MOD_STITCH, false},
    {L"其他",     L"图标导出",        L"多尺寸 ICO：16/32/48/64/128/256",              MOD_ICON_EXPORT, false},
    {L"系统",     L"清除元数据",      L"剥离 EXIF/GPS/XMP/ICC/COM · 像素零改动",       MOD_STRIP_META, false},
    {L"系统",     L"右键菜单设置",    L"资源管理器右键菜单自由组合",                   MOD_SET_DPI + 100, true},
};

static int group_index(const wchar_t* g) {
    static const wchar_t* groups[] = {L"证件照相关", L"体积压缩", L"编辑转换", L"其他", L"系统"};
    for (int i = 0; i < 5; ++i) if (wcscmp(groups[i], g) == 0) return i;
    return 4;
}

// ---------- 全局状态 ----------
struct ParamCtl { HWND hwnd; int type; int module; int y0 = 0; }; // y0=创建时的基准 y（滚动偏移用）
enum { CTL_LABEL, CTL_COMBO, CTL_CHECK, CTL_EDIT, CTL_EDITTEXT, CTL_SLIDER, CTL_PATHBTN, CTL_COLORS, CTL_SEP };

static HWND g_hwnd = nullptr;
static HWND g_sidebar, g_list, g_run_btn, g_run_sel_btn, g_progress, g_status, g_param_panel;
static std::vector<ParamCtl> g_ctls;
static std::vector<FileItem> g_files;
static std::vector<int> g_active_idx; // 本次提交管线处理的索引集合（进度/收尾统计用）
static bool g_pdf_merge_pending = false;        // 多图转 PDF 合并会话待收尾
static std::wstring g_pdf_merge_dst;            // 合并 PDF 输出路径
// 智能压缩：选中图 720px 预览在当前质量下实测的每像素字节数（列表估算外推用）
static double g_live_bpp = 0;
static int g_live_bpp_q = -1;
static std::wstring g_live_bpp_path;
// 智能压缩逐文件实测缓存：path → (质量, 估算KB；<0=实测失败)。定义细节见 LiveEstReq 段
static std::map<std::wstring, std::pair<int, long long>> g_est_cache;
static std::wstring g_wm_img_path; // 图片水印路径（对话框选择，不入控件）
static int g_wm_color_sel = 0;     // 水印颜色：色块选中索引（0~7，与 ops 8 色表对齐）
static int g_module = MOD_COMPRESS_TARGET; // 默认打开：压缩到指定大小
static int g_sidebar_sel = -1; // 侧边栏当前有效选中（listbox 索引）
static int g_dpi = 96;
static int g_last_cx = 0, g_last_cy = 0; // 上次客户区尺寸（WM_SIZE 去重）
static int g_name_col = 0; // 列0宽（layout_controls 计算，paint_workspace 自绘列标题用）
static int g_cw[7] = {};   // 7列宽（0文件名 1尺寸 2原体积 3预计输出 4状态 5输出体积 6移除✕）
static int g_cw_applied[7] = {}; // 已下发到 ListView 的列宽（布局去重：仅变化列才 SetColumnWidth，拖拽窗体不卡顿）
static int g_cw_user[6] = {0}; // 用户拖拽设定的列宽（0=未设定，布局仍自动计算）
static int g_col_drag = -1;    // 正在拖拽右边界调整宽度的列（0..5），-1=无
static int g_col_drag_x0 = 0;  // 拖拽起点鼠标 x
static int g_col_drag_w0 = 0;  // 拖拽起点该列宽度
static int g_view_mode = VM_LIST;      // 当前显示方式
static HIMAGELIST g_imglist = nullptr; // 缩略图 ImageList（缩略图模式）
static int S(int v) { return v * g_dpi / 96; } // 逻辑px → 物理px
// 窗口垂直非客户区高度（标题栏+上下边框，随 DPI 缩放）。优先用 AdjustWindowRectExForDpi
// 按 g_dpi 实测（PMv2 下非 ForDpi 版本用系统 DPI，多屏不同缩放时会算错），旧系统回退 AdjustWindowRectEx
static int nc_frame_cy(DWORD style, int dpi) {
    RECT fr = {0, 0, 0, 0};
    using FnAdjDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    auto adj = (FnAdjDpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    if (adj) adj(&fr, style, FALSE, 0, (UINT)dpi);
    else AdjustWindowRectEx(&fr, style, FALSE, 0);
    return fr.bottom - fr.top;
}
static Pipeline g_pipe;
static HFONT g_font, g_font_bold, g_font_small, g_font_h1, g_font_num, g_font_pv;
static HBRUSH g_brush_white, g_brush_panel, g_brush_side, g_brush_sep;

static const wchar_t* kExtFilter[] = {L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"tif", L"tiff",
                                      L"pdf", L"heic", L"heif", L"hif", L"avif"};

static bool is_image_file(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring e = name.substr(dot + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    for (auto* f : kExtFilter) if (e == f) return true;
    return false;
}

static int module_index_by_id(int id) {
    for (int i = 0; i < (int)(sizeof kModules / sizeof kModules[0]); ++i)
        if (kModules[i].id == id) return i;
    return 0;
}

// ---------- 临时调试日志 ----------
static void dbg_log(const wchar_t* s);
// ---------- 前向声明 ----------
static void refresh_preview();
static void layout_controls(HWND hwnd);
static void add_thumb_entry(int idx);
static void thumb_request(int gen, const std::wstring& path, int sz);
static void thumb_shutdown();
static void rebuild_thumbs();

// ---------- GDI 小工具 ----------
static void fill_rect(HDC dc, RECT rc, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &rc, b);
    DeleteObject(b);
}

static void fill_round(HDC dc, RECT rc, COLORREF c, int r) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HBRUSH ob = (HBRUSH)SelectObject(dc, b);
    HPEN op = (HPEN)SelectObject(dc, p);
    RoundRect(dc, rc.left, rc.top, rc.right + 1, rc.bottom + 1, r, r);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void fmt_size(long long b, wchar_t* buf, size_t n);       // 体积格式化（≥1000 KB 显示 MB）
static void fmt_est_kb(long long kb, wchar_t* buf, size_t n);    // KB 估算值格式化（≥1000 KB 显示 MB）

static void draw_text(HDC dc, const wchar_t* s, RECT rc, COLORREF c, HFONT f, UINT flags = DT_SINGLELINE | DT_VCENTER | DT_LEFT) {
    HFONT of = (HFONT)SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &rc, flags);
    SelectObject(dc, of);
}

// 居中 ✕（两根斜线，避免字体缺 U+2715 字形）；penw>0 时用指定线宽（加粗）
static void draw_x_mark(HDC dc, RECT rc, COLORREF c, int penw = 0) {
    int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2, r = S(4); // 叉形 0.8×（原 S(5)），正中心缩小
    HPEN p = CreatePen(PS_SOLID, penw > 0 ? penw : std::max(1, S(1)), c);
    HPEN op = (HPEN)SelectObject(dc, p);
    MoveToEx(dc, cx - r, cy - r, nullptr); LineTo(dc, cx + r, cy + r);
    MoveToEx(dc, cx + r, cy - r, nullptr); LineTo(dc, cx - r, cy + r);
    SelectObject(dc, op);
    DeleteObject(p);
}

// ✕ 移除按钮：常态透明背景 + 黑色加粗 ✕；悬停时淡红底 + 红叉凸显
static void draw_x_button(HDC dc, RECT box, bool hover) {
    if (hover) fill_round(dc, box, RGB(0xFD, 0xE3, 0xE3), S(6));
    draw_x_mark(dc, box, hover ? RGB(0xD9, 0x30, 0x25) : RGB(0x00, 0x00, 0x00),
                std::max(2, S(2)));
}

// ✕ 移除按钮矩形：列表模式=第7列内居中 S(22) 方块（严格等于绘制块，杜绝点击越界），
// 缩略图模式=图标右上角 S(20) 方块。自绘/悬停命中/点击命中三处共用，保证视觉与热区一致
static bool x_rect_for_row(HWND list, int row, RECT* out) {
    if (row < 0) return false;
    if (g_view_mode == VM_LIST) {
        RECT rc;
        if (!ListView_GetSubItemRect(list, row, 6, LVIR_BOUNDS, &rc)) return false;
        int bs = S(22);
        int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
        *out = {cx - bs / 2, cy - bs / 2, cx + bs / 2, cy + bs / 2};
        return true;
    }
    RECT ir;
    if (!ListView_GetItemRect(list, row, &ir, LVIR_ICON)) return false;
    int bs = S(20);
    *out = RECT{ir.right - bs, ir.top, ir.right, ir.top + bs};
    return true;
}

// ---------- 列表悬停跟踪（✕ 凸显动画态） ----------
static int g_x_hover_row = -1;     // 当前悬停 ✕ 的行（-1=无）
static bool g_list_track = false;  // TrackMouseEvent 已注册
static WNDPROC g_list_oldproc = nullptr;

static LRESULT CALLBACK list_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_MOUSEMOVE: {
        DWORD ppos = GetMessagePos();
        POINT pt = {(short)LOWORD(ppos), (short)HIWORD(ppos)};
        ScreenToClient(h, &pt);
        int new_hover = -1;
        LVHITTESTINFO ht = {};
        ht.pt = pt;
        if (ListView_HitTest(h, &ht) >= 0 && ht.iItem < (int)g_files.size()) {
            RECT xr;
            if (x_rect_for_row(h, ht.iItem, &xr) && PtInRect(&xr, pt)) new_hover = ht.iItem;
        }
        if (new_hover != g_x_hover_row) {
            RECT r;
            if (g_x_hover_row >= 0 && x_rect_for_row(h, g_x_hover_row, &r))
                InvalidateRect(h, &r, TRUE); // 旧按钮退回常态（TRUE：擦除背景，杜绝旧像素残影）
            g_x_hover_row = new_hover;
            if (g_x_hover_row >= 0 && x_rect_for_row(h, g_x_hover_row, &r))
                InvalidateRect(h, &r, TRUE); // 新按钮凸显
            SetCursor(LoadCursorW(nullptr, g_x_hover_row >= 0 ? IDC_HAND : IDC_ARROW));
        }
        if (!g_list_track) {
            TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            g_list_track = true;
        }
        break;
    }
    case WM_MOUSELEAVE:
        g_list_track = false;
        if (g_x_hover_row >= 0) {
            RECT r;
            if (x_rect_for_row(h, g_x_hover_row, &r)) InvalidateRect(h, &r, TRUE);
            g_x_hover_row = -1;
        }
        break;
    }
    return CallWindowProcW(g_list_oldproc, h, m, wp, lp);
}

// ---------- 参数面板 ----------
static int g_panel_y = 30; // 控件布局游标（组框标题下方起始）
static HWND g_param_scrollbar = nullptr; // SCROLLBAR 子控件（参数溢出时显示）
static int g_param_scroll = 0;           // 当前滚动偏移 px
static int g_param_scroll_max = 0;       // 最大滚动 px
static void layout_param_scroll(); // 参数滚动布局（定义在 preview_card_rect 之后）

static bool param_wheel(int delta); // 滚轮驱动参数区滚动（定义在 layout_param_scroll 之后）

// 下拉框/滑块子类化：悬停滚轮不误改选中值/滑块位置——直接滚动参数区；
// 仅当下拉列表展开时放行原过程（滚轮滚动展开的选项列表，TRACKBAR 对该消息返回 0 不受影响）
static LRESULT CALLBACK eat_wheel_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_MOUSEWHEEL) {
        WNDPROC op = (WNDPROC)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0))
            return CallWindowProcW(op, h, m, wp, lp);
        param_wheel((short)HIWORD(wp));
        return 0;
    }
    WNDPROC op = (WNDPROC)GetWindowLongPtrW(h, GWLP_USERDATA);
    return CallWindowProcW(op, h, m, wp, lp);
}
static void subclass_eat_wheel(HWND h) {
    WNDPROC op = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)eat_wheel_proc);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)op);
}

static void add_ctl(int type, const wchar_t* text, const std::vector<std::wstring>& items, int ctrl_id, int col = 0) {
    // col=0 整行宽；col=2 成对首列（半宽，正常推进行游标）；col=1 右列（半宽，与上一个控件同行，不推进游标）
    const int x = S(18);  // 参数卡片内边距：卡体 S(10)..S(280)，控件左右各缩进 8px
    const int w = S(254);
    const int cw = col ? (w - S(10)) / 2 : w;   // 半宽列：两列间隔 S(10)
    const int cx = (col == 1) ? x + cw + S(10) : x; // 右列起点
    HWND h = nullptr;
    if (type == CTL_LABEL) {
        int y = g_panel_y + 4;
        h = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, S(20),
                          g_param_panel, (HMENU)(INT_PTR)ctrl_id, nullptr, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_ctls.push_back({h, CTL_LABEL, ctrl_id, y});
        g_panel_y += S(28);
    } else if (type == CTL_COMBO) {
        // col=1：与上一个控件同行（该行总高 S(54)），回退到行首摆放，不推进游标；col=2 正常从行首开始
        int ly = (col == 1) ? g_panel_y - S(54) : g_panel_y;
        HWND lab = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, cx, ly, cw, S(18),
                                 g_param_panel, (HMENU)(INT_PTR)ctrl_id, nullptr, nullptr);
        SendMessageW(lab, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_ctls.push_back({lab, CTL_LABEL, ctrl_id, ly});
        HWND cb = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                cx, ly + S(20), cw, S(200), g_param_panel,
                                (HMENU)(INT_PTR)(ctrl_id + 500), nullptr, nullptr);
        SendMessageW(cb, WM_SETFONT, (WPARAM)g_font, TRUE);
        for (auto& s : items) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)s.c_str());
        SendMessageW(cb, CB_SETCURSEL, 0, 0);
        subclass_eat_wheel(cb); // 悬停滚轮不改选中值
        g_ctls.push_back({cb, CTL_COMBO, ctrl_id, ly + S(20)});
        if (col != 1) g_panel_y += S(54);
    } else if (type == CTL_CHECK) {
        int y = g_panel_y + 2;
        h = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                          x, y, w, S(22), g_param_panel, (HMENU)(INT_PTR)ctrl_id, nullptr, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
        g_ctls.push_back({h, CTL_CHECK, ctrl_id, y});
        g_panel_y += S(30);
    } else if (type == CTL_EDIT) {
        // col=1：与上一个控件（组合框，行高 S(54)）同行，回退到行首摆放，不推进游标
        int ly = (col == 1) ? g_panel_y - S(54) : g_panel_y;
        HWND lab = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, cx, ly, cw, S(18),
                                 g_param_panel, (HMENU)(INT_PTR)ctrl_id, nullptr, nullptr);
        SendMessageW(lab, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_ctls.push_back({lab, CTL_LABEL, ctrl_id, ly});
        const wchar_t* init = items.empty() ? L"" : items[0].c_str();
        HWND ed = CreateWindowW(L"EDIT", init, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                                cx, ly + S(20), (col == 1) ? S(80) : S(120), S(24), g_param_panel,
                                (HMENU)(INT_PTR)(ctrl_id + 500), nullptr, nullptr);
        SendMessageW(ed, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_ctls.push_back({ed, CTL_EDIT, ctrl_id, ly + S(20)});
        if (col != 1) g_panel_y += S(54); // 与组合框行高一致，两列成对时对齐
    } else if (type == CTL_EDITTEXT) {
        // 文本输入（无 ES_NUMBER）：水印文字等；col=2 成对首列 / col=1 右列，与 CTL_EDIT 同规则
        int ly = (col == 1) ? g_panel_y - S(54) : g_panel_y;
        HWND lab = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, cx, ly, cw, S(18),
                                 g_param_panel, (HMENU)(INT_PTR)ctrl_id, nullptr, nullptr);
        SendMessageW(lab, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_ctls.push_back({lab, CTL_LABEL, ctrl_id, ly});
        const wchar_t* init = items.empty() ? L"" : items[0].c_str();
        HWND ed = CreateWindowW(L"EDIT", init, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                cx, ly + S(20), col ? cw : w, S(24), g_param_panel,
                                (HMENU)(INT_PTR)(ctrl_id + 500), nullptr, nullptr);
        SendMessageW(ed, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_ctls.push_back({ed, CTL_EDITTEXT, ctrl_id, ly + S(20)});
        if (col != 1) g_panel_y += S(54); // 与组合框/输入框行高一致
    } else if (type == CTL_SLIDER) {
        HWND sl = CreateWindowW(TRACKBAR_CLASSW, L"",
                                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
                                x, g_panel_y, w, S(30), g_param_panel,
                                (HMENU)(INT_PTR)(ctrl_id + 500), nullptr, nullptr);
        SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELPARAM(1, 95));
        SendMessageW(sl, TBM_SETPOS, TRUE, 80);
        subclass_eat_wheel(sl); // 悬停滚轮不改滑块位置
        g_ctls.push_back({sl, CTL_SLIDER, ctrl_id, g_panel_y});
        g_panel_y += S(36);
    } else if (type == CTL_PATHBTN) {
        // 文件选择按钮：BN_CLICKED(id=base+500) 由 WM_COMMAND 处理
        HWND b = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE,
                               x, g_panel_y, w, S(26), g_param_panel,
                               (HMENU)(INT_PTR)(ctrl_id + 500), nullptr, nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_ctls.push_back({b, CTL_PATHBTN, ctrl_id, g_panel_y});
        g_panel_y += S(32);
    } else if (type == CTL_SEP) {
        // 分组横线：如文字水印与图片水印之间
        g_panel_y += S(8);
        HWND sep = CreateWindowW(L"STATIC", nullptr, WS_CHILD | WS_VISIBLE,
                                 x, g_panel_y, w, S(2), g_param_panel,
                                 (HMENU)(INT_PTR)ctrl_id, nullptr, nullptr);
        g_ctls.push_back({sep, CTL_SEP, ctrl_id, g_panel_y});
        g_panel_y += S(12);
    } else if (type == CTL_COLORS) {
        // 水印颜色：常见 8 色（黑/白/红/橙/黄/绿/蓝/紫），一行 8 个，点击选择。
        // 色块 = BS_OWNERDRAW 按钮，id=40..47（HMENU 直接用，与参数 base 无关）；
        // WM_DRAWITEM / BN_CLICKED 由 panel_proc 转发到主窗口处理。
        HWND lab = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, g_panel_y, w, S(18),
                                 g_param_panel, (HMENU)(INT_PTR)ctrl_id, nullptr, nullptr);
        SendMessageW(lab, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_ctls.push_back({lab, CTL_LABEL, ctrl_id, g_panel_y});
        g_panel_y += S(20);
        const int chip = S(22), gap = S(5), per_row = 8;
        for (int i = 0; i < 8; ++i) {
            int cx2 = x + i * (chip + gap);
            int bid = 40 + i;
            HWND b = CreateWindowW(L"BUTTON", nullptr, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                   cx2, g_panel_y, chip, chip, g_param_panel,
                                   (HMENU)(INT_PTR)bid, nullptr, nullptr);
            g_ctls.push_back({b, CTL_COLORS, bid, g_panel_y});
        }
        g_panel_y += S(28);
    }
}

// 证件照：仅当选中"自定义宽高"时允许编辑宽高输入框
static void update_idphoto_custom_state() {
    bool custom = false;
    for (auto& c : g_ctls)
        if (c.type == CTL_COMBO && c.module == 1)
            custom = (int)SendMessageW(c.hwnd, CB_GETCURSEL, 0, 0) == 5;
    for (auto& e : g_ctls)
        if (e.type == CTL_EDIT && (e.module == 11 || e.module == 12))
            EnableWindow(e.hwnd, custom ? TRUE : FALSE);
}

// 居中裁剪：仅当选中"自定义"时允许编辑输出宽/高输入框
static void update_crop_custom_state() {
    bool custom = false;
    for (auto& c : g_ctls)
        if (c.type == CTL_COMBO && c.module == 6)
            custom = (int)SendMessageW(c.hwnd, CB_GETCURSEL, 0, 0) == 7;
    for (auto& e : g_ctls)
        if (e.type == CTL_EDIT && (e.module == 13 || e.module == 14))
            EnableWindow(e.hwnd, custom ? TRUE : FALSE);
}

// 格式转换：仅当输出格式=PDF（下拉第 7 项）时允许勾选"合并为单个 PDF"
static void update_pdfmerge_state() {
    bool pdf = false;
    for (auto& c : g_ctls)
        if (c.type == CTL_COMBO && c.module == 7)
            pdf = (int)SendMessageW(c.hwnd, CB_GETCURSEL, 0, 0) == 6;
    for (auto& e : g_ctls)
        if (e.type == CTL_CHECK && e.module == 15)
            EnableWindow(e.hwnd, pdf ? TRUE : FALSE);
}

// 压缩到指定大小：仅当下拉选"自定义"（第 5 项）时允许编辑自定义目标体积
static void update_target_custom_state() {
    bool custom = false;
    for (auto& c : g_ctls)
        if (c.type == CTL_COMBO && c.module == 2)
            custom = (int)SendMessageW(c.hwnd, CB_GETCURSEL, 0, 0) == 4;
    for (auto& e : g_ctls)
        if (e.type == CTL_EDIT && e.module == 16)
            EnableWindow(e.hwnd, custom ? TRUE : FALSE);
}

// ---------- 右键菜单注册（HKCU，免提权） ----------
// 图片扩展名白名单（与 kExtFilter 拖入白名单一致）：仅这些扩展名显示右键菜单。
// 注意：静态动词直接挂在 HKCU\...\.<ext>\shell 下在 Win10/11 不可靠（扩展名
// UserChoice 为 AppX 时 Explorer 不加载该键）——统一注册在 *\\shell 下，
// 用 AppliesTo(AQS) 按文件名模式过滤到支持的扩展名
static const wchar_t* const kShellExts[] = {L".jpg", L".jpeg", L".png", L".gif", L".bmp",
                                            L".webp", L".tif", L".tiff", L".pdf",
                                            L".heic", L".heif", L".hif", L".avif"};

static std::wstring shell_menu_key(bool dir, const wchar_t* ext) {
    std::wstring k = L"Software\\Classes\\";
    if (dir) k += L"Directory\\shell\\SimplePic";
    else if (ext) k += std::wstring(ext) + L"\\shell\\SimplePic"; // 旧版：按扩展名注册（已废弃）
    else k += L"*\\shell\\SimplePic";
    return k;
}

// AppliesTo 过滤表达式：System.FileName 通配（与扩展名键方案不同，此法在
// UserChoice 为 AppX 的扩展名上依然生效）；多扩展名用 OR 连接
static std::wstring build_applies_to() {
    std::wstring q;
    for (size_t i = 0; i < sizeof(kShellExts) / sizeof(kShellExts[0]); ++i) {
        if (i) q += L" OR ";
        q += std::wstring(L"System.FileName:\"*") + kShellExts[i] + L"\"";
    }
    return q;
}

static bool key_exists(const std::wstring& key) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

static bool get_shell_menu(bool dir) {
    if (dir) return key_exists(shell_menu_key(true, nullptr));
    if (key_exists(shell_menu_key(false, nullptr))) return true; // 当前方案 / 更早的全类型遗留
    for (const wchar_t* e : kShellExts)
        if (key_exists(shell_menu_key(false, e))) return true; // 上一版按扩展名的遗留
    return false;
}

static void set_shell_menu(bool dir, bool enable) {
    if (!enable) {
        RegDeleteTreeW(HKEY_CURRENT_USER, shell_menu_key(dir, nullptr).c_str());
        if (!dir)
            for (const wchar_t* e : kShellExts)
                RegDeleteTreeW(HKEY_CURRENT_USER, shell_menu_key(false, e).c_str());
        return;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring icon = std::wstring(L"\"") + exe + L"\",0";       // 菜单项左侧图标
    std::wstring cmd = std::wstring(L"\"") + exe + L"\" \"%1\"";   // 命中后启动命令
    auto write_verb = [&](const std::wstring& key, const wchar_t* label, const std::wstring& applies_to) {
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (key + L"\\command").c_str(), 0, nullptr,
                            0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
        }
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr,
                            0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)label,
                           (DWORD)((wcslen(label) + 1) * sizeof(wchar_t)));
            RegSetValueExW(k, L"Icon", 0, REG_SZ, (const BYTE*)icon.c_str(),
                           (DWORD)((icon.size() + 1) * sizeof(wchar_t)));
            if (!applies_to.empty())
                RegSetValueExW(k, L"AppliesTo", 0, REG_SZ, (const BYTE*)applies_to.c_str(),
                               (DWORD)((applies_to.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
        }
    };
    if (dir) {
        write_verb(shell_menu_key(true, nullptr), L"用 SimplePic 打开", L"");
    } else {
        // 图片：注册在 * 下 + AppliesTo 过滤（对 AppX 关联的扩展名也生效）
        write_verb(shell_menu_key(false, nullptr), L"用 SimplePic 处理图片", build_applies_to());
        for (const wchar_t* e : kShellExts) // 清掉上一版按扩展名的遗留键
            RegDeleteTreeW(HKEY_CURRENT_USER, shell_menu_key(false, e).c_str());
    }
}

// 旧版注册（全文件类型 / 按扩展名）→ 新方案（* + AppliesTo）迁移：
// 启动与打开设置页时自愈；只要发现旧痕迹就按当前开关状态重写
static void migrate_shell_menu() {
    bool file_on = key_exists(shell_menu_key(false, nullptr));
    if (!file_on)
        for (const wchar_t* e : kShellExts)
            if (key_exists(shell_menu_key(false, e))) { file_on = true; break; }
    if (!file_on) return; // 从未启用过文件右键菜单
    bool dir_on = key_exists(shell_menu_key(true, nullptr));
    set_shell_menu(false, false); // 清掉所有旧形态
    set_shell_menu(false, true);  // 写入新形态（* + AppliesTo + 图标）
    if (dir_on) set_shell_menu(true, true); // 文件夹菜单顺带补图标
}

// 智能压缩滑块读数：质量 + 选中图实测估算
static long long est_kb_for(const FileItem& f, const Params& p); // 定义在下方
static void collect_params(Params& p);                           // 定义在下方
static void start_live_est_all();                                // 定义在下方（逐文件实测派发）
static void update_live_label() {
    for (auto& c : g_ctls) {
        if (c.type == CTL_LABEL && c.module == 27) {
            Params p;
            collect_params(p);
            int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
            wchar_t buf[96], kbbuf[24];
            if (sel >= 0 && sel < (int)g_files.size() && g_files[sel].w > 0) {
                fmt_est_kb(est_kb_for(g_files[sel], p), kbbuf, 24);
                swprintf(buf, 96, L"质量 %d · 选中图 %s", p.live_q, kbbuf);
            } else {
                swprintf(buf, 96, L"质量 %d", p.live_q);
            }
            SetWindowTextW(c.hwnd, buf);
        }
    }
}

static void rebuild_param_panel() {
    for (auto& c : g_ctls) DestroyWindow(c.hwnd);
    g_ctls.clear();
    g_panel_y = S(52); // 面板标题下方

    // 销毁后先同步重绘面板（擦背景 + WM_DRAWITEM 自绘），抹净旧控件残影，
    // 再创建新控件——新控件画在干净底板上，不会与旧像素重叠
    InvalidateRect(g_param_panel, nullptr, TRUE);
    UpdateWindow(g_param_panel);

    int m = g_module;
    switch (m) {
    case MOD_ID_PHOTO:
        add_ctl(CTL_COMBO, L"证件照规格", {L"一寸 295×413", L"小一寸 260×378",
                L"二寸 413×579", L"小二寸 413×531", L"护照 390×567", L"自定义宽高"}, 1);
        add_ctl(CTL_EDIT, L"自定义宽 px（选自定义时生效）", {L"295"}, 11);
        add_ctl(CTL_EDIT, L"自定义高 px", {L"413"}, 12);
        update_idphoto_custom_state();
        break;
    case MOD_COMPRESS_TARGET:
        add_ctl(CTL_COMBO, L"目标体积", {L"100 KB", L"200 KB", L"500 KB", L"1000 KB", L"自定义"}, 2);
        add_ctl(CTL_EDIT, L"自定义目标体积 KB（小于 1 按 1 KB 处理）", {L"50"}, 16);
        add_ctl(CTL_CHECK, L"小于目标时填充元数据到达标", {}, 3);
        for (auto& c : g_ctls)
            if (c.type == CTL_CHECK && c.module == 3)
                SendMessageW(c.hwnd, BM_SETCHECK, BST_UNCHECKED, 0);
        update_target_custom_state();
        break;
    case MOD_COMPRESS_LIVE:
        add_ctl(CTL_SLIDER, L"压缩质量", {}, 20);
        add_ctl(CTL_LABEL, L"质量 80", {}, 27);       // 实时读数（update_live_label）
        add_ctl(CTL_LABEL, L"拖动滑块，预计输出与预览即时更新", {}, 0);
        break;
    case MOD_FILL_SIZE:
        add_ctl(CTL_EDIT, L"目标体积 KB", {L"2000"}, 4);
        break;
    case MOD_SET_DPI:
        add_ctl(CTL_EDIT, L"DPI 数值", {L"300"}, 5);
        break;
    case MOD_CENTER_CROP:
        add_ctl(CTL_COMBO, L"裁剪比例", {L"1:1", L"4:3", L"3:4", L"16:9", L"9:16", L"3:2", L"2:3", L"自定义"}, 6);
        add_ctl(CTL_EDIT, L"输出宽 px（选自定义时生效）", {L"800"}, 13);
        add_ctl(CTL_EDIT, L"输出高 px", {L"600"}, 14);
        update_crop_custom_state();
        break;
    case MOD_FORMAT_CONVERT:
        add_ctl(CTL_COMBO, L"输出格式", {L"JPG", L"PNG", L"BMP", L"TIFF", L"GIF", L"WebP", L"PDF"}, 7);
        add_ctl(CTL_CHECK, L"多图合并为单个 PDF（仅 PDF 输出）", {}, 15);
        update_pdfmerge_state();
        break;
    case MOD_ROTATE:
        add_ctl(CTL_COMBO, L"旋转角度", {L"0°（不旋转）", L"90° 顺时针", L"180°", L"90° 逆时针"}, 8);
        for (auto& c : g_ctls) // 默认 90° 顺时针
            if (c.type == CTL_COMBO && c.module == 8)
                SendMessageW(c.hwnd, CB_SETCURSEL, 1, 0);
        add_ctl(CTL_LABEL, L"输出保持原图片格式，仅旋转角度", {}, 0);
        break;
    case MOD_FLIP:
        add_ctl(CTL_COMBO, L"镜像方向", {L"水平翻转", L"垂直翻转"}, 9);
        break;
    case MOD_WATERMARK:
        add_ctl(CTL_EDITTEXT, L"水印文字", {L"SimplePic"}, 21, 2);
        add_ctl(CTL_EDIT, L"字号 px", {L"32"}, 22, 1);
        add_ctl(CTL_COLORS, L"颜色", {}, 23);
        add_ctl(CTL_COMBO, L"不透明度", {L"10%", L"20%", L"30%", L"40%", L"50%", L"60%",
                L"70%", L"80%", L"90%", L"100%"}, 24, 2);
        add_ctl(CTL_COMBO, L"角度", {L"0°", L"15°", L"30°", L"45°", L"90°"}, 25, 1);
        for (auto& c : g_ctls) // 不透明度默认 50%（与 Params 默认一致）
            if (c.type == CTL_COMBO && c.module == 24)
                SendMessageW(c.hwnd, CB_SETCURSEL, 4, 0);
        add_ctl(CTL_COMBO, L"布局", {L"平铺", L"居中单个", L"右下角单个"}, 26, 2);
        add_ctl(CTL_EDIT, L"平铺间距 px", {L"80"}, 28, 1);
        break;
    case MOD_WATERMARK_IMG:
        add_ctl(CTL_PATHBTN, L"选择水印图片", {}, 29);
        add_ctl(CTL_LABEL, g_wm_img_path.empty() ? L"未选择水印图片" : g_wm_img_path.c_str(), {}, 29);
        add_ctl(CTL_EDIT, L"水印大小（占图宽 %）", {L"25"}, 34);
        add_ctl(CTL_COMBO, L"不透明度", {L"10%", L"20%", L"30%", L"40%", L"50%", L"60%",
                L"70%", L"80%", L"90%", L"100%"}, 38, 2);
        add_ctl(CTL_COMBO, L"角度", {L"0°", L"15°", L"30°", L"45°", L"60°", L"90°"}, 39, 1);
        for (auto& c : g_ctls) // 不透明度默认 50%
            if (c.type == CTL_COMBO && c.module == 38)
                SendMessageW(c.hwnd, CB_SETCURSEL, 4, 0);
        add_ctl(CTL_COMBO, L"布局", {L"平铺", L"居中单个", L"左上角", L"右上角", L"左下角", L"右下角"}, 36, 2);
        add_ctl(CTL_EDIT, L"平铺间距 px", {L"80"}, 37, 1);
        break;
    case MOD_STITCH:
        add_ctl(CTL_COMBO, L"拼接方式", {L"纵向（从上到下）", L"横向（从左到右）", L"九宫格（3×3）"}, 30);
        add_ctl(CTL_COMBO, L"输出格式", {L"PNG（无损）", L"JPG（质量 95）"}, 31);
        add_ctl(CTL_LABEL, L"九宫格取列表前 9 张，按列表顺序排列", {}, 0);
        break;
    case MOD_SET_DPI + 100: { // 右键菜单设置：HKCU 注册表即时生效
        migrate_shell_menu(); // 旧版全文件类型注册自愈：迁移为按扩展名 + 补图标
        add_ctl(CTL_CHECK, L"图片文件右键菜单：用 SimplePic 处理", {}, 32);
        add_ctl(CTL_CHECK, L"文件夹右键菜单：用 SimplePic 打开", {}, 33);
        for (auto& c : g_ctls) {
            if (c.type == CTL_CHECK && c.module == 32)
                SendMessageW(c.hwnd, BM_SETCHECK, get_shell_menu(false) ? BST_CHECKED : BST_UNCHECKED, 0);
            if (c.type == CTL_CHECK && c.module == 33)
                SendMessageW(c.hwnd, BM_SETCHECK, get_shell_menu(true) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        add_ctl(CTL_LABEL, L"勾选写入注册表（HKCU），取消即移除", {}, 0);
        break;
    }
    case MOD_ICON_EXPORT:
        add_ctl(CTL_LABEL, L"导出多尺寸 ICO：16/32/48/64/128/256", {}, 0);
        break;
    case MOD_STRIP_META:
        add_ctl(CTL_LABEL, L"剥离 EXIF/GPS/XMP/ICC/COM，像素零改动", {}, 0);
        break;
    default:
        add_ctl(CTL_LABEL, L"（阶段 2 实现）", {}, 0);
        break;
    }
    if (m != MOD_STRIP_META && m != MOD_FILL_SIZE && m != MOD_ICON_EXPORT) {
        add_ctl(CTL_CHECK, L"清除元数据（EXIF/定位）", {}, 10);
    }
    g_param_scroll = 0;   // 换模块回到顶部
    layout_param_scroll();
}

static void collect_params(Params& p) {
    p.module_id = g_module;
    static const int kTargetKb[] = {100, 200, 500, 1000};
    static const int kRotate[] = {0, 90, 180, 270};
    static const int kAngle[] = {0, 15, 30, 45, 90};
    for (auto& c : g_ctls) {
        int base = c.module;
        if (c.type == CTL_COMBO) {
            int sel = (int)SendMessageW(c.hwnd, CB_GETCURSEL, 0, 0);
            if (sel < 0) sel = 0;
            switch (base) {
            case 1: p.id_photo = sel; break;
            case 2: // 目标体积：前 4 项为预设；"自定义"（第 5 项）由下方输入框（module 16）提供
                if (sel <= 3) p.target_kb = kTargetKb[std::min(sel, 3)];
                break;
            case 6: p.crop_ratio_idx = sel + 1; break;
            case 7: { // 输出格式：UI 序（JPG/PNG/BMP/TIFF/GIF/WebP/PDF）→ 算子格式码
                static const int kOutFmt[] = {0, 1, 2, 4, 5, 6, 3};
                p.out_fmt = kOutFmt[std::min(sel, 6)];
                break;
            }
            case 8: p.rotate_deg = kRotate[std::min(sel, 3)]; break;
            case 9: p.flip = sel + 1; break;
            case 24: p.wm_alpha = (sel + 1) * 10; break;
            case 38: p.wm_img_alpha = (sel + 1) * 10; break;
            case 25: p.wm_angle = kAngle[std::min(sel, 4)]; break;
            case 26: p.wm_layout = sel; break;
            case 36: { // 图片水印布局：UI 序 → 算子布局码
                static const int kImgLayout[] = {0, 1, 3, 4, 5, 2};
                p.wm_img_layout = kImgLayout[std::min(sel, 5)];
                break;
            }
            case 39: { static const int kImgAngle[] = {0, 15, 30, 45, 60, 90};
                       p.wm_img_angle = kImgAngle[std::min(sel, 5)]; break; }
            case 30: p.stitch_dir = sel; break;
            case 31: p.stitch_fmt = sel; break;
            }
        } else if (c.type == CTL_CHECK) {
            bool checked = SendMessageW(c.hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (base == 3) p.fill_if_smaller = checked;
            if (base == 10) p.strip_meta = checked;
            if (base == 15) p.pdf_merge = checked;
        } else if (c.type == CTL_EDIT || c.type == CTL_EDITTEXT) {
            wchar_t buf[64] = {};
            GetWindowTextW(c.hwnd, buf, 64);
            int v = _wtoi(buf);
            if (base == 4) p.fill_target_kb = v > 0 ? v : 2000;
            if (base == 5) p.dpi = v > 0 ? v : 300;
            if (base == 11 && v > 0) p.id_custom_w = v;
            if (base == 12 && v > 0) p.id_custom_h = v;
            if (base == 13 && v > 0) p.crop_w = v;
            if (base == 14 && v > 0) p.crop_h = v;
            if (base == 16) { // 自定义目标体积：仅下拉选"自定义"时生效；<1KB 自动退回 1KB
                bool custom = false;
                for (auto& c2 : g_ctls)
                    if (c2.type == CTL_COMBO && c2.module == 2)
                        custom = (int)SendMessageW(c2.hwnd, CB_GETCURSEL, 0, 0) == 4;
                if (custom) p.target_kb = v > 0 ? v : 1;
            }
            if (base == 21) p.wm_text = buf; // 文字水印内容
            if (base == 22) p.wm_font_size = std::min(500, std::max(6, v)); // 越界钳到边界，不回退默认
            if (base == 28) p.wm_spacing = (v >= 10 && v <= 2000) ? v : 80;
            if (base == 37) p.wm_img_spacing = (v >= 10 && v <= 2000) ? v : 80;
            if (base == 34) p.wm_img_scale = (v >= 1 && v <= 100) ? v : 25;
        } else if (c.type == CTL_SLIDER) {
            if (base == 20) p.live_q = (int)SendMessageW(c.hwnd, TBM_GETPOS, 0, 0);
        }
    }
    p.wm_color = g_wm_color_sel; // 水印颜色：色块选择（全局）
    p.wm_image_path = g_wm_img_path; // 图片水印路径（对话框选择，存全局）
}

// ---------- 预计输出体积（实时估算） ----------
// 按当前模块+参数估算单张图预计输出体积，返回 KB；-1 表示无法估算
static long long est_kb_for(const FileItem& f, const Params& p) {
    auto cur_kb = [&]() { return (f.bytes + 1023) / 1024; };
    switch (g_module) {
    case MOD_COMPRESS_TARGET:
        return p.target_kb;
    case MOD_FILL_SIZE:
        // 增体积语义：不足目标补到目标，已达/超过目标保持原样（与 ops 算子一致）
        return std::max(cur_kb(), (long long)p.fill_target_kb);
    case MOD_SET_DPI: case MOD_STRIP_META: case MOD_FLIP: case MOD_ROTATE:
        return cur_kb(); // 字节级/镜像/旋转操作，体积基本不变
    case MOD_ID_PHOTO: {
        static const int kPreset[5][2] = {{295,413},{260,378},{413,579},{413,531},{390,567}};
        int tw, th;
        if (p.id_photo <= 4) { tw = kPreset[p.id_photo][0]; th = kPreset[p.id_photo][1]; }
        else { tw = p.id_custom_w; th = p.id_custom_h; }
        if (tw <= 0 || th <= 0 || f.w <= 0 || f.h <= 0) return -1;
        double ratio = (double)tw * th / ((double)f.w * f.h);
        return std::max<long long>(1, (long long)(cur_kb() * ratio));
    }
    case MOD_CENTER_CROP: {
        if (f.w <= 0 || f.h <= 0) return -1;
        if (p.crop_ratio_idx >= 1 && p.crop_ratio_idx <= 7) {
            static const int ratios[8][2] = {{0,0},{1,1},{4,3},{3,4},{16,9},{9,16},{3,2},{2,3}};
            int a = ratios[p.crop_ratio_idx][0], b = ratios[p.crop_ratio_idx][1];
            int tw = std::min(f.w, f.h * a / b), th = std::min(tw * b / a, f.h);
            return std::max<long long>(1, (long long)(cur_kb() * ((double)tw * th / (f.w * f.h))));
        }
        // 自定义：固定输出尺寸（允许放大/缩小），输出恰好 crop_w × crop_h
        if (p.crop_w <= 0 || p.crop_h <= 0) return -1;
        return std::max<long long>(1, (long long)(cur_kb() * ((double)p.crop_w * p.crop_h / (f.w * f.h))));
    }
    case MOD_FORMAT_CONVERT: {
        if (f.w <= 0 || f.h <= 0) return -1;
        size_t dot = f.path.find_last_of(L'.');
        std::wstring e = dot == std::wstring::npos ? L"" : f.path.substr(dot + 1);
        for (auto& c : e) c = (wchar_t)towlower(c);
        bool src_jpg = (e == L"jpg" || e == L"jpeg");
        // 同格式互转视为体积不变；跨格式按每像素字节经验值估算（JPG 0.45 / PNG 1.1 / BMP·TIFF 3.0 / PDF 0.5 / GIF 0.35 / WebP 0.8）
        int same = (p.out_fmt == 0 && src_jpg) || (p.out_fmt == 1 && e == L"png")
                || (p.out_fmt == 2 && e == L"bmp") || (p.out_fmt == 4 && (e == L"tif" || e == L"tiff"))
                || (p.out_fmt == 5 && e == L"gif") || (p.out_fmt == 6 && e == L"webp");
        if (same)
            return cur_kb();
        static const double bps[7] = {0.45, 1.1, 3.0, 0.5, 3.0, 0.35, 0.8}; // 索引=out_fmt 格式码
        if (p.out_fmt >= 0 && p.out_fmt <= 6)
            return std::max<long long>(1, (long long)((double)f.w * f.h * bps[p.out_fmt] / 1024.0));
        return cur_kb();
    }
    case MOD_COMPRESS_LIVE: {
        // 逐文件实测结果优先（每文件独立，互不影响）；未测完时回退用选中图外推
        auto it = g_est_cache.find(f.path);
        if (it != g_est_cache.end() && it->second.first == p.live_q && it->second.second > 0)
            return it->second.second;
        if (f.w > 0 && f.h > 0 && g_live_bpp > 0)
            return std::max<long long>(1, (long long)((double)f.w * f.h * g_live_bpp / 1024.0));
        return cur_kb();
    }
    case MOD_WATERMARK: case MOD_WATERMARK_IMG:
        return cur_kb(); // 水印体积近似不变
    case MOD_ICON_EXPORT:
        return 60; // 多尺寸 ico 经验中值
    default:
        return -1;
    }
}

static void update_est_column() {
    if (g_files.empty()) return;
    Params p;
    collect_params(p);
    wchar_t buf[40];
    for (size_t i = 0; i < g_files.size(); ++i) {
        long long kb = est_kb_for(g_files[i], p);
        if (kb < 0) wcscpy_s(buf, L"—");
        else fmt_est_kb(kb, buf, 40);
        ListView_SetItemText(g_list, (int)i, 3, buf);
    }
    start_live_est_all(); // 智能压缩：补测尚无当前质量实测结果的文件（后台，不阻塞）
}

// ---------- 文件列表 ----------
static void add_file_path(const std::wstring& path) {
    if (!is_image_file(path)) { dbg_log(L"add_file skip ext"); return; }
    for (auto& f : g_files) if (f.path == path) return;
    FileItem item;
    item.path = path;
    probe_image(path, item.w, item.h);
    std::error_code ec;
    item.bytes = (long long)std::filesystem::file_size(std::filesystem::path(path), ec);
    g_files.push_back(std::move(item));

    int idx = (int)g_files.size() - 1;
    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = idx;
    std::filesystem::path fp(path);
    std::wstring fname = fp.filename().wstring();
    lvi.pszText = (LPWSTR)fname.c_str();
    int ins = ListView_InsertItem(g_list, &lvi);
    dbg_log((L"add_file inserted idx=" + std::to_wstring(ins) + L" files=" + std::to_wstring(g_files.size())).c_str());

    wchar_t buf[64];
    swprintf(buf, 64, L"%d×%d", item.w, item.h);
    ListView_SetItemText(g_list, idx, 1, buf);
    fmt_size(item.bytes, buf, 64);
    ListView_SetItemText(g_list, idx, 2, buf);
    ListView_SetItemText(g_list, idx, 4, (LPWSTR)L"等待中");
    ListView_SetItemText(g_list, idx, 5, (LPWSTR)L"-");
    if (g_view_mode != VM_LIST && g_imglist) {
        add_thumb_entry(idx); // 缩略图模式：追加缩略图（索引=行号，保持对齐）
        LVITEMW it = {};
        it.mask = LVIF_IMAGE;
        it.iItem = idx;
        it.iImage = idx;
        ListView_SetItem(g_list, &it);
    }
    update_est_column();                    // 预计输出列按当前模块实时估算
    InvalidateRect(g_hwnd, nullptr, FALSE); // 刷新统计卡
}

static void add_directory_recursive(const std::wstring& dir) {
    std::filesystem::path root(dir);
    std::error_code ec;
    for (auto& e : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (e.is_regular_file() && is_image_file(e.path().filename().wstring()))
            add_file_path(e.path().wstring());
    }
}

static void clear_files() {
    g_files.clear();
    g_est_cache.clear();
    ListView_DeleteAllItems(g_list);
    refresh_preview();
    rebuild_thumbs(); // 缩略图模式：清空 ImageList
    dbg_log(L"clear_files done");
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------- 列表选中集合 / 移出 ----------
static std::vector<int> selected_indices() {
    std::vector<int> v;
    int i = -1;
    while ((i = (int)SendMessageW(g_list, LVM_GETNEXTITEM, (WPARAM)i, MAKELPARAM(LVNI_SELECTED, 0))) >= 0)
        v.push_back(i);
    return v;
}

// ---- 右键菜单项图标：把程序 ico（资源 ID 1）画成 32bpp 带 alpha 的 DIB ----
// 菜单原生支持 MIIM_BITMAP 项图标，无需完整 owner-draw。
static HBITMAP g_menu_icon_bmp = nullptr;

static HBITMAP make_menu_icon_bmp() {
    int size = GetSystemMetrics(SM_CXSMICON); // 系统小图标规格（一般 16px）
    if (size <= 0) size = 16;
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                   IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
    if (!icon) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp) {
        HDC mdc = CreateCompatibleDC(sdc);
        HGDIOBJ old = SelectObject(mdc, bmp);
        RECT rc = {0, 0, size, size};
        FillRect(mdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH)); // 清底，保证 alpha 为 0 起步
        DrawIconEx(mdc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL); // 32bpp ico 会保留 alpha 通道
        SelectObject(mdc, old);
        DeleteDC(mdc);
    }
    ReleaseDC(nullptr, sdc);
    DestroyIcon(icon);
    return bmp;
}

static void attach_menu_icon(HMENU menu, const UINT* ids, UINT n) {
    if (!g_menu_icon_bmp) g_menu_icon_bmp = make_menu_icon_bmp();
    if (!g_menu_icon_bmp) return;
    for (UINT k = 0; k < n; ++k) {
        MENUITEMINFO mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_BITMAP;
        mii.hbmpItem = g_menu_icon_bmp;
        SetMenuItemInfoW(menu, ids[k], FALSE, &mii);
    }
}

static void remove_selected_files() {
    if (g_pipe.running() || g_stitch_running) return; // 处理中禁止改动列表，避免索引错位
    bool removed = false;
    for (int i = (int)g_files.size() - 1; i >= 0; --i) {
        if (ListView_GetItemState(g_list, i, LVIS_SELECTED) & LVIS_SELECTED) {
            g_est_cache.erase(g_files[i].path);
            g_files.erase(g_files.begin() + i);
            ListView_DeleteItem(g_list, i);
            removed = true;
        }
    }
    if (removed) {
        update_est_column();
        refresh_preview(); // 索引位移/选中行被删后同步预览缓存
        rebuild_thumbs();  // 缩略图模式：删除后 ImageList 索引错位，整体重建
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

// ---------- 结果预览（实时预测：按当前模块+参数对选中图应用算子） ----------
struct PreviewCache {
    int item = -1;               // 预览对应的 g_files 索引
    std::wstring src_path;
    sp::Image src;               // 解码+限长边720，RGBA（算子输入）
    bool src_ok = false;
    sp::Image dst;               // 预测结果，BGRA（可直接显示）
    bool dst_ok = false;
    bool pixel_same = false;     // 该模块不改变像素（压缩/DPI/清元数据等）
};
static PreviewCache g_pv;

// 预览解码异步化：8MB+ 大图全分辨率解码需数百 ms~秒级，放后台线程，
// 选中切换即时响应；完成后 PostMessage 回 UI 线程填缓存。
// 另配 3 张 LRU 小图缓存（每张 ≤720px ≈ 2MB），最近看过的文件回切零等待。
struct PreviewDecodeResult { std::wstring path; sp::Image img; bool ok = false; };
struct PvCacheEntry { std::wstring path; sp::Image img; bool ok = false; };
static void shrink_image_max(sp::Image& im, int maxd); // 定义在下方（预览算子段）
static std::deque<PvCacheEntry> g_pv_cache;          // 末端=最新
static std::set<std::wstring> g_pv_inflight;         // 后台解码中的路径（防重复派发）
static std::mutex g_pv_inflight_mu;
static bool g_pv_decoding = false;                   // UI 侧标记：预览卡显示"解码中…"

// ---------- 智能压缩：逐文件后台实测估算 ----------
// 每个文件各自解码 720px 预览 → 按当前质量实测 JPEG 字节 → 外推该文件全尺寸估算。
// 结果按 path→(质量,KB) 缓存：同一质量不重测；滑块拖动仅重测质量不同的文件。
struct LiveEstReq    { std::wstring path; int q; };
struct LiveEstResult { std::wstring path; int q; long long bytes720 = 0; int w720 = 0, h720 = 0; bool ok = false; };
static std::deque<LiveEstReq> g_est_queue;            // 待实测队列（质量变化时清空重排）
static std::mutex g_est_mu;
static std::condition_variable g_est_cv;
static std::set<std::wstring> g_est_inflight;         // 已排队/解码中的路径（防重复派发）
static int g_est_q_current = -1;                      // 队列当前承载的质量

static void live_est_worker() {
    for (;;) {
        LiveEstReq req;
        {
            std::unique_lock<std::mutex> lk(g_est_mu);
            g_est_cv.wait(lk, [] { return !g_est_queue.empty(); });
            req = g_est_queue.front();
            g_est_queue.pop_front();
        }
        auto* r = new LiveEstResult();
        r->path = req.path; r->q = req.q;
        sp::Image im;
        if (sp::load_image(req.path, im)) {
            shrink_image_max(im, 720);
            int w720 = im.w, h720 = im.h; // 缩放前尺寸（外推按原图像素密度，输出像素=原图×k²）
            // 与批量执行同一计划：q<40 时降分辨率再以 eff_q 编码（保证预估=实际行为）
            double k; int eq;
            sp::smart_compress_plan(req.q, k, eq);
            if (k < 1.0) {
                sp::Image scaled;
                scaled.w = std::max(1, (int)llround(im.w * k));
                scaled.h = std::max(1, (int)llround(im.h * k));
                scaled.px.assign((size_t)scaled.w * scaled.h * 4, 255);
                sp::resize_image(im, scaled);
                im = std::move(scaled);
            }
            auto data = encode_jpg(im, eq);
            r->ok = true;
            r->bytes720 = (long long)data.size();
            r->w720 = w720; r->h720 = h720;
        }
        PostMessageW(g_hwnd, WM_SP_LIVE_EST, 0, (LPARAM)r);
    }
}

static void ensure_est_worker() {
    static bool started = false;
    if (!started) { started = true; std::thread(live_est_worker).detach(); }
}

// 把尚未有"当前质量实测结果"的文件排入队列（在 update_est_column 末尾触发）
static void start_live_est_all() {
    if (g_module != MOD_COMPRESS_LIVE) return;
    Params p;
    collect_params(p);
    int q = std::max(1, std::min(95, p.live_q));
    ensure_est_worker();
    std::vector<LiveEstReq> add;
    {
        std::lock_guard<std::mutex> lk(g_est_mu);
        if (q != g_est_q_current) { // 质量变化：作废排队中的旧质量请求（解码中的旧结果按质量键自然淘汰）
            g_est_queue.clear();
            g_est_q_current = q;
        }
        for (auto& f : g_files) {
            auto it = g_est_cache.find(f.path);
            if (it != g_est_cache.end() && it->second.first == q) continue; // 已有当前质量结果
            if (!g_est_inflight.insert(f.path).second) continue;            // 已排队/解码中
            add.push_back({f.path, q});
        }
        for (auto& r : add) g_est_queue.push_back(r);
    }
    if (!add.empty()) g_est_cv.notify_one();
}

static bool preview_cache_get(const std::wstring& path, sp::Image& out, bool& ok) {
    for (auto it = g_pv_cache.begin(); it != g_pv_cache.end(); ++it) {
        if (it->path == path) {
            PvCacheEntry e = std::move(*it);
            g_pv_cache.erase(it);                    // 命中即提升到末端（LRU）
            g_pv_cache.push_back(std::move(e));
            out = g_pv_cache.back().img;             // 拷贝一份给调用方
            ok = g_pv_cache.back().ok;
            return true;
        }
    }
    return false;
}
static void preview_cache_put(std::wstring path, sp::Image img, bool ok) {
    for (auto it = g_pv_cache.begin(); it != g_pv_cache.end(); ++it)
        if (it->path == path) { g_pv_cache.erase(it); break; }
    g_pv_cache.push_back({std::move(path), std::move(img), ok});
    while (g_pv_cache.size() > 3) g_pv_cache.pop_front();
}
static void preview_decode_async(std::wstring path) {
    g_pv_decoding = true; // 无论是否重复派发，UI 都应显示"解码中…"
    {
        std::lock_guard<std::mutex> lk(g_pv_inflight_mu);
        if (!g_pv_inflight.insert(path).second) return; // 同一文件已在解码，不重复派发
    }
    std::thread([path = std::move(path)]() {
        auto* r = new PreviewDecodeResult();
        r->path = path;
        r->ok = sp::load_image(path, r->img);
        if (r->ok) shrink_image_max(r->img, 1024); // 720→1024：预览卡高DPI下不再放大发糊
        PostMessageW(g_hwnd, WM_SP_PREVIEW_DECODED, 0, (LPARAM)r);
    }).detach();
}

// ---------- 大图预览弹窗 ----------
static HWND g_pv_pop = nullptr;   // 弹窗句柄（null=未打开）
static sp::Image g_pv_pop_img;    // 弹窗缓存的大图（BGRA，处理完成后）
static bool g_pv_pop_ok = false;
static sp::Image g_pv_pop_src_img;    // 弹窗缓存的原图（BGRA）
static bool g_pv_pop_src_ok = false;
static bool g_pv_pop_compare = false; // true=对比模式（左原图/右处理后）；false=单图预览（原图）
static double g_pv_zoom = 1.0;             // 弹窗缩放倍数（1=适配窗口，滚轮 ±25%）
static int g_pv_pan_x = 0, g_pv_pan_y = 0; // 缩放后平移偏移（弹窗客户区 px，左键拖拽）
static std::wstring g_pv_pop_src_label, g_pv_pop_dst_label; // 对比模式顶部标签（体积+分辨率）
static void load_preview_popup_image(bool orig_only);
static void open_preview_popup(bool compare);

// stb 解码是 RGBA，GDI 32bpp 按 BGRA 解释，交换 R/B
static void preview_bgra(sp::Image& im) {
    if (!im.ok()) return;
    for (auto* p = im.px.data(); p + 3 < im.px.data() + im.px.size(); p += 4)
        std::swap(p[0], p[2]);
}

// 预览图限长边 720px，降低驻留内存与 StretchDIBits 开销
static void shrink_image_max(sp::Image& im, int maxd) {
    if (!im.ok() || (im.w <= maxd && im.h <= maxd)) return;
    // 等比缩放：长边贴 maxd（此前误用 im.w>=maxd 判断长短边，导致宽图/竖图被压成正方形失真）
    int tw, th;
    if (im.w >= im.h) { tw = maxd; th = std::max(1, im.h * maxd / im.w); }
    else              { th = maxd; tw = std::max(1, im.w * maxd / im.h); }
    sp::Image out(tw, th);
    sp::resize_image(im, out);
    im = std::move(out);
}

// 预览算子：按当前模块+参数对给定源图做几何预测。
// src 可以是 720px 预览缓存（卡片小图）或全分辨率原图（弹窗大图），
// file_w 传"当前文件的真实宽度"用于自定义裁剪的比例折算。
static sp::Image preview_apply_ops(const sp::Image& src, const Params& p, int file_w, bool& pixel_same) {
    sp::Image out;
    pixel_same = false;
    switch (g_module) {
    case MOD_ID_PHOTO: {
        static const int kPreset[5][2] = {{295,413},{260,378},{413,579},{413,531},{390,567}};
        int tw, th;
        if (p.id_photo <= 4) { tw = kPreset[p.id_photo][0]; th = kPreset[p.id_photo][1]; }
        else { tw = p.id_custom_w; th = p.id_custom_h; }
        if (tw > 0 && th > 0) out = fit_center_crop(src, tw, th, VAnchor::Split25);
        break;
    }
    case MOD_CENTER_CROP: {
        static const int ratios[8][2] = {{0,0},{1,1},{4,3},{3,4},{16,9},{9,16},{3,2},{2,3}};
        if (p.crop_ratio_idx >= 1 && p.crop_ratio_idx <= 7) {
            int a = ratios[p.crop_ratio_idx][0], b = ratios[p.crop_ratio_idx][1];
            int tw = std::min(src.w, src.h * a / b), th = tw * b / a;
            out = sp::Image(tw, th);
            int cx = (src.w - tw) / 2, cy = (src.h - th) / 2;
            for (int y = 0; y < th; ++y)
                memcpy(out.row(y), src.row(cy + y) + cx * 4, (size_t)tw * 4);
        } else {
            // 自定义：固定输出尺寸 —— 等比缩放铺满 + 居中裁剪（与算子一致；
            // k=原图→当前图折算，预览缓存/全分辨率下结果几何一致）
            double k = (file_w > 0) ? (double)src.w / file_w : 1.0;
            int tw = std::max(1, (int)llround(p.crop_w * k));
            int th = std::max(1, (int)llround(p.crop_h * k));
            out = sp::fit_center_crop(src, tw, th, sp::VAnchor::Center);
        }
        break;
    }
    case MOD_FORMAT_CONVERT:
        if (p.rotate_deg) { sp::Image r; sp::rotate_image(src, r, p.rotate_deg); out = std::move(r); }
        else out = src;
        break;
    case MOD_ROTATE:
        if (p.rotate_deg) { sp::Image r; sp::rotate_image(src, r, p.rotate_deg); out = std::move(r); }
        else out = src;
        break;
    case MOD_FLIP:
        sp::flip_image(src, out, p.flip == 1);
        if (p.rotate_deg) { sp::Image r; sp::rotate_image(out, r, p.rotate_deg); out = std::move(r); }
        break;
    case MOD_WATERMARK: {
        // 预览图上按比例渲染水印（字号/间距按 原图→预览 缩放，构图与全尺寸输出一致）
        double k = (file_w > 0) ? (double)src.w / file_w : 1.0;
        int mw = 0, mh = 0;
        std::vector<uint8_t> mask;
        if (render_text_mask(p.wm_text, std::max(6, (int)llround(p.wm_font_size * k)),
                             p.wm_angle, mw, mh, mask)) {
            WmColor wc = wm_color_by_index(p.wm_color);
            out = src;
            blend_watermark(out, mask, mw, mh, wc.r, wc.g, wc.b,
                            p.wm_alpha, p.wm_layout, std::max(10, (int)llround(p.wm_spacing * k)));
        } else out = src;
        break;
    }
    case MOD_WATERMARK_IMG: {
        // 图片水印独立模块：缓存解码结果，路径不变不重复解码
        if (p.wm_image_path.empty()) { out = src; pixel_same = true; break; }
        static Image wm_cache;
        static std::wstring wm_cache_path;
        if (wm_cache_path != p.wm_image_path) {
            load_image(p.wm_image_path, wm_cache);
            wm_cache_path = p.wm_image_path;
        }
        if (wm_cache.ok()) {
            out = src;
            blend_image_watermark(out, wm_cache, p.wm_img_alpha, p.wm_img_layout,
                                  p.wm_img_spacing, p.wm_img_scale, p.wm_img_angle);
        } else { out = src; pixel_same = true; }
        break;
    }
    case MOD_COMPRESS_LIVE: {
        // 智能压缩：q<40 时批量执行会降分辨率——预览同步缩放反映真实观感
        double k; int eq;
        smart_compress_plan(std::max(1, std::min(95, p.live_q)), k, eq);
        if (k < 1.0) {
            sp::Image scaled;
            scaled.w = std::max(1, (int)llround(src.w * k));
            scaled.h = std::max(1, (int)llround(src.h * k));
            scaled.px.assign((size_t)scaled.w * scaled.h * 4, 255);
            resize_image(src, scaled);
            out = std::move(scaled);
        } else { out = src; pixel_same = true; }
        break;
    }
    default:
        out = src;   // 压缩/增体积/DPI/清元数据/ico/阶段2模块：像素不变
        pixel_same = true;
        break;
    }
    return out;
}

static void refresh_preview() {
    int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    const FileItem* f = (sel >= 0 && sel < (int)g_files.size()) ? &g_files[sel] : nullptr;
    // 解码缓存：文件未变则不重新解码（解码是预览链路的最大开销）。
    // 大图走后台线程解码，UI 立即返回（预览卡先显示"解码中…"）
    if (sel != g_pv.item || !f || g_pv.src_path != f->path) {
        g_pv = PreviewCache{};
        g_pv.item = sel;
        g_pv_decoding = false;
        if (f) {
            g_pv.src_path = f->path;
            bool cached_ok = false;
            if (preview_cache_get(f->path, g_pv.src, cached_ok)) {
                g_pv.src_ok = cached_ok; // LRU 命中：零等待
            } else {
                preview_decode_async(f->path); // 缓存未命中：后台解码
            }
        }
    }
    // 算子预测：在 ≤720px 小图上应用几何变换（代价极低），参数变动即时刷新
    g_pv.dst_ok = false;
    g_pv.pixel_same = false;
    if (g_pv.src_ok) {
        Params p;
        collect_params(p);
        sp::Image out = preview_apply_ops(g_pv.src, p, g_files[g_pv.item].w, g_pv.pixel_same);
        if (out.ok()) {
            g_pv.dst = std::move(out);
            preview_bgra(g_pv.dst);
            g_pv.dst_ok = true;
        }
    }
    // 智能压缩：实测当前质量下预览图的每像素字节（文件/滑块变化时重算）
    if (g_module == MOD_COMPRESS_LIVE && g_pv.src_ok) {
        Params p;
        collect_params(p);
        if (g_live_bpp_q != p.live_q || g_live_bpp_path != g_pv.src_path) {
            double k; int eq;
            sp::smart_compress_plan(std::max(1, std::min(95, p.live_q)), k, eq);
            const sp::Image* work = &g_pv.src;
            sp::Image scaled;
            if (k < 1.0) {
                scaled.w = std::max(1, (int)llround(g_pv.src.w * k));
                scaled.h = std::max(1, (int)llround(g_pv.src.h * k));
                scaled.px.assign((size_t)scaled.w * scaled.h * 4, 255);
                sp::resize_image(g_pv.src, scaled);
                work = &scaled;
            }
            auto data = encode_jpg(*work, eq);
            if (g_pv.src.w > 0)
                g_live_bpp = (double)data.size() / ((double)g_pv.src.w * g_pv.src.h);
            g_live_bpp_q = p.live_q;
            g_live_bpp_path = g_pv.src_path;
            update_est_column(); // 用实测值刷新估算列
        }
    }
    // 大图预览弹窗打开时，同步重载内容（参数/文件已变）
    if (g_pv_pop) { load_preview_popup_image(!g_pv_pop_compare); InvalidateRect(g_pv_pop, nullptr, TRUE); }
    InvalidateRect(g_param_panel, nullptr, FALSE);
}

// ---------- 大图预览弹窗 ----------
// 在指定矩形内等比居中绘制 BGRA 图像（预览卡/弹窗共用）
static void draw_image_fit(HDC dc, RECT ir, const sp::Image& im) {
    int aw = ir.right - ir.left, ah = ir.bottom - ir.top;
    int dw = aw, dh = ah;
    if (im.w * ah > im.h * aw) dh = std::max(1, im.h * aw / im.w);
    else dw = std::max(1, im.w * ah / im.h);
    int dx = ir.left + (aw - dw) / 2, dy = ir.top + (ah - dh) / 2;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = im.w;
    bmi.bmiHeader.biHeight = -im.h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    // HALFTONE 高质量拉伸（默认 COLORONCOLOR 点采样，缩小时丢行丢列、锯齿明显）
    int prev_mode = SetStretchBltMode(dc, HALFTONE);
    if (prev_mode) { POINT po = {0, 0}; SetBrushOrgEx(dc, po.x, po.y, &po); }
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, im.w, im.h,
                  im.px.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    if (prev_mode) SetStretchBltMode(dc, prev_mode);
}

// 带缩放/平移的绘制（弹窗滚轮缩放）：z=1 时等价 draw_image_fit（fit 居中）；
// z>1 以 fit 尺寸为基准放大，图像中心 = 矩形中心 + (panx,pany)，超界部分按矩形裁剪
// （对比模式左右两半各自裁剪，放大后互不越界）
static void draw_image_zoom(HDC dc, RECT ir, const sp::Image& im, double z, int panx, int pany) {
    int aw = ir.right - ir.left, ah = ir.bottom - ir.top;
    double s0 = std::min((double)aw / im.w, (double)ah / im.h);
    int dw = std::max(1, (int)(im.w * s0 * z));
    int dh = std::max(1, (int)(im.h * s0 * z));
    int dx = (ir.left + ir.right) / 2 + panx - dw / 2;
    int dy = (ir.top + ir.bottom) / 2 + pany - dh / 2;
    SaveDC(dc);
    IntersectClipRect(dc, ir.left, ir.top, ir.right, ir.bottom);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = im.w;
    bmi.bmiHeader.biHeight = -im.h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    int prev_mode = SetStretchBltMode(dc, HALFTONE);
    if (prev_mode) { POINT po = {0, 0}; SetBrushOrgEx(dc, po.x, po.y, &po); }
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, im.w, im.h,
                  im.px.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    if (prev_mode) SetStretchBltMode(dc, prev_mode);
    RestoreDC(dc, -1);
}

// 预览卡矩形（draw_panel 与点击命中共用同一几何，避免两处漂移）。
// 固定锚定面板底部：参数再多也不推移/遮挡（参数区改为滚动）
static RECT preview_card_rect(HWND panel) {
    RECT rc;
    GetClientRect(panel, &rc);
    int pv_top = std::max(S(56), (int)rc.bottom - S(340));
    return RECT{S(10), pv_top, rc.right - S(10), rc.bottom - S(116)}; // 底部让位给处理按钮卡（间隙 S(10)）；左右内边距 S(10)：与中间区的 S(20) 间隙各分一半，分隔线居中
}

// 处理按钮卡矩形：包裹"仅处理选中/批量处理全部"两枚紫色按钮，
// 底边与面板底（红线 H-bottom_h）平齐 → 与侧栏末组卡、文件列表卡三者底边水平对齐
static RECT run_card_rect(HWND panel) {
    RECT rc;
    GetClientRect(panel, &rc);
    return RECT{S(10), rc.bottom - S(106), rc.right - S(10), rc.bottom}; // 高 S(106)：内边距 S(11)+按钮S(38)+间隙S(8)+按钮S(38)+S(11)
}

// 参数卡片矩形：与预览卡同宽（左右 S(14) 内边距），包裹全部参数控件（标题下方 ↔ 预览卡上方）
static RECT param_card_rect(HWND panel) {
    RECT pv = preview_card_rect(panel);
    return RECT{S(10), S(50), pv.right, std::max((int)pv.top - S(6), S(52))};
}

// ---------- 参数区滚动：参数过多时在"标题↔预览卡"之间滚动 ----------
static int param_area_top() { return S(56); } // 模块标题下方
static int param_area_bottom(HWND panel) { return preview_card_rect(panel).top - S(8); }

// 按当前偏移重排参数控件：y = y0 - scroll，落在区域外者隐藏（防止压住标题/预览卡）
static void param_apply_scroll() {
    int top = param_area_top();
    int bottom = param_area_bottom(g_param_panel);
    RECT prc;
    GetClientRect(g_param_panel, &prc);
    for (auto& c : g_ctls) {
        if (!c.hwnd) continue;
        RECT cr;
        GetWindowRect(c.hwnd, &cr);
        MapWindowPoints(nullptr, g_param_panel, (POINT*)&cr, 2);
        int h = cr.bottom - cr.top;
        int vh = (c.type == CTL_COMBO) ? S(26) : h; // COMBOBOX 窗口高含下拉区，可见行只有一行
        int y = c.y0 - g_param_scroll;
        if (y + vh <= top || y >= bottom) {
            if (IsWindowVisible(c.hwnd)) ShowWindow(c.hwnd, SW_HIDE); // 已隐藏不重复调用
        } else {
            bool was_visible = IsWindowVisible(c.hwnd) != 0;
            bool moved = (cr.top != y);
            ShowWindow(c.hwnd, SW_SHOW);
            if (moved || !was_visible)
                MoveWindow(c.hwnd, cr.left, y, cr.right - cr.left, h, FALSE);
            // 裁剪越界部分：控件跨参数区上下边界时（如滚动到一半的行），
            // 用窗口区域只显示落在 [top,bottom) 内的部分，避免压到模块标题/预览卡上
            int ct = std::max(0, top - y);
            int cb = std::min(h, bottom - y);
            if (ct > 0 || cb < h)
                SetWindowRgn(c.hwnd, CreateRectRgn(0, ct, cr.right - cr.left, std::max(cb, ct)), TRUE);
            else
                SetWindowRgn(c.hwnd, nullptr, TRUE); // 完全在区内：清除裁剪
        }
    }
    // 关键：滚动后强制整块参数区擦除重绘。面板带 WS_CLIPCHILDREN，子控件移走/隐藏后
    // 腾出的区域不会自动失效——STATIC 透明底文字在旧位置留下重影，逐次滚动累积错乱。
    RECT area = {0, top, prc.right, bottom};
    InvalidateRect(g_param_panel, &area, TRUE);
    UpdateWindow(g_param_panel);
    for (auto& c : g_ctls)
        if (c.hwnd && IsWindowVisible(c.hwnd)) InvalidateRect(c.hwnd, nullptr, TRUE);
}

static void layout_param_scroll() {
    RECT rc;
    GetClientRect(g_param_panel, &rc);
    int avail = std::max(S(60), param_area_bottom(g_param_panel) - param_area_top());
    int content = g_panel_y + S(6) - param_area_top();
    g_param_scroll_max = std::max(0, content - avail);
    if (g_param_scroll > g_param_scroll_max) g_param_scroll = g_param_scroll_max;
    if (g_param_scroll < 0) g_param_scroll = 0;
    if (g_param_scrollbar) {
        if (g_param_scroll_max > 0) {
            MoveWindow(g_param_scrollbar, rc.right - S(16), param_area_top(),
                       S(16), avail, TRUE);
            SCROLLINFO si = { sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE };
            si.nMin = 0;
            // nMax 必须是总内容高度（余量+可见），滑块比例 = 可见/内容；
            // 若填滚动余量，可见高度 ≥ 余量时控件判"无内容可滚"，内部位置钳回 0 /
            // 滑块满格，反复滚轮 SetScrollInfo 后滑块与内容脱节（滚动多次出 bug 的根因）
            si.nMax = g_param_scroll_max + avail - 1;
            si.nPage = (UINT)avail;
            SetScrollInfo(g_param_scrollbar, SB_CTL, &si, TRUE);
            ShowWindow(g_param_scrollbar, SW_SHOW);
        } else {
            ShowWindow(g_param_scrollbar, SW_HIDE);
        }
    }
    param_apply_scroll();
}

// 滚轮驱动参数区滚动；返回是否消费
static bool param_wheel(int delta) {
    if (g_param_scroll_max <= 0) return false;
    int old = g_param_scroll;
    g_param_scroll -= (delta / WHEEL_DELTA) * S(36);
    g_param_scroll = std::max(0, std::min(g_param_scroll, g_param_scroll_max));
    if (g_param_scroll == old) return g_param_scroll_max > 0;
    SCROLLINFO si = { sizeof(SCROLLINFO), SIF_POS };
    si.nPos = g_param_scroll;
    SetScrollInfo(g_param_scrollbar, SB_CTL, &si, TRUE);
    param_apply_scroll();
    return true;
}

static void param_scroll_by(int code, int pos) {
    int old = g_param_scroll;
    int line = S(32);
    RECT rc; GetClientRect(g_param_panel, &rc);
    int page = std::max(line, param_area_bottom(g_param_panel) - param_area_top());
    switch (code) {
    case SB_LINEUP:        g_param_scroll -= line; break;
    case SB_LINEDOWN:      g_param_scroll += line; break;
    case SB_PAGEUP:        g_param_scroll -= page; break;
    case SB_PAGEDOWN:      g_param_scroll += page; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK:    g_param_scroll = pos; break;
    default: return; // SB_ENDSCROLL 等
    }
    g_param_scroll = std::max(0, std::min(g_param_scroll, g_param_scroll_max));
    if (g_param_scroll != old) {
        SCROLLINFO si = { sizeof(SCROLLINFO), SIF_POS };
        si.nPos = g_param_scroll;
        SetScrollInfo(g_param_scrollbar, SB_CTL, &si, TRUE);
        param_apply_scroll();
    }
}

// 弹窗大图：全分辨率解码原图 → （对比模式）同一套算子预测 → 限长边 S(1600) → BGRA
// orig_only=true 仅加载原图（双击列表/缩略图的放大预览）；false 额外生成处理结果（对比右图）
static void load_preview_popup_image(bool orig_only) {
    g_pv_pop_ok = false;
    g_pv_pop_src_ok = false;
    g_pv_pop_img = sp::Image();
    g_pv_pop_src_img = sp::Image();
    g_pv_zoom = 1.0; g_pv_pan_x = g_pv_pan_y = 0; // 新内容：缩放复位
    g_pv_pop_src_label.clear();
    g_pv_pop_dst_label.clear();
    if (g_pv.item < 0 || g_pv.item >= (int)g_files.size()) return;
    sp::Image full;
    if (!sp::load_image(g_files[g_pv.item].path, full)) return;
    const FileItem& f = g_files[g_pv.item];
    {   // 原图标签：实际文件体积 + 原始分辨率
        wchar_t sz[40], dim[32], lab[96];
        fmt_size(f.bytes, sz, 40);
        swprintf(dim, 32, L"%d×%d", full.w, full.h);
        swprintf(lab, 96, L"原图（%s，%s）", sz, dim);
        g_pv_pop_src_label = lab;
    }
    // 原图：缩到长边 S(1600) 转 BGRA 缓存（单图/对比模式左图共用）
    g_pv_pop_src_img = full;
    shrink_image_max(g_pv_pop_src_img, S(1600));
    preview_bgra(g_pv_pop_src_img);
    g_pv_pop_src_ok = true;
    if (orig_only) return;
    // 处理后：全分辨率跑同一套算子（与主窗预览一致），再缩放缓存
    Params p;
    collect_params(p);
    bool ps = false;
    sp::Image out = preview_apply_ops(full, p, f.w, ps);
    // 指定体积压缩：真实算子在质量二分到 40 仍超标时会迭代降分辨率（ops.cpp 同款循环），
    // 预览复刻同一预测（同一 compress_to_size + 同一降采样策略），使"处理后"的
    // 分辨率与右图观感都与实际输出一致（此前走像素不变分支，标签错显原图分辨率）
    if (g_module == MOD_COMPRESS_TARGET && p.target_kb > 0 && out.ok()) {
        long long target = (long long)p.target_kb * 1000; // 与算子一致：1000 进制
        bool src_jpeg = false;
        {   size_t d = f.path.find_last_of(L'.');
            std::wstring e = (d == std::wstring::npos) ? L"" : f.path.substr(d + 1);
            for (auto& c : e) c = (wchar_t)towlower(c);
            src_jpeg = (e == L"jpg" || e == L"jpeg");
        }
        // 达标 JPEG 直通/填充场景：算子直接复制原文件，分辨率不变，无需预测
        if (!(src_jpeg && (long long)f.bytes <= target)) {
            // 等价简化：compress_to_size 二分可达标时不降分辨率（标签尺寸不变），
            // 不可达标（q40 编码仍超标）时返回的正是 q40 编码（二分全失败 best=最小体积=q40）。
            // 故用一次 encode_jpg(·,40) 即可复现算子的降分辨率轨迹，
            // 避免二分的 ~7 轮全尺寸编码把 UI 卡住数秒
            auto data = encode_jpg(full, 40);
            if ((long long)data.size() > target) {
                double k = 1.0;
                for (int it = 0; it < 10 && (long long)data.size() > target; ++it) {
                    double ratio = std::sqrt((double)target / (double)data.size()) * 0.85;
                    k = std::max(0.05, k * ratio);
                    int sw = std::max(1, (int)llround(full.w * k));
                    int sh = std::max(1, (int)llround(full.h * k));
                    if (sw == full.w && sh == full.h) break; // 已到下限，与算子一致不再空转
                    sp::Image scaled(sw, sh);
                    resize_image(full, scaled);
                    data = encode_jpg(scaled, 40);
                    out = std::move(scaled); // 最后一轮缩放图 = 实际输出的分辨率与观感
                }
            }
        }
    }
    const sp::Image& base = out.ok() ? out : full;
    {   // 处理后标签：预计输出体积（与列表“预计输出”列同一估算口径）+ 处理后分辨率
        wchar_t sz[44], dim[32], lab[112];
        fmt_est_kb(est_kb_for(f, p), sz, 44);
        swprintf(dim, 32, L"%d×%d", base.w, base.h);
        swprintf(lab, 112, L"处理后（%s，%s）", sz, dim);
        g_pv_pop_dst_label = lab;
    }
    g_pv_pop_img = base;
    shrink_image_max(g_pv_pop_img, S(1600));
    preview_bgra(g_pv_pop_img);
    g_pv_pop_ok = true;
}

static LRESULT CALLBACK pop_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    static bool s_pan = false;        // 左键拖拽平移中（仅 zoom>1 时）
    static POINT s_pan_last = {};
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        fill_rect(dc, rc, RGB(0xF7, 0xF8, 0xFA));
        const double z = g_pv_zoom;
        const int px = g_pv_pan_x, py = g_pv_pan_y;
        if (g_pv_pop_compare) {
            // 对比模式：左半原图 / 右半处理后，各带顶部标签（黑字，体积+分辨率）
            if (g_pv_pop_src_ok) {
                int mid = (rc.left + rc.right) / 2;
                int top = rc.top + S(12);
                int img_top = top + S(26); // 标签带随字号（small+4px）加高
                int bottom = rc.bottom - S(16);
                RECT limg = {rc.left + S(16), img_top, mid - S(8), bottom};
                RECT rimg = {mid + S(8), img_top, rc.right - S(16), bottom};
                draw_image_zoom(dc, limg, g_pv_pop_src_img, z, px, py);
                if (g_pv_pop_ok) draw_image_zoom(dc, rimg, g_pv_pop_img, z, px, py);
                else draw_text(dc, L"无法加载处理结果", rimg, C_GRAY, g_font_small,
                               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                RECT llab = {limg.left, top, limg.right, img_top - S(2)};
                RECT rlab = {rimg.left, top, rimg.right, img_top - S(2)};
                draw_text(dc, g_pv_pop_src_label.c_str(), llab, RGB(0, 0, 0), g_font_pv,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                draw_text(dc, g_pv_pop_dst_label.c_str(), rlab, RGB(0, 0, 0), g_font_pv,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                draw_text(dc, L"无法加载预览", rc, C_GRAY, g_font_small,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        } else {
            // 单图模式：原图 fit 铺满
            if (g_pv_pop_src_ok) {
                RECT ir = {rc.left + S(16), rc.top + S(16), rc.right - S(16), rc.bottom - S(16)};
                draw_image_zoom(dc, ir, g_pv_pop_src_img, z, px, py);
            } else {
                draw_text(dc, L"无法加载预览", rc, C_GRAY, g_font_small,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        // 滚轮缩放：±25%，范围 1~8 倍；以光标为锚点（光标下的图像点缩放前后不动）。
        // 对比模式左右两半共用同一缩放与平移（同步对比）
        short delta = (short)HIWORD(wp);
        double nz = g_pv_zoom * (delta > 0 ? 1.25 : 0.8);
        nz = std::max(1.0, std::min(8.0, nz));
        if (nz == g_pv_zoom) return 0;
        RECT rc;
        GetClientRect(h, &rc);
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
        ScreenToClient(h, &pt);
        // 光标所在的图像矩形（对比模式按中线分左右；单图模式全窗）
        RECT ir;
        if (g_pv_pop_compare && g_pv_pop_src_ok) {
            int mid = (rc.left + rc.right) / 2;
            int img_top = rc.top + S(12) + S(24);
            ir = pt.x < mid ? RECT{rc.left + S(16), img_top, mid - S(8), rc.bottom - S(16)}
                            : RECT{mid + S(8), img_top, rc.right - S(16), rc.bottom - S(16)};
        } else {
            ir = {rc.left + S(16), rc.top + S(16), rc.right - S(16), rc.bottom - S(16)};
        }
        if (!PtInRect(&ir, pt)) return 0;
        double s0 = std::min((double)(ir.right - ir.left) / std::max(1, g_pv_pop_src_img.w),
                             (double)(ir.bottom - ir.top) / std::max(1, g_pv_pop_src_img.h));
        int cx = (ir.left + ir.right) / 2, cy = (ir.top + ir.bottom) / 2;
        // 图像点 =（光标 - 中心 - 平移）/ (s0*zoom)；放大后令该点仍在光标下
        double ix = (pt.x - cx - g_pv_pan_x) / (s0 * g_pv_zoom);
        double iy = (pt.y - cy - g_pv_pan_y) / (s0 * g_pv_zoom);
        g_pv_zoom = nz;
        g_pv_pan_x = (int)std::llround(pt.x - cx - ix * s0 * nz);
        g_pv_pan_y = (int)std::llround(pt.y - cy - iy * s0 * nz);
        if (g_pv_zoom <= 1.0) { g_pv_zoom = 1.0; g_pv_pan_x = g_pv_pan_y = 0; } // 回到 fit：平移复位
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (g_pv_zoom > 1.0) { // 缩放后左键拖拽平移
            s_pan = true;
            SetCapture(h);
            s_pan_last = {(short)LOWORD(lp), (short)HIWORD(lp)};
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        }
        return 0;
    case WM_MOUSEMOVE:
        if (s_pan) {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            g_pv_pan_x += pt.x - s_pan_last.x;
            g_pv_pan_y += pt.y - s_pan_last.y;
            s_pan_last = pt;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (s_pan) { s_pan = false; ReleaseCapture(); SetCursor(LoadCursorW(nullptr, IDC_ARROW)); }
        return 0;
    case WM_LBUTTONDBLCLK: // 双击复位缩放
        g_pv_zoom = 1.0;
        g_pv_pan_x = g_pv_pan_y = 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(h); return 0; }
        break;
    case WM_DESTROY:
        g_pv_pop = nullptr;
        g_pv_pop_ok = false;
        g_pv_pop_img = sp::Image();
        g_pv_pop_src_ok = false;
        g_pv_pop_src_img = sp::Image();
        g_pv_pop_compare = false;
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// 弹窗预览：compare=true → 左原图/右处理后的对比窗（点击右侧预览卡）；
// compare=false → 原图放大预览单图窗（双击列表/缩略图）
static void open_preview_popup(bool compare) {
    if (compare) {
        if (g_pv.item < 0 || g_pv.item >= (int)g_files.size() || !g_pv.dst_ok) {
            MessageBoxW(g_hwnd, L"请先在列表中选中图片", L"SimplePic", MB_OK | MB_ICONINFORMATION);
            return;
        }
    } else {
        if (g_pv.item < 0 || g_pv.item >= (int)g_files.size()) {
            MessageBoxW(g_hwnd, L"请先在列表中选中图片", L"SimplePic", MB_OK | MB_ICONINFORMATION);
            return;
        }
    }
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = pop_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"SimplePicPreviewPop";
        RegisterClassW(&wc);
        reg = true;
    }
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int fw, fh; // 窗口内容尺寸
    if (compare) {
        // 对比模式：宽扁（左右两半各能放下大图）
        fw = std::min(S(1360), (int)wa.right * 9 / 10);
        fh = std::min(S(820), (int)wa.bottom * 3 / 4);
        fw = std::max(fw, S(760));
        fh = std::max(fh, S(480));
    } else {
        // 原图模式：正方形大窗（原逻辑）
        fw = fh = std::min(S(880), std::max(S(420),
            (int)std::min<long long>((long long)wa.right * 3 / 4, (long long)wa.bottom * 3 / 4)));
    }
    RECT wr = {0, 0, fw, fh};
    AdjustWindowRect(&wr, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
    RECT mr;
    GetWindowRect(g_hwnd, &mr);
    int x = mr.left + ((mr.right - mr.left) - (wr.right - wr.left)) / 2;
    int y = mr.top + ((mr.bottom - mr.top) - (wr.bottom - wr.top)) / 2;
    if (!g_pv_pop) {
        g_pv_pop = CreateWindowExW(WS_EX_APPWINDOW, L"SimplePicPreviewPop",
                                   compare ? L"对比预览" : L"原图预览",
                                   WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                   x, y, wr.right - wr.left, wr.bottom - wr.top,
                                   g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    } else {
        MoveWindow(g_pv_pop, x, y, wr.right - wr.left, wr.bottom - wr.top, TRUE);
    }
    g_pv_pop_compare = compare; // 先置模式再加载（pop_proc 绘制按此分支）
    load_preview_popup_image(!compare);
    std::wstring name = std::filesystem::path(g_files[g_pv.item].path).filename().wstring();
    const wchar_t* prefix = compare ? L"对比预览（左原图 / 右处理后 · 滚轮缩放 · 拖动平移 · 双击复位） - "
                                    : L"原图预览（滚轮缩放 · 拖动平移 · 双击复位） - ";
    SetWindowTextW(g_pv_pop, (std::wstring(prefix) + name).c_str());
    ShowWindow(g_pv_pop, SW_SHOW);
    SetForegroundWindow(g_pv_pop);
    InvalidateRect(g_pv_pop, nullptr, TRUE);
}

// ---------- 缩略图（ImageList 构建，供缩略图视图模式） ----------
static int thumb_px_size() {
    return g_view_mode == VM_THUMB_S ? S(72) : g_view_mode == VM_THUMB_M ? S(110) : S(160);
}

// RGBA → 32bpp top-down DIB
static HBITMAP dib_from_image(const sp::Image& im) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = im.w;
    bmi.bmiHeader.biHeight = -im.h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hb && bits)
        for (int y = 0; y < im.h; ++y)
            memcpy((BYTE*)bits + (size_t)y * im.w * 4, im.row(y), (size_t)im.w * 4);
    return hb;
}

// 等比缩放后居中贴到 sz×sz 白底正方形（ImageList 单元格是正方形，
// 保持原文件比例，多出的部分留白，直接放小图会被拉伸导致失真）
static void thumb_pad_square(sp::Image& im, int sz) {
    sp::Image pad(sz, sz);
    int ox = std::max(0, (sz - im.w) / 2), oy = std::max(0, (sz - im.h) / 2);
    int cw = std::min(im.w, sz - ox), chh = std::min(im.h, sz - oy);
    for (int y = 0; y < chh; ++y)
        memcpy(pad.row(oy + y) + ox * 4, im.row(y), (size_t)cw * 4);
    im = std::move(pad);
}

// 缩略图后台解码线程：串行解码（load_image 本身重：PDF 整页渲染/大图解码），
// UI 线程只收结果，彻底消除切换视图时的卡顿
static void thumb_worker_loop() {
    for (;;) {
        ThumbJob j;
        {
            std::unique_lock<std::mutex> lk(g_thumb_mtx);
            g_thumb_cv.wait(lk, [] { return g_thumb_quit || !g_thumb_jobs.empty(); });
            if (g_thumb_quit) return;
            j = g_thumb_jobs.front();
            g_thumb_jobs.pop_front();
        }
        if (j.gen != g_thumb_gen.load()) continue; // 视图已切换：旧世代任务作废
        sp::Image im;
        if (sp::load_image(j.path, im)) {
            shrink_image_max(im, j.sz);
            thumb_pad_square(im, j.sz);
            preview_bgra(im);
        } else {
            im = sp::Image(j.sz, j.sz); // 解码失败：白块占位，保证索引对齐
        }
        PostMessageW(g_hwnd, WM_SP_THUMB_READY, (WPARAM)j.gen,
                     (LPARAM)new ThumbResult{j.path, j.sz, std::move(im)});
    }
}

static void thumb_request(int gen, const std::wstring& path, int sz) {
    {
        std::lock_guard<std::mutex> lk(g_thumb_mtx);
        g_thumb_jobs.push_back({gen, path, sz});
    }
    g_thumb_cv.notify_one();
    if (!g_thumb_worker.joinable()) g_thumb_worker = std::thread(thumb_worker_loop);
}

static void thumb_shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_thumb_mtx);
        g_thumb_quit = true;
        g_thumb_jobs.clear();
    }
    g_thumb_cv.notify_all();
    if (g_thumb_worker.joinable()) g_thumb_worker.detach(); // 进程退出随之回收
}

static HBITMAP thumb_cached(const std::wstring& path, int sz) {
    auto it = g_thumb_cache.find(path);
    if (it == g_thumb_cache.end()) return nullptr;
    auto it2 = it->second.find(sz);
    return it2 == it->second.end() ? nullptr : it2->second;
}

// 为第 idx 行补缩略图：缓存命中直接用（零解码），否则白块占位 + 后台异步解码，
// 结果经 WM_SP_THUMB_READY 回来后按索引替换（保证 ImageList 与 g_files 索引对齐）
static void add_thumb_entry(int idx) {
    if (!g_imglist || idx < 0 || idx >= (int)g_files.size()) return;
    int sz = thumb_px_size();
    HBITMAP hb = thumb_cached(g_files[idx].path, sz);
    HBITMAP white = nullptr;
    if (!hb) {
        thumb_request(g_thumb_gen.load(), g_files[idx].path, sz);
        white = dib_from_image(sp::Image(sz, sz));
        hb = white;
    }
    if (hb) ImageList_Add(g_imglist, hb, nullptr);
    if (white) DeleteObject(white);
}

// 重建整个缩略图 ImageList（切视图 / 删除文件后索引错位时调用）：
// 全部走缓存或占位，UI 线程不再同步解码任何原图
static void rebuild_thumbs() {
    if (g_view_mode == VM_LIST) return;
    ++g_thumb_gen; // 世代 +1：在途的旧解码结果作废
    ListView_SetImageList(g_list, nullptr, LVSIL_NORMAL);
    if (g_imglist) { ImageList_Destroy(g_imglist); g_imglist = nullptr; }
    int sz = thumb_px_size();
    g_imglist = ImageList_Create(sz, sz, ILC_COLOR32, (int)g_files.size() + 1, 8);
    for (size_t i = 0; i < g_files.size(); ++i) add_thumb_entry((int)i);
    ListView_SetImageList(g_list, g_imglist, LVSIL_NORMAL);
    for (size_t i = 0; i < g_files.size(); ++i) {
        LVITEMW it = {};
        it.mask = LVIF_IMAGE;
        it.iItem = (int)i;
        it.iImage = (int)i;
        ListView_SetItem(g_list, &it);
    }
    InvalidateRect(g_list, nullptr, TRUE);
}

// ---------- 开始处理 ----------
// only：要处理的索引子集；空 = 全部
static void start_stitch(const Params& p, const std::vector<int>& idxs, const std::wstring& out_dir);

static void start_processing(std::vector<int> only, const std::wstring& custom_out = L"") {
    if (g_pipe.running() || g_stitch_running) return;
    if (g_files.empty()) {
        MessageBoxW(g_hwnd, L"请先添加图片", L"SimplePic", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_module == MOD_SET_DPI + 100) { // 右键菜单设置页：即时生效，无需批量
        MessageBoxW(g_hwnd, L"右键菜单设置为开关型选项，勾选即生效，无需开始处理", L"SimplePic", MB_OK | MB_ICONINFORMATION);
        return;
    }
    int mi = module_index_by_id(g_module);
    if (kModules[mi].phase2) {
        MessageBoxW(g_hwnd, L"该功能在阶段 2 实现", L"SimplePic", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // 校验索引
    std::vector<int> idxs;
    for (int i : only)
        if (i >= 0 && i < (int)g_files.size()) idxs.push_back(i);
    if (idxs.empty())
        for (int i = 0; i < (int)g_files.size(); ++i) idxs.push_back(i);
    if (g_module == MOD_STITCH && idxs.size() < 2) {
        MessageBoxW(g_hwnd, L"长图拼接至少需要 2 张图片", L"SimplePic", MB_OK | MB_ICONINFORMATION);
        return;
    }

    Params p;
    collect_params(p);

    std::filesystem::path first(g_files[idxs[0]].path);
    std::wstring out_dir = custom_out.empty() ? (first.parent_path() / L"SimplePic_Output").wstring()
                                              : custom_out; // 右键「输出到位置…」指定的目录

    if (g_module == MOD_STITCH) { // 拼接：全部图片 → 一张长图，走专用线程
        start_stitch(p, idxs, out_dir);
        return;
    }

    SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, (int)idxs.size()));
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);

    // 多图转 PDF 合并会话：列表顺序即 PDF 页序；输出 <首文件名>_合并.pdf
    g_pdf_merge_pending = false;
    if (g_module == MOD_FORMAT_CONVERT && p.out_fmt == 3 && p.pdf_merge && idxs.size() >= 2) {
        std::vector<std::wstring> ordered;
        for (int i : idxs) ordered.push_back(g_files[i].path);
        std::wstring dst = (std::filesystem::path(out_dir) /
                            (first.stem().wstring() + L"_合并.pdf")).wstring();
        sp::pdf_merge_begin(ordered, dst);
        g_pdf_merge_pending = true;
        g_pdf_merge_dst = dst;
    }

    g_active_idx = idxs;
    for (int i : idxs) {
        g_files[i].state = ItemState::Waiting;
        ListView_SetItemText(g_list, i, 4, (LPWSTR)L"等待中");
    }

    auto files = g_files;
    HWND hwnd = g_hwnd;
    g_pipe.start(std::move(files), idxs, p, out_dir, [hwnd](int idx, ItemState st, const ItemResult& res) {
        auto* msg = new std::pair<int, ItemResult>(idx, res);
        PostMessageW(hwnd, WM_SP_ITEM, (WPARAM)st, (LPARAM)msg);
    });
    SetWindowTextW(g_status, (L"处理中… 输出到 " + out_dir).c_str());
}

// 拼接：后台线程逐张解码 → 拼合 → 写单文件；进度经 WM_SP_ITEM 推进，结果经 WM_SP_STITCH_DONE 收尾
static void start_stitch(const Params& p, const std::vector<int>& idxs, const std::wstring& out_dir) {
    std::vector<std::wstring> paths;
    for (int i : idxs) paths.push_back(g_files[i].path);
    g_active_idx = idxs;
    for (int i : idxs) {
        g_files[i].state = ItemState::Waiting;
        ListView_SetItemText(g_list, i, 4, (LPWSTR)L"等待中");
        ListView_SetItemText(g_list, i, 5, (LPWSTR)L"-");
    }
    SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, (int)idxs.size()));
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    HWND hwnd = g_hwnd;
    g_stitch_running = true;
    SetWindowTextW(g_status, L"拼接中…");
    std::thread([paths, p, out_dir, hwnd, idxs]() {
        std::filesystem::create_directories(std::filesystem::path(out_dir));
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t ts[72];
        swprintf(ts, 72, L"拼接_%04d%02d%02d_%02d%02d%02d%s",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 p.stitch_fmt == 0 ? L".png" : L".jpg");
        std::filesystem::path out = std::filesystem::path(out_dir) / ts;
        auto prog = [&idxs, hwnd](int c, int) -> bool {
            // 第 c 张加载完成 → 状态行 Done + 进度推进
            auto* msg = new std::pair<int, ItemResult>(idxs[std::min(c - 1, (int)idxs.size() - 1)], ItemResult{});
            PostMessageW(hwnd, WM_SP_ITEM, (WPARAM)ItemState::Done, (LPARAM)msg);
            return true;
        };
        ItemResult res = stitch_files(paths, out.wstring(), p.stitch_dir, p.stitch_fmt, prog);
        PostMessageW(hwnd, WM_SP_STITCH_DONE, 0, (LPARAM)new ItemResult(std::move(res)));
    }).detach();
}

// ---------- 自绘：侧边栏 ----------
// 卡片间隔行条目标记（列表 data 为该值时整行只画侧栏底色）
static const int SB_SPACER = -1000;

// 卡片行背景：同一分区的多行拼成一张连续白色圆角卡片
// （首行圆上角、末行圆下角、中间行直角拼接；圆角只出现在整卡四角）
static void draw_sidebar_card_row(const DRAWITEMSTRUCT* dis, bool first, bool last) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    RECT card = {rc.left + S(8), rc.top, rc.right - S(8), rc.bottom};
    const int R = S(10);
    if (first && last) {
        fill_round(dc, card, C_CARD, R);
    } else if (first) {
        fill_round(dc, card, C_CARD, R);
        RECT sq = {card.left, card.top + (card.bottom - card.top) / 2, card.right, card.bottom};
        fill_rect(dc, sq, C_CARD); // 底角方正：卡片延续到下一行
    } else if (last) {
        fill_round(dc, card, C_CARD, R);
        RECT sq = {card.left, card.top, card.right, card.top + (card.bottom - card.top) / 2};
        fill_rect(dc, sq, C_CARD); // 顶角方正：衔接上一行
    } else {
        fill_rect(dc, card, C_CARD);
    }
}

// ---------- 侧边栏模块图标（细线 SVG 风格，GDI 矢量手绘，随 DPI 缩放） ----------
static HPEN icon_pen(COLORREF c, int w) {
    LOGBRUSH lb = {BS_SOLID, c, 0};
    return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, w, &lb, 0, nullptr);
}

// 在以 (cx,cy) 为中心、边长 s 的正方形内画模块图标（细线描边 + 圆角端点，风格同参考图）
static void draw_module_icon(HDC dc, int mod, int cx, int cy, int s, COLORREF c) {
    int w = s / 9 < 2 ? 2 : s / 9; // 描边宽度
    HPEN pen = icon_pen(c, w);
    HPEN op = (HPEN)SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    HBRUSH wbr = CreateSolidBrush(C_WHITE); // 圆钮白芯（盖住穿过的线条）
    int l = cx - s / 2, t = cy - s / 2, r = l + s, b = t + s;
    auto seg  = [&](int x1, int y1, int x2, int y2) { MoveToEx(dc, x1, y1, nullptr); LineTo(dc, x2, y2); };
    auto boxr = [&](int x1, int y1, int x2, int y2) { Rectangle(dc, x1, y1, x2, y2); };
    auto knob = [&](int x, int y, int rad) { // 白芯圆钮（描边用当前笔）
        HBRUSH ob2 = (HBRUSH)SelectObject(dc, wbr);
        Ellipse(dc, x - rad, y - rad, x + rad, y + rad);
        SelectObject(dc, ob2);
    };

    switch (mod) {
    case MOD_COMPRESS_TARGET: { // 压缩到指定大小：上下双箭头向中线收拢
        seg(cx, t + s/8, cx, cy - s/6);
        seg(cx - s/6, cy - s/3, cx, cy - s/6); seg(cx + s/6, cy - s/3, cx, cy - s/6);
        seg(cx, b - s/8, cx, cy + s/6);
        seg(cx - s/6, cy + s/3, cx, cy + s/6); seg(cx + s/6, cy + s/3, cx, cy + s/6);
        seg(l, cy, r, cy);
        break;
    }
    case MOD_COMPRESS_LIVE: // 智能压缩：滑杆 + 圆钮
        seg(l, cy, r, cy);
        knob(cx, cy, s / 4);
        break;
    case MOD_FILL_SIZE: { // 批量增加体积：双箭头从中线向外撑开
        seg(cx, cy, cx, t + s/6);
        seg(cx - s/6, t + s/3, cx, t + s/6); seg(cx + s/6, t + s/3, cx, t + s/6);
        seg(cx, cy, cx, b - s/6);
        seg(cx - s/6, b - s/3, cx, b - s/6); seg(cx + s/6, b - s/3, cx, b - s/6);
        seg(l, cy, r, cy);
        break;
    }
    case MOD_ID_PHOTO: // 证件照：卡片相框 + 人像
        boxr(l, t, r, b);
        knob(cx, cy - s/8, s / 8);
        seg(l + s/5, b - s/5, r - s/5, b - s/5);
        break;
    case MOD_CENTER_CROP: // 居中裁剪：经典 crop 双直角
        seg(l + s*4/10, t + s/10, l + s*4/10, b - s*4/10);
        seg(l + s*4/10, b - s*4/10, r - s/10, b - s*4/10);
        seg(l + s/10, t + s*4/10, r - s*4/10, t + s*4/10);
        seg(r - s*4/10, t + s*4/10, r - s*4/10, b - s/10);
        break;
    case MOD_FORMAT_CONVERT: { // 格式转换：上下反向双箭头
        int y1 = cy - s/5, y2 = cy + s/5;
        seg(l + s/10, y1, r - s/10, y1);
        seg(r - s/10 - s/4, y1 - s/6, r - s/10, y1); seg(r - s/10 - s/4, y1 + s/6, r - s/10, y1);
        seg(r - s/10, y2, l + s/10, y2);
        seg(l + s/10 + s/4, y2 - s/6, l + s/10, y2); seg(l + s/10 + s/4, y2 + s/6, l + s/10, y2);
        break;
    }
    case MOD_ROTATE: { // 旋转：270° 圆弧折线 + 箭头（GDI Arc 方向不可控，用折线稳妥）
        const double PI = 3.141592653589793;
        double rad = s * 0.30, a0 = 60 * PI / 180, sweep = 270 * PI / 180;
        int ex = 0, ey = 0;
        for (int i = 0; i <= 14; ++i) {
            double th = a0 - sweep * i / 14;
            int px = cx + (int)(rad * cos(th)), py = cy - (int)(rad * sin(th));
            if (i == 0) MoveToEx(dc, px, py, nullptr); else LineTo(dc, px, py);
            ex = px; ey = py;
        }
        double the = a0 - sweep;
        double tx = sin(the), ty = cos(the);            // 端点切线（顺时针前进方向）
        double nx = cos(the), ny = -sin(the);           // 法线
        int tipx = ex + (int)(tx * s / 5), tipy = ey + (int)(ty * s / 5);
        seg(ex + (int)(nx * s/8), ey + (int)(ny * s/8), tipx, tipy);
        seg(ex - (int)(nx * s/8), ey - (int)(ny * s/8), tipx, tipy);
        break;
    }
    case MOD_FLIP: // 翻转镜像：中轴线 + 左右对指三角
        seg(cx, t + s/10, cx, b - s/10);
        MoveToEx(dc, l + s/8, cy - s/4, nullptr); LineTo(dc, l + s/8, cy + s/4);
        LineTo(dc, cx - s/6, cy); LineTo(dc, l + s/8, cy - s/4);
        MoveToEx(dc, r - s/8, cy - s/4, nullptr); LineTo(dc, r - s/8, cy + s/4);
        LineTo(dc, cx + s/6, cy); LineTo(dc, r - s/8, cy - s/4);
        break;
    case MOD_WATERMARK: // 文字水印：字母 T + 底部水印线
        seg(cx - s/3, t + s/5, cx + s/3, t + s/5);
        seg(cx, t + s/5, cx, b - s/4);
        seg(l, b - s/8, r, b - s/8);
        break;
    case MOD_WATERMARK_IMG: // 图片水印：相框 + 山峰 + 太阳
        boxr(l, t, r, b);
        seg(l + s/6, b - s/5, cx - s/10, cy + s/6);
        seg(cx - s/10, cy + s/6, r - s/6, b - s/5);
        knob(r - s/4, t + s/4, s / 10);
        break;
    case MOD_SET_DPI: // 修改 DPI：纵向文档 + 文本行
        boxr(l + s/5, t, r - s/5, b);
        seg(l + s/3, t + s/3, r - s/3, t + s/3);
        seg(l + s/3, cy, r - s/3, cy);
        seg(l + s/3, b - s/3, r - s/3, b - s/3);
        break;
    case MOD_STITCH: // 长图拼接：双联画框
        boxr(l, t, r, b);
        seg(cx, t, cx, b);
        break;
    case MOD_ICON_EXPORT: { // 图标导出：四宫格
        int d = s / 10;
        boxr(l + d, t + d, cx - d, cy - d);
        boxr(cx + d, t + d, r - d, cy - d);
        boxr(l + d, cy + d, cx - d, b - d);
        boxr(cx + d, cy + d, r - d, b - d);
        break;
    }
    case MOD_STRIP_META: // 清除元数据：文档 + 清除斜杠
        boxr(l + s/5, t, r - s/5, b);
        seg(l + s/3, t + s/3, r - s/3, t + s/3);
        seg(l + s/3, cy, r - s/3, cy);
        seg(l + s/5, b - s/8, r - s/5, t + s/8);
        break;
    case MOD_SET_DPI + 100: { // 右键菜单设置：三条调节杆（滑块错位排布）
        int y1 = t + s/4, y2 = cy, y3 = b - s/4, rad = s / 6;
        seg(l, y1, r, y1); knob(l + s/3, y1, rad);
        seg(l, y2, r, y2); knob(r - s/3, y2, rad);
        seg(l, y3, r, y3); knob(l + s/3, y3, rad);
        break;
    }
    default: // 兜底：小圆点
        knob(cx, cy, s / 6);
        break;
    }
    SelectObject(dc, ob);
    DeleteObject(wbr);
    DeleteObject(pen);
}

static void draw_sidebar_item(const DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    int data = (int)dis->itemData;
    fill_rect(dc, rc, C_SIDE_BG);
    if (data == SB_SPACER) return; // 卡片间隔：纯底色空行

    const int n_mods = (int)(sizeof kModules / sizeof kModules[0]);
    bool is_title = data < 0;
    // 卡片角色：标题行=首行；功能项按 kModules 相邻分组判断首/末
    bool first = is_title || data == 0 || wcscmp(kModules[data - 1].group, kModules[data].group) != 0;
    bool last  = !is_title && (data + 1 >= n_mods || wcscmp(kModules[data + 1].group, kModules[data].group) != 0);
    draw_sidebar_card_row(dis, first, last);

    if (data < 0) {
        // 分组标题：短彩色竖条 + 黑色加粗组名
        RECT bar = {rc.left + S(14), (rc.top + rc.bottom - S(12)) / 2,
                    rc.left + S(14) + S(4), (rc.top + rc.bottom + S(12)) / 2};
        fill_rect(dc, bar, group_color(-data - 1));
        wchar_t txt[64];
        ListBox_GetText(dis->hwndItem, dis->itemID, txt);
        const wchar_t* name = wcschr(txt, L' ') ? wcschr(txt, L' ') + 1 : txt;
        RECT trc = {rc.left + S(26), rc.top, rc.right - S(8), rc.bottom};
        draw_text(dc, name, trc, C_TEXT, g_font_bold); // 分区标题：黑色加粗
    } else {
        // 模块项：细线图标 + 文字；选中=淡紫圆角胶囊+紫字（图标同步变紫）
        // 最后一行行高被拉伸补底差（末组卡延伸到客户区底）：内容仍按标准行高 S(30) 定位
        int vb = rc.bottom;
        if (data == n_mods - 1 && rc.bottom - rc.top > S(30)) vb = rc.top + S(30);
        const ModuleDef& md = kModules[data];
        bool sel = (dis->itemState & ODS_SELECTED) != 0;
        if (sel) {
            RECT rr = {rc.left + S(10), rc.top + S(2), rc.right - S(10), vb - S(2)};
            fill_round(dc, rr, C_PURPLE_BG, S(8));
        }
        COLORREF ic = sel ? C_PURPLE : C_TEXT;
        draw_module_icon(dc, md.id, rc.left + S(37), (rc.top + vb) / 2, S(18), ic); // 较标题右移 13px
        RECT trc = {rc.left + S(53), rc.top, rc.right - S(8), vb};
        draw_text(dc, md.name, trc, sel ? C_PURPLE : C_TEXT, sel ? g_font_bold : g_font);
    }
}

// ---------- 自绘：工具栏按钮 / 开始处理 ----------
static void draw_tool_btn(const DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    int id = dis->CtlID;
    if (id == IDC_BTN_CLEAR) {
        fill_round(dc, rc, pressed ? RGB(0xFD, 0xF0, 0xF0) : C_WHITE, S(6));
        HPEN p = CreatePen(PS_SOLID, 1, C_RED);
        HPEN op = (HPEN)SelectObject(dc, p);
        HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, S(6), S(6)); // 圆角描边
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(p);
        draw_text(dc, L"清空列表", rc, C_RED, g_font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        COLORREF bg = pressed ? RGB(0x42, 0x3A, 0x9B) : C_PURPLE;
        fill_round(dc, rc, bg, S(6));
        const wchar_t* t = (id == IDC_BTN_ADD) ? L"＋ 添加图片" : L"添加文件夹";
        draw_text(dc, t, rc, C_WHITE, g_font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void draw_run_btn(const DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    fill_round(dc, rc, pressed ? RGB(0x42, 0x3A, 0x9B) : C_PURPLE, S(8));
    const wchar_t* t = (dis->CtlID == IDC_BTN_RUN_SEL) ? L"仅处理选中图片" : L"批量处理全部图片";
    draw_text(dc, t, rc, C_WHITE, g_font_bold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---------- 自绘：参数面板 ----------
static void draw_panel(const DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    fill_rect(dc, rc, C_PANEL_BG);
    // 区块分隔竖线（右）：面板左缘恰好居中于中间卡与面板卡之间的 S(20) 间隙；底部越过卡片底边横线 1 行（相交）
    RECT lb = {rc.left, S(8), rc.left + 1, rc.bottom - S(46) + 2};
    fill_rect(dc, lb, C_CARD_BD);
    // 标题：紫色竖条 + 模块名
    int mi = module_index_by_id(g_module);
    RECT bar = {S(10), S(16), S(10) + S(4), S(16) + S(20)};
    fill_rect(dc, bar, C_PURPLE);
    RECT trc = {S(22), S(10), rc.right - S(12), S(40)};
    draw_text(dc, kModules[mi].name, trc, C_TEXT, g_font_h1);

    // ---- 参数卡片：与预览卡同宽，全部参数控件置于卡内 ----
    RECT pcard = param_card_rect(dis->hwndItem);
    fill_round(dc, pcard, C_CARD, S(8));
    HPEN ppen = CreatePen(PS_SOLID, 1, C_CARD_BD);
    HPEN popen_ = (HPEN)SelectObject(dc, ppen);
    HBRUSH pobr = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, pcard.left, pcard.top, pcard.right + 1, pcard.bottom + 1, S(8), S(8)); // 圆角描边（直角 Rectangle 会盖掉圆角）    SelectObject(dc, popen_); SelectObject(dc, pobr);
    DeleteObject(ppen);

    // ---- 实时效果预览区（单卡，按当前模块+参数实时预测） ----
    // 几何由 preview_card_rect 统一给出（点击命中弹大图也用同一矩形）
    RECT card = preview_card_rect(dis->hwndItem);
    fill_round(dc, card, C_CARD, S(8));
    HPEN pen = CreatePen(PS_SOLID, 1, C_CARD_BD);
    HPEN open_ = (HPEN)SelectObject(dc, pen);
    HBRUSH obr = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, card.left, card.top, card.right + 1, card.bottom + 1, S(8), S(8)); // 圆角描边（直角 Rectangle 会盖掉圆角）
    SelectObject(dc, open_); SelectObject(dc, obr);
    DeleteObject(pen);

    // ---- 处理按钮卡：包裹两枚紫色处理按钮，底边与红线平齐 ----
    RECT runcard = run_card_rect(dis->hwndItem);
    fill_round(dc, runcard, C_CARD, S(8));
    HPEN rpen = CreatePen(PS_SOLID, 1, C_CARD_BD);
    HPEN ropen_ = (HPEN)SelectObject(dc, rpen);
    HBRUSH robr = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, runcard.left, runcard.top, runcard.right + 1, runcard.bottom + 1, S(8), S(8));
    SelectObject(dc, ropen_); SelectObject(dc, robr);
    DeleteObject(rpen);

    // 底部说明条：固定操作提示（预计输出体积已由列表"预计输出"列与统计卡承载）
    RECT cap = {card.left + S(8), card.bottom - S(22), card.right - S(8), card.bottom - S(4)};
    const sp::Image* img = nullptr;
    if (g_pv.item >= 0 && g_pv.item < (int)g_files.size() && g_pv.src_ok && g_pv.dst_ok)
        img = &g_pv.dst;

    // 图像区（卡内去掉说明条）
    RECT ir = {card.left + S(6), card.top + S(6), card.right - S(6), cap.top - S(2)};
    if (img && img->ok()) {
        draw_image_fit(dc, ir, *img);
    } else if (g_pv_decoding && g_pv.item >= 0 && g_pv.item < (int)g_files.size()) {
        draw_text(dc, L"大图解码中…", ir, C_GRAY, g_font_small, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        draw_text(dc, L"选中文件，此处实时预览处理效果",
                  ir, C_GRAY, g_font_small, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    draw_text(dc, L"单击预览窗打开原图和预览图对比视图",
              cap, C_TEXT2, g_font_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// ---------- 自绘：主区域背景（模块头部 + 统计卡 + 工具栏条） ----------
struct LayoutGeom {
    int side_w, panel_w, bottom_h;
    int main_x, main_w;
    int header_h, stats_h, toolbar_h;
    int list_top;
};

static LayoutGeom geom() {
    LayoutGeom g;
    g.side_w = S(210);
    g.panel_w = S(290);
    g.bottom_h = S(46);
    g.header_h = S(56);
    g.stats_h = S(68);
    g.toolbar_h = S(84); // 两行：按钮行 + 格式提示行
    g.main_x = g.side_w - S(10); // 中间区左界：距侧栏卡右缘(side_w-S(30)) S(20)，分隔竖线居中于间隙（side_w-S(20) 处）
    g.main_w = 0; // 调用处填
    g.list_top = g.header_h + g.stats_h + g.toolbar_h + S(10); // 与工具条带底留 S(10) 间距（与其他卡片间隙一致）
    return g;
}

// ---------- 表头列宽拖拽 ----------
// 列最小宽（逻辑 px，S() 换算）：文件名/尺寸/原体积/预计输出/状态/输出体积
static int k_col_min(int i) {
    static const int mn[6] = {60, 50, 50, 60, 40, 50};
    return S(mn[i]);
}

// 表头自绘矩形（与 paint_workspace 列标题行一致）
static RECT header_rect(int W) {
    LayoutGeom g = geom();
    return {g.main_x, g.list_top + S(2), W - g.panel_w - S(10), g.list_top + S(28)};
}

// 命中表头列边界（±3 逻辑px 热区）→ 返回该列索引（拖边界调该列宽），未命中返回 -1
static int header_hit_col(const POINT& pt, int W) {
    if (g_view_mode != VM_LIST || g_col_drag >= 0) return g_col_drag;
    if (!PtInRect(&header_rect(W), pt)) return -1;
    LayoutGeom g = geom();
    int lx = g.main_x; // 与列标题绘制同起点（此前误加 S(16) 偏移导致热区错位、拖拽抓不到线）
    int acc = 0;
    for (int i = 0; i < 6; ++i) { // 边界 0..5：列 i 右边界（✕ 列右边界=列表边缘，不可调）
        acc += std::max(1, g_cw[i]);
        int bx = lx + acc;
        if (pt.x >= bx - S(3) && pt.x <= bx + S(3)) return i;
    }
    return -1;
}

// 应用列宽：写用户设定 → 生效 → 通知 ListView 重排内容 → 刷新表头自绘
static void apply_col_width(int i, int w) {
    if (i < 0 || i >= 6) return;
    w = std::max(k_col_min(i), w);
    g_cw_user[i] = w;
    g_cw[i] = w;
    g_cw_applied[i] = w; // 与 ListView 实际宽度保持同步（布局去重依赖）
    if (i == 0) g_name_col = g_cw[0];
    ListView_SetColumnWidth(g_list, i, w);
    InvalidateRect(g_hwnd, nullptr, FALSE); // 主窗口自绘表头重绘
}

static void fmt_size(long long b, wchar_t* buf, size_t n) {
    if (b >= 1000 * 1024) swprintf(buf, n, L"%.1f MB", b / 1048576.0); // 超过 1000 KB 即以 MB 显示
    else swprintf(buf, n, L"%.1f KB", b / 1024.0);
}

// 以 KB 计的估算体积（带 ≈ 前缀）：≥1000 KB 转为 MB 显示
static void fmt_est_kb(long long kb, wchar_t* buf, size_t n) {
    if (kb >= 1000) swprintf(buf, n, L"≈ %.1f MB", kb / 1024.0);
    else swprintf(buf, n, L"≈ %lld KB", kb);
}

// ---------- 显示方式切换条（列表 / 缩略图 小·中·大），绘制与命中共用 ----------
static const wchar_t* kViewSegTxt[4] = {L"列表", L"缩略图 小", L"缩略图 中", L"缩略图 大"};

static RECT view_seg_rect(const RECT& client, int i) {
    LayoutGeom g = geom();
    // 移到工具条按钮行（与“添加图片”同排）：右对齐红线，每个按钮与“添加图片”同宽同高
    const int bw = S(96), bh = S(32), gap = S(6);
    int right = client.right - g.panel_w - S(10); // 红线（工具条右缘）
    int r = right - (3 - i) * (bw + gap);
    int ty = g.header_h + g.stats_h + S(10);      // 与工具条按钮同一行
    return {r - bw, ty, r, ty + bh};
}

static int hit_view_seg(const RECT& client, const POINT& pt) {
    for (int i = 0; i < 4; ++i)
        if (PtInRect(&view_seg_rect(client, i), pt)) return i;
    return -1;
}

// ---------- 侧边栏自绘滚动条（细圆角浅色，与窗口右缘留边距） ----------
static bool g_sb_drag = false; // 滑块拖拽中
static int  g_sb_grab = 0;     // 按下点相对滑块顶部的偏移

static RECT side_sb_strip() {
    LayoutGeom g = geom();
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    RECT s = {g.side_w - S(30), 0, g.side_w - S(20), rc.bottom - g.bottom_h}; // 条带宽 S(10)，恰在侧栏卡片与内容区之间的 10px 空隙内
    return s;
}
// 末行附加高度：末组卡底边延伸量（客户区比内容多出的部分并入最后一行行高）
static int g_side_extra = 0;

static int side_item_h(int i) {
    int base = (int)ListBox_GetItemData(g_sidebar, i) == SB_SPACER ? S(10) : S(30); // 间隔行：卡片间距
    if (i == ListBox_GetCount(g_sidebar) - 1) base += g_side_extra; // 末行=末组卡最后一项
    return base;
}
static int side_content_h() {
    int n = ListBox_GetCount(g_sidebar), t = 0;
    for (int i = 0; i < n; ++i) t += side_item_h(i);
    return t;
}
static int side_view_h() { RECT s = side_sb_strip(); return s.bottom - s.top; }
static void side_invalidate() {
    RECT s = side_sb_strip();
    InvalidateRect(g_hwnd, &s, FALSE);
}
// 滑块几何（客户区坐标）。返回 false = 内容不超高，无滑块
static bool side_thumb_geom(RECT* track, RECT* thumb) {
    RECT s = side_sb_strip();
    *track = {s.left + S(1), s.top + S(4), s.right - S(1), s.bottom - S(4)};
    int content = side_content_h(), view = s.bottom - s.top;
    if (content <= view) return false;
    int tl = track->bottom - track->top;
    int th = std::max(S(28), (int)((double)tl * view / content));
    int off = 0;
    for (int i = 0, top = ListBox_GetTopIndex(g_sidebar); i < top; ++i) off += side_item_h(i);
    int ty = track->top + (int)((tl - th) * (double)off / (content - view) + 0.5);
    ty = std::max((int)track->top, std::min(ty, (int)track->bottom - th));
    *thumb = {track->left, ty, track->right, ty + th};
    return true;
}
static void side_scroll_to_offset(int off) { // 像素偏移 → top 索引
    int n = ListBox_GetCount(g_sidebar);
    if (n <= 0) return;
    int acc = 0, top = 0;
    for (int i = 0; i < n; ++i) {
        if (acc >= off) { top = i; break; }
        acc += side_item_h(i);
        top = i + 1;
    }
    ListBox_SetTopIndex(g_sidebar, std::min(top, n - 1));
    side_invalidate();
}
static void side_wheel_scroll(int delta) { // ±120 的倍数，向下滚为负
    int n = ListBox_GetCount(g_sidebar);
    if (n <= 0 || side_content_h() <= side_view_h()) return;
    int lines = (delta / WHEEL_DELTA) * 3;
    int top = ListBox_GetTopIndex(g_sidebar) - lines;
    ListBox_SetTopIndex(g_sidebar, std::max(0, std::min(top, n - 1)));
    side_invalidate();
}
static void draw_side_scrollbar(HDC dc) {
    RECT s = side_sb_strip();
    fill_rect(dc, s, C_SIDE_BG);
    RECT track, thumb;
    if (!side_thumb_geom(&track, &thumb)) return;
    // 浅色圆角滑块：常态淡灰、拖拽加深一档
    fill_round(dc, thumb, g_sb_drag ? RGB(0xC2, 0xC0, 0xBB) : RGB(0xDA, 0xD8, 0xD3), S(4));
}
// 侧边栏列表框子类：滚轮转发给主窗口统一处理（原生滚动条已移除，滚轮自管）
static LRESULT CALLBACK side_list_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_MOUSEWHEEL) { SendMessageW(g_hwnd, WM_MOUSEWHEEL, wp, lp); return 0; }
    WNDPROC op = (WNDPROC)GetWindowLongPtrW(h, GWLP_USERDATA);
    return CallWindowProcW(op, h, m, wp, lp);
}

// ---------- 品牌卡 LOGO 位图：资源内嵌 PNG（ID=2 RCDATA）→ 预乘 ARGB DIB ----------
static HBITMAP g_logo_bmp = nullptr;
static int g_logo_w = 0, g_logo_h = 0;

static HBITMAP load_logo_bitmap() {
    if (g_logo_bmp) return g_logo_bmp;
    HRSRC hr = FindResourceW(nullptr, MAKEINTRESOURCEW(2), (LPCWSTR)RT_RCDATA);
    if (!hr) return nullptr;
    HGLOBAL hg = LoadResource(nullptr, hr);
    if (!hg) return nullptr;
    const uint8_t* data = (const uint8_t*)LockResource(hg);
    DWORD len = SizeofResource(nullptr, hr);
    if (!data || !len) return nullptr;
    Image img;
    if (!load_image_from_memory(data, len, img) || !img.ok()) return nullptr;
    // RGBA → 预乘 BGRA（AlphaBlend + AC_SRC_ALPHA 要求预乘）
    std::vector<uint8_t> bgra((size_t)img.w * img.h * 4);
    for (size_t i = 0; i < bgra.size(); i += 4) {
        unsigned a = img.px[i + 3];
        bgra[i]     = (uint8_t)(img.px[i + 2] * a / 255); // B
        bgra[i + 1] = (uint8_t)(img.px[i + 1] * a / 255); // G
        bgra[i + 2] = (uint8_t)(img.px[i]     * a / 255); // R
        bgra[i + 3] = (uint8_t)a;
    }
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = img.w;
    bmi.bmiHeader.biHeight = -img.h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return nullptr;
    memcpy(bits, bgra.data(), bgra.size());
    g_logo_bmp = bmp;
    g_logo_w = img.w;
    g_logo_h = img.h;
    return bmp;
}

static void paint_workspace(HDC dc, const RECT& rc) {
    LayoutGeom g = geom();
    g.main_w = rc.right - g.panel_w - g.main_x;
    int W = rc.right, H = rc.bottom;
    fill_rect(dc, rc, C_SIDE_BG); // 工作区背景与侧栏同色：白色卡片浮在浅灰底上（参考附图）

    // ---- 侧栏顶部品牌卡：LOGO + 软件名 + 版本号（与下方模块卡片同宽同圆角风格） ----
    {
        const int hh = S(66); // 顶部边距 S(8) + 卡高 S(48) + 间隔 S(10)：列表框下移量同步在 layout_controls
        RECT card = {S(8), S(8), g.side_w - S(30), hh - S(10)};
        fill_round(dc, card, C_CARD, S(10));
        RECT logo = {card.left + S(9), card.top + S(7), card.left + S(9) + S(34), card.top + S(7) + S(34)};
        // LOGO：资源内嵌 PNG（品牌原图）AlphaBlend 缩放到 S(34)，带圆角 alpha
        HBITMAP lbmp = load_logo_bitmap();
        if (lbmp) {
            HDC mdc = CreateCompatibleDC(dc);
            HGDIOBJ olb = SelectObject(mdc, lbmp);
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dc, logo.left, logo.top, logo.right - logo.left, logo.bottom - logo.top,
                       mdc, 0, 0, g_logo_w, g_logo_h, bf);
            SelectObject(mdc, olb);
            DeleteDC(mdc);
        } else {
            fill_round(dc, logo, C_PURPLE, S(8)); // 资源缺失兜底：纯紫圆角块
        }
        RECT nrc = {logo.right + S(10), logo.top, card.right - S(8), logo.top + S(19)};
        draw_text(dc, L"SimplePic", nrc, C_TEXT, g_font_bold); // 软件名：黑色加粗
        RECT vrc = {logo.right + S(10), logo.top + S(18), card.right - S(8), logo.top + S(32)};
        draw_text(dc, L"图片处理系统 v1.0", vrc, C_TEXT2, g_font_small); // 版本号：灰小字
    }

    draw_side_scrollbar(dc); // 侧边栏右侧自绘滚动条（含浅色滑块）

    // ---- 区块分隔竖线（左）：居中于侧栏卡与中间卡之间的 S(20) 间隙，底部越过卡片底边横线 1 行（相交） ----
    RECT dvl = {g.side_w - S(20), S(8), g.side_w - S(20) + 1, H - g.bottom_h + 2};
    fill_rect(dc, dvl, C_CARD_BD);

    // ---- 中间操作区：左界红线 main_x、右界红线 W-panel_w-S(10)（距参数面板 10px），越界一律裁剪 ----
    SaveDC(dc);
    IntersectClipRect(dc, g.main_x, 0, W - g.panel_w - S(10), H);

    // ---- 模块头部 ----
    int mi = module_index_by_id(g_module);
    const ModuleDef& md = kModules[mi];
    RECT icon = {g.main_x, S(11), g.main_x + S(34), S(11) + S(34)};
    fill_round(dc, icon, group_color(group_index(md.group)), S(8));
    // 线性图标与侧栏功能列表同款（draw_module_icon），白色描边居中于色块
    draw_module_icon(dc, md.id, icon.left + S(17), icon.top + S(17), S(20), C_WHITE);
    RECT nrc = {icon.right + S(12), S(8), rc.right, S(30)};
    draw_text(dc, md.name, nrc, C_TEXT, g_font_h1);
    RECT drc = {icon.right + S(12), S(30), rc.right, S(52)};
    draw_text(dc, md.desc, drc, C_TEXT2, g_font_small);
    RECT hline = {g.main_x, g.header_h - 1, W - g.panel_w, g.header_h};
    fill_rect(dc, hline, C_CARD_BD);

    // ---- 统计卡 ----
    int card_gap = S(10);
    int card_w = (g.main_w - S(10) - card_gap * 3) / 4; // 右侧留 S(10) 不越过右红线
    int cy = g.header_h + S(8);
    int ch = g.stats_h - S(16);
    long long total = 0;
    for (auto& f : g_files) total += f.bytes;

    struct Card { const wchar_t* label; const wchar_t* value; bool green; };
    wchar_t v1[32], v2[32], v3[32], v4[32];
    swprintf(v1, 32, L"%d 张", (int)g_files.size());
    fmt_size(total, v2, 32);
    // 预计输出：逐文件估算求和（与列表"预计输出"列同一逻辑）
    Params p;
    collect_params(p);
    if (g_files.empty()) {
        wcscpy_s(v3, L"—");
    } else {
        long long sum = 0;
        for (auto& f : g_files) {
            long long kb = est_kb_for(f, p);
            sum += kb >= 0 ? kb : (f.bytes + 1023) / 1024;
        }
        fmt_est_kb(sum, v3, 32);
    }
    swprintf(v4, 32, L"%u 线程", std::thread::hardware_concurrency());
    Card cards[4] = {
        {L"已选文件", v1, false}, {L"原始合计", v2, false},
        {L"预计输出", v3, true},  {L"并行处理", v4, false},
    };
    for (int i = 0; i < 4; ++i) {
        int right = (i == 3) ? g.main_x + g.main_w - S(10) // 末卡右缘精确贴红线（消除整除取整误差）
                             : g.main_x + i * (card_w + card_gap) + card_w;
        RECT cr = {g.main_x + i * (card_w + card_gap), cy, right, cy + ch};
        fill_round(dc, cr, cards[i].green ? C_GREEN_BG : C_CARD_BG, S(8));
        RECT lrc = {cr.left + S(12), cr.top + S(6), cr.right - S(8), cr.top + S(24)};
        draw_text(dc, cards[i].label, lrc, C_TEXT2, g_font_small);
        // 数值右对齐 + 放大加粗强调（视觉锚点：卡右缘）
        RECT vrc = {cr.left + S(12), cr.top + S(20), cr.right - S(12), cr.bottom - S(2)};
        draw_text(dc, cards[i].value, vrc, cards[i].green ? C_GREEN : C_TEXT, g_font_num,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    }

    // ---- 工具栏条（两行：上行按钮，下行格式说明） ----
    RECT strip = {g.main_x, g.header_h + g.stats_h, W - g.panel_w - S(10), g.header_h + g.stats_h + g.toolbar_h}; // 右缘=红线
    fill_round(dc, strip, C_TOOL_STRIP, S(8)); // 圆角条带，与其他卡片倒角一致
    RECT hint = {g.main_x + S(16), strip.top + S(50), strip.right - S(12), strip.bottom};
    draw_text(dc, L"支持 JPG、PNG、GIF、BMP、WebP、TIFF、HEIC、PDF 格式 · 可直接拖入图片或文件夹",
              hint, C_TEXT2, g_font_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // ---- 显示方式切换条 ----
    for (int i = 0; i < 4; ++i) {
        RECT sr = view_seg_rect(rc, i);
        if (i == g_view_mode) {
            fill_round(dc, sr, C_PURPLE, S(6));
            draw_text(dc, kViewSegTxt[i], sr, C_WHITE, g_font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            fill_round(dc, sr, C_WHITE, S(6));
            HPEN sp_ = CreatePen(PS_SOLID, 1, C_CARD_BD);
            HPEN spos = (HPEN)SelectObject(dc, sp_);
            HBRUSH sob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, sr.left, sr.top, sr.right + 1, sr.bottom + 1, S(6), S(6)); // 圆角描边
            SelectObject(dc, spos); SelectObject(dc, sob);
            DeleteObject(sp_);
            draw_text(dc, kViewSegTxt[i], sr, C_TEXT2, g_font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // ---- 列标题行（自绘，仅列表模式；缩略图模式无列标题） ----
    if (g_view_mode == VM_LIST) {
        RECT hdr = {g.main_x, g.list_top + S(2), W - g.panel_w - S(10), g.list_top + S(28)}; // 右缘=红线
        fill_round(dc, hdr, C_CARD, S(8)); // 圆角列标题行，与其他卡片倒角一致
        int lx = g.main_x; // 列标题与列表同左界（红线对齐）
        int acc = 0;
        struct { const wchar_t* t; } titles[6] = {
            {L"文件名"}, {L"尺寸"}, {L"原体积"}, {L"预计输出"}, {L"状态"}, {L"输出体积"},
        };
        for (int i = 0; i < 6; ++i) {
            RECT trc = {lx + acc + S(6), hdr.top, lx + acc + std::max(1, g_cw[i]), hdr.bottom};
            draw_text(dc, titles[i].t, trc, C_TEXT2, g_font_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            // 列间竖线分隔
            HPEN vp = CreatePen(PS_SOLID, 1, C_CARD_BD);
            HPEN vop = (HPEN)SelectObject(dc, vp);
            int vx = lx + acc + std::max(1, g_cw[i]);
            MoveToEx(dc, vx, hdr.top + S(6), nullptr);
            LineTo(dc, vx, hdr.bottom - S(6));
            SelectObject(dc, vop);
            DeleteObject(vp);
            acc += std::max(1, g_cw[i]);
        }
        // 第 7 列（✕ 移除按钮列）：标题“移除”，与 ✕ 按钮同为中心对齐
        RECT xtrc = {lx + acc, hdr.top, hdr.right, hdr.bottom};
        draw_text(dc, L"移除", xtrc, C_TEXT2, g_font_small, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT hline2 = {g.main_x + 1, hdr.bottom - 1, W - g.panel_w - S(10) - 1, hdr.bottom}; // 左右各收 1px 避开圆角
        fill_rect(dc, hline2, C_CARD_BD);
    }

    RestoreDC(dc, -1); // 解除中间区裁剪

    // ---- 底部状态区 ----
    RECT bline = {0, H - g.bottom_h, W, H - g.bottom_h + 1};
    fill_rect(dc, bline, C_CARD_BD);
}

static void layout_controls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    g_last_cx = W; g_last_cy = H; // 记录本次布局的客户区尺寸（WM_SIZE 去重用）
    LayoutGeom g = geom();
    g.main_w = W - g.panel_w - g.main_x;

    MoveWindow(g_sidebar, 0, S(66), g.side_w - S(22), H - g.bottom_h - S(66), TRUE); // 顶部 S(66)：品牌卡（LOGO+名称+版本）；右侧 S(22)：滚动条条带 + 右缘边距
    { // 末组卡随窗体增高：客户区比内容多出的高度并入最后一行行高（末组卡底=客户区底=红线，与文件列表卡对齐）
      // 性能：仅高度变化时才重设行高并整列表重绘（拖拽横向尺寸时跳过，避免每像素全列表重绘）
        static int s_last_side_h = -1;
        RECT src;
        GetClientRect(g_sidebar, &src);
        if (src.bottom != s_last_side_h) {
            s_last_side_h = src.bottom;
            InvalidateRect(g_sidebar, nullptr, TRUE); // 高度变化 → 重绘全部行（末组卡底边延伸随窗体高度更新）
            int n = ListBox_GetCount(g_sidebar);
            g_side_extra = 0;
            int content = 0;
            for (int i = 0; i < n; ++i) content += side_item_h(i);
            g_side_extra = std::max(0, (int)src.bottom - content);
            for (int i = 0; i < n; ++i)
                SendMessageW(g_sidebar, LB_SETITEMHEIGHT, (WPARAM)i, MAKELPARAM(side_item_h(i), 0));
        }
    }
    int toolbar_y = g.header_h + g.stats_h;
    MoveWindow(GetDlgItem(hwnd, IDC_BTN_ADD), g.main_x + S(16), toolbar_y + S(10), S(96), S(32), TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_BTN_ADDDIR), g.main_x + S(120), toolbar_y + S(10), S(96), S(32), TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_BTN_CLEAR), g.main_x + S(224), toolbar_y + S(10), S(88), S(32), TRUE);
    bool list_mode = (g_view_mode == VM_LIST);
    int list_y = g.list_top + (list_mode ? S(30) : S(4));
    MoveWindow(g_list, g.main_x, list_y, g.main_w - S(10), H - g.bottom_h - list_y, TRUE); // 底边=红线，与侧栏末组卡/处理按钮卡水平对齐
    { // 文件列表卡补倒角：区域裁剪（ListView 自身无圆角能力）。
      // 列表模式仅圆底角（顶部衔接列标题卡），缩略图模式四角全圆；半径与其他卡一致 S(10)
      // 性能：仅列表尺寸/视图模式变化时才重建区域（SetWindowRgn 每像素调用是拖拽卡顿主因之一）
        static int s_last_list_w = -1, s_last_list_h = -1, s_last_list_mode = -1;
        RECT lrc;
        GetClientRect(g_list, &lrc);
        if (lrc.right != s_last_list_w || lrc.bottom != s_last_list_h || (int)list_mode != s_last_list_mode) {
            s_last_list_w = lrc.right;
            s_last_list_h = lrc.bottom;
            s_last_list_mode = (int)list_mode;
            int r = S(10);
            HRGN rgn = CreateRoundRectRgn(0, 0, lrc.right + 1, lrc.bottom + 1, r, r);
            if (list_mode) {
                HRGN top = CreateRectRgn(0, 0, lrc.right + 1, r); // 顶角方正，衔接上方列标题
                CombineRgn(rgn, rgn, top, RGN_OR);
                DeleteObject(top);
            }
            SetWindowRgn(g_list, rgn, FALSE); // 不同步重绘：主窗口随后的整窗重绘已覆盖
        }
    }
    if (list_mode) {
        // 7 列宽度：✕ 移除列固定宽不随窗体变化；窗体加宽时富余宽度全给文件名列；
        // 用户拖拽过的列尊重设定（不低于最小宽）；总宽不足时按比例压回，
        // 保证移除按钮 ✕ 在最小窗口宽度下也始终可见（无横向滚动遮挡）
        RECT lrc;
        GetClientRect(g_list, &lrc);
        int lw = lrc.right;
        int min_name = S(90), min_x = S(32);
        const int w_x = S(36); // ✕ 移除列固定宽
        bool user_mode = false;
        for (int i = 0; i < 6; ++i) if (g_cw_user[i] > 0) { user_mode = true; break; }
        if (user_mode) {
            int sum = 0;
            for (int i = 0; i < 6; ++i) { g_cw[i] = std::max(k_col_min(i), g_cw_user[i]); sum += g_cw[i]; }
            int avail = lw - w_x;
            if (avail > sum) { // 窗体加宽：富余全给文件名列
                g_cw[0] += avail - sum;
            } else if (avail < sum) { // 不足：按比例压回（尊重最小宽）
                double k = (double)avail / sum;
                if (k < 0.3) k = 0.3;
                sum = 0;
                for (int i = 0; i < 6; ++i) { g_cw[i] = std::max(k_col_min(i), (int)(g_cw[i] * k)); sum += g_cw[i]; }
            }
            g_cw[6] = std::max(min_x, lw - sum);
        } else {
            static const int base5[5] = {90, 80, 90, 90, 90}; // 尺寸/原体积/预计输出/状态/输出体积
            int sum5 = S(90 + 80 + 90 + 90 + 90);
            if (lw >= sum5 + min_name + w_x) {
                g_cw[0] = lw - sum5 - w_x; // 文件名列吃掉全部富余（不设上限），✕ 列固定贴最右
                for (int i = 0; i < 5; ++i) g_cw[i + 1] = S(base5[i]);
                g_cw[6] = w_x;
            } else {
                int avail = lw - min_name - min_x;
                double k = (double)avail / sum5;
                if (k < 0.5) k = 0.5;
                g_cw[0] = min_name;
                for (int i = 0; i < 5; ++i) g_cw[i + 1] = (int)(S(base5[i]) * k);
                int used = g_cw[0] + min_x;
                for (int i = 0; i < 5; ++i) used += g_cw[i + 1];
                g_cw[6] = std::max(min_x, lw - used);
            }
        }
        g_name_col = g_cw[0];
        for (int i = 0; i < 7; ++i)
            if (g_cw[i] != g_cw_applied[i]) { // 仅下发宽度变化的列（拖拽窗体时通常只有文件名列，避免每像素 7 次列表重排）
                ListView_SetColumnWidth(g_list, i, g_cw[i]);
                g_cw_applied[i] = g_cw[i];
            }
    }
    MoveWindow(g_param_panel, W - g.panel_w, 0, g.panel_w, H - g.bottom_h, TRUE);
    RECT prc;
    GetClientRect(g_param_panel, &prc);
    RECT rrc = run_card_rect(g_param_panel); // 与处理按钮卡同源：按钮居卡内，卡底=红线
    MoveWindow(g_run_sel_btn, rrc.left + S(12), rrc.top + S(11), rrc.right - rrc.left - S(24), S(38), TRUE);
    MoveWindow(g_run_btn, rrc.left + S(12), rrc.top + S(57), rrc.right - rrc.left - S(24), S(38), TRUE);
    MoveWindow(g_progress, S(8), H - S(34), W - S(16), S(14), TRUE);
    MoveWindow(g_status, S(8), H - S(18), W - S(16), S(16), TRUE);
    layout_param_scroll(); // 面板高度变化：参数区滚动范围/可见性重算
    InvalidateRect(g_param_panel, nullptr, TRUE); // TRUE：连背景一起重绘，防残影
}

// ---------- 显示方式切换 ----------
static const wchar_t* state_text(ItemState st); // 定义在下方

// 重建列表所有行为纯文本行（索引与 g_files 对齐）：
// 缩略图模式给行设置过 iImage，切回列表模式后 ImageList 已解绑，
// comctl32 会对"带图标索引但无 ImageList"的行绘制红叉缺图占位符
static void rebuild_list_rows() {
    ListView_DeleteAllItems(g_list);
    wchar_t buf[64];
    for (size_t i = 0; i < g_files.size(); ++i) {
        const FileItem& f = g_files[i];
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = (int)i;
        std::filesystem::path fp(f.path);
        std::wstring fname = fp.filename().wstring();
        lvi.pszText = (LPWSTR)fname.c_str();
        ListView_InsertItem(g_list, &lvi);
        swprintf(buf, 64, L"%d×%d", f.w, f.h);
        ListView_SetItemText(g_list, (int)i, 1, buf);
        fmt_size(f.bytes, buf, 64);
        ListView_SetItemText(g_list, (int)i, 2, buf);
        Params p;
        collect_params(p);
        long long kb = est_kb_for(f, p);
        if (kb < 0) wcscpy_s(buf, L"—");
        else fmt_est_kb(kb, buf, 64);
        ListView_SetItemText(g_list, (int)i, 3, buf);
        ListView_SetItemText(g_list, (int)i, 4, (LPWSTR)state_text(f.state));
        if (f.state == ItemState::Done && f.result.out_bytes > 0) {
            fmt_size(f.result.out_bytes, buf, 64);
            ListView_SetItemText(g_list, (int)i, 5, buf);
        } else {
            ListView_SetItemText(g_list, (int)i, 5, (LPWSTR)L"-");
        }
    }
}

static void set_view_mode(int m) {
    g_view_mode = m;
    g_x_hover_row = -1; // 切视图后行几何变化，悬停态作废
    // 解绑并销毁旧 ImageList
    ListView_SetImageList(g_list, nullptr, LVSIL_NORMAL);
    if (g_imglist) { ImageList_Destroy(g_imglist); g_imglist = nullptr; }
    LONG style = GetWindowLongW(g_list, GWL_STYLE) & ~LVS_TYPEMASK;
    if (m == VM_LIST) {
        style |= LVS_REPORT;
        SetWindowLongW(g_list, GWL_STYLE, style);
        rebuild_list_rows(); // 清除缩略图 iImage 关联，杜绝红叉占位符
    } else {
        style |= LVS_ICON | LVS_AUTOARRANGE;
        SetWindowLongW(g_list, GWL_STYLE, style);
        int sz = thumb_px_size();
        ListView_SetIconSpacing(g_list, sz + S(12), sz + S(44)); // 图标 + 下方文件名
        rebuild_thumbs();
    }
    layout_controls(g_hwnd);
    // 图标视图 SetIconSpacing/AutoArrange 会重排图标：旧位置的 ✕ 像素若不被擦除
    // 会留下残影（视觉上每个缩略图出现两个 ✕），布局完成后强制整表擦除重绘
    InvalidateRect(g_list, nullptr, TRUE);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------- 面板消息转发器（STATIC 不转发子控件通知） ----------
static WNDPROC g_panel_oldproc = nullptr; // 原STATIC类过程（自绘/擦背景逻辑都在它里面）

static LRESULT CALLBACK panel_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_LBUTTONDOWN: {
        // 点击预览卡 → 弹出大图预览窗
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
        if (PtInRect(&preview_card_rect(h), pt)) open_preview_popup(true);
        break;
    }
    case WM_SETCURSOR: {
        // 预览卡上显示手型光标（可点击放大）
        if (LOWORD(lp) == HTCLIENT) {
            DWORD ppos = GetMessagePos();
            POINT pt = {(short)LOWORD(ppos), (short)HIWORD(ppos)};
            ScreenToClient(h, &pt);
            SetCursor(LoadCursorW(nullptr, PtInRect(&preview_card_rect(h), pt) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    case WM_VSCROLL: {
        // 参数区滚动条（SCROLLBAR 子控件）：本地消费，不转发
        if ((HWND)lp == g_param_scrollbar) { param_scroll_by(LOWORD(wp), HIWORD(wp)); return 0; }
        return SendMessageW(GetParent(h), m, wp, lp);
    }
    case WM_MOUSEWHEEL: // 悬停参数区时滚轮滚动参数（Win10+ 按光标路由）
        if (param_wheel((short)HIWORD(wp))) return 0;
        break;
    case WM_COMMAND:
    case WM_HSCROLL: // 智能压缩滑块（TRACKBAR 通知走 WM_HSCROLL）
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return SendMessageW(GetParent(h), m, wp, lp);
    }
    // 子类化必须回调原过程：走 DefWindowProcW 会丢失 STATIC 的
    // WM_PAINT→WM_DRAWITEM（自绘）与 WM_ERASEBKGND→WM_CTLCOLORSTATIC（擦背景）
    // 能力，面板永远不重绘自己 → 切模块时旧控件残影擦不掉，与新控件重叠
    return CallWindowProcW(g_panel_oldproc, h, m, wp, lp);
}

// ---------- 状态徽章颜色 ----------
static COLORREF state_color(ItemState st) {
    switch (st) {
    case ItemState::Done:    return C_GREEN;
    case ItemState::Running: return C_AMBER;
    case ItemState::Failed:  return C_RED;
    default:                 return C_GRAY;
    }
}

static const wchar_t* state_text(ItemState st) {
    switch (st) {
    case ItemState::Running: return L"处理中";
    case ItemState::Done:    return L"已完成";
    case ItemState::Failed:  return L"失败";
    default:                 return L"等待中";
    }
}

static COLORREF state_pill_bg(ItemState st) {
    switch (st) {
    case ItemState::Done:    return RGB(0xEA, 0xF7, 0xEF);
    case ItemState::Running: return RGB(0xFE, 0xF3, 0xE2);
    case ItemState::Failed:  return RGB(0xFD, 0xEC, 0xEC);
    default:                 return RGB(0xF1, 0xF2, 0xF4);
    }
}

// ---------- 窗口过程 ----------
static void dbg_log(const wchar_t* s) {
    FILE* f = nullptr;
    _wfopen_s(&f, L"E:\\TRAE Project\\SimplePic\\build\\dbg.log", L"a, ccs=UTF-8");
    if (f) { fwprintf(f, L"%s\n", s); fclose(f); }
}
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        using FnGetDpi = UINT(WINAPI*)(HWND);
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
            auto get_dpi = (FnGetDpi)GetProcAddress(u32, "GetDpiForWindow");
            g_dpi = get_dpi ? (int)get_dpi(hwnd) : 96;
        }
        if (HDC hdc = GetDC(hwnd)) {
            if (g_dpi <= 96) g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
            ReleaseDC(hwnd, hdc);
        }
        g_font       = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        g_font_bold  = CreateFontW(-S(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        g_font_small = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        g_font_pv    = CreateFontW(-(S(11) + 4), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI"); // 预览对比窗标签：small 累计加大 4px
        g_font_h1    = CreateFontW(-S(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        g_font_num   = CreateFontW(-S(20), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI"); // 统计卡数字：放大强调
        g_brush_white = CreateSolidBrush(C_CARD); // 参数控件在卡片内：背景=卡片色 F8F9FD
        g_brush_panel = CreateSolidBrush(C_PANEL_BG);
        g_brush_side  = CreateSolidBrush(C_SIDE_BG);
        g_brush_sep   = CreateSolidBrush(C_CARD_BD); // 参数面板分组横线

        // 侧边栏（自绘）；无原生滚动条——滚动条条带画在侧栏与内容区之间的空隙里；
        // OWNERDRAWVARIABLE：间隔行(S(10))与功能行(S(30))高度不同，必须逐条目设高
        g_sidebar = CreateWindowW(L"LISTBOX", nullptr,
                                  WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_HASSTRINGS
                                  | LBS_OWNERDRAWVARIABLE | LBS_NOINTEGRALHEIGHT,
                                  0, 0, S(210), 600, hwnd, (HMENU)IDC_SIDEBAR, nullptr, nullptr);
        SendMessageW(g_sidebar, WM_SETFONT, (WPARAM)g_font, TRUE);
        wchar_t last_group[64] = L"";
        for (int i = 0; i < (int)(sizeof kModules / sizeof kModules[0]); ++i) {
            if (wcscmp(last_group, kModules[i].group) != 0) {
                wcscpy_s(last_group, kModules[i].group);
                int gi = ListBox_AddString(g_sidebar, (L"▪ " + std::wstring(last_group)).c_str());
                ListBox_SetItemData(g_sidebar, gi, -(group_index(last_group) + 1)); // 负数=分组标题
            }
            std::wstring label = std::wstring(L"   ") + kModules[i].name;
            int idx = ListBox_AddString(g_sidebar, label.c_str());
            ListBox_SetItemData(g_sidebar, idx, i);
            if (kModules[i].id == g_module) g_sidebar_sel = idx;
            // 分区末尾插入间隔行（卡片之间的空隙）；末组不再加——末组卡底边要延伸到客户区底
            bool group_end = (i + 1 >= (int)(sizeof kModules / sizeof kModules[0]))
                          || wcscmp(kModules[i + 1].group, kModules[i].group) != 0;
            if (group_end && i + 1 < (int)(sizeof kModules / sizeof kModules[0])) {
                int sp = ListBox_AddString(g_sidebar, L"");
                ListBox_SetItemData(g_sidebar, sp, SB_SPACER);
            }
        }
        // OWNERDRAWVARIABLE 下 MEASUREITEM 在 AddString 瞬间触发、itemData 尚未写入，
        // 间隔行高度设不进去 → 列表填充完后逐条目用 LB_SETITEMHEIGHT 补设真实高度
        for (int i = 0, n = ListBox_GetCount(g_sidebar); i < n; ++i)
            SendMessageW(g_sidebar, LB_SETITEMHEIGHT, (WPARAM)i, MAKELPARAM(side_item_h(i), 0));
        ListBox_SetCurSel(g_sidebar, g_sidebar_sel);
        { // 子类化：滚轮转发主窗口（自绘滚动条滚动）
            WNDPROC op = (WNDPROC)SetWindowLongPtrW(g_sidebar, GWLP_WNDPROC, (LONG_PTR)side_list_proc);
            SetWindowLongPtrW(g_sidebar, GWLP_USERDATA, (LONG_PTR)op);
        }

        // 工具栏按钮（自绘）
        struct { int id; const wchar_t* text; } btns[] = {
            {IDC_BTN_ADD, L"＋ 添加图片"}, {IDC_BTN_ADDDIR, L"添加文件夹"}, {IDC_BTN_CLEAR, L"清空列表"},
        };
        for (auto& b : btns) {
            HWND hb = CreateWindowW(L"BUTTON", b.text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                    0, 0, S(96), S(32), hwnd, (HMENU)(INT_PTR)b.id, nullptr, nullptr);
            SendMessageW(hb, WM_SETFONT, (WPARAM)g_font, TRUE);
        }

        // 文件列表（LVS_NOCOLUMNHEADER：不创建原生 SysHeader32 表头——
        // 该表头在 Per-Monitor DPI 感知进程里存在自激重绘风暴（~150次/秒），
        // 已实测确认（隐藏表头风暴即停）；列标题改由主窗口自绘，见 paint_workspace
        g_list = CreateWindowW(WC_LISTVIEWW, nullptr,
                               WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS,
                               0, 0, 600, 400, hwnd, (HMENU)IDC_LIST, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(g_list, C_CARD);
        ListView_SetTextBkColor(g_list, C_CARD);
        // 子类化列表：跟踪 ✕ 悬停（凸显效果）与手型光标
        g_list_oldproc = (WNDPROC)SetWindowLongPtrW(g_list, GWLP_WNDPROC, (LONG_PTR)list_proc);
        struct { const wchar_t* name; int w; } cols[] = {
            {L"文件名", 220}, {L"尺寸", 90}, {L"原体积", 80}, {L"预计输出", 90}, {L"状态", 90}, {L"输出体积", 90}, {L"", 36},
        };
        LVCOLUMNW lc = {};
        lc.mask = LVCF_TEXT | LVCF_WIDTH;
        for (int i = 0; i < 7; ++i) {
            lc.pszText = (LPWSTR)cols[i].name;
            lc.cx = S(cols[i].w);
            ListView_InsertColumn(g_list, i, &lc);
        }

        // 参数面板（自绘 STATIC + 消息转发）
        // SS_NOTIFY：让 STATIC 接收鼠标消息（点击预览卡弹大图、手型光标都依赖它）
        // WS_CLIPCHILDREN：面板重绘时不覆盖子控件区域，杜绝擦除顺序引发的控件消失/残影
        g_param_panel = CreateWindowW(L"STATIC", nullptr,
                                      WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY | WS_CLIPCHILDREN,
                                      0, 0, S(290), 600, hwnd, (HMENU)IDC_PARAM_PANEL, nullptr, nullptr);
        g_panel_oldproc = (WNDPROC)SetWindowLongPtrW(g_param_panel, GWLP_WNDPROC, (LONG_PTR)panel_proc);

        // 参数区滚动条（SCROLLBAR 子控件；参数不溢出时由 layout_param_scroll 隐藏）
        g_param_scrollbar = CreateWindowW(L"SCROLLBAR", nullptr,
                                          WS_CHILD | SBS_VERT | WS_VISIBLE,
                                          0, 0, S(16), 100, g_param_panel,
                                          (HMENU)IDC_PARAM_SCROLL, nullptr, nullptr);

        // 开始处理（面板子控件，紫色自绘）：仅处理选中 + 批量全部，两枚纵排
        g_run_sel_btn = CreateWindowW(L"BUTTON", L"仅处理选中图片",
                                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                      0, 0, S(258), S(38), g_param_panel, (HMENU)IDC_BTN_RUN_SEL, nullptr, nullptr);
        SendMessageW(g_run_sel_btn, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
        g_run_btn = CreateWindowW(L"BUTTON", L"批量处理全部图片",
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  0, 0, S(258), S(38), g_param_panel, (HMENU)IDC_BTN_RUN, nullptr, nullptr);
        SendMessageW(g_run_btn, WM_SETFONT, (WPARAM)g_font_bold, TRUE);

        // 进度条 + 状态
        g_progress = CreateWindowW(PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                   0, 0, 600, 18, hwnd, (HMENU)IDC_PROGRESS, nullptr, nullptr);
        SendMessageW(g_progress, PBM_SETBARCOLOR, 0, (LPARAM)C_PURPLE);
        SendMessageW(g_progress, PBM_SETBKCOLOR, 0, (LPARAM)C_CARD_BG);
        g_status = CreateWindowW(L"STATIC", L"就绪 · 拖入图片或文件夹开始",
                                 WS_CHILD | WS_VISIBLE, 0, 0, 600, 20, hwnd, (HMENU)IDC_STATUS, nullptr, nullptr);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font_small, TRUE);

        DragAcceptFiles(hwnd, TRUE);
        rebuild_param_panel();
        layout_controls(hwnd);
        return 0;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) return 0; // 最小化：尺寸无意义，跳过整轮布局（此前会以 0x0 布局一轮）
        if ((int)LOWORD(lp) == g_last_cx && (int)HIWORD(lp) == g_last_cy && g_last_cx)
            return 0; // 尺寸未变的重复 WM_SIZE：跳过
        layout_controls(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE); // 全量重绘背景区，杜绝拉伸残影
        // 不再调用 UpdateWindow：拖拽时每像素同步整窗重绘是卡顿主因，
        // 改为靠消息队列合并重绘（多次 WM_SIZE → 一次 WM_PAINT，视觉无差异）
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lp;
        // 最小尺寸按"最小客户区"反推窗口外框（ptMinTrackSize 含非客户区）：
        //   宽 S(1280)；高 = 侧栏完整内容 S(650)（15 功能行+5 分组标题行，各 S(30)，
        //   +5 间隔行各 S(10)）+ 品牌卡带 S(66) + 底部状态栏 S(46) = S(762)。
        //   保证最小高度时左侧功能列表完整显示、不出现滚动条。
        // 非客户区高度按当前窗口样式实测；创建前样式未定按 WS_OVERLAPPEDWINDOW 估算
        DWORD st = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (!st) st = WS_OVERLAPPEDWINDOW;
        mmi->ptMinTrackSize.x = S(1280);
        mmi->ptMinTrackSize.y = S(762) + nc_frame_cy(st, g_dpi);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // 自绘双缓冲，禁默认擦除防闪烁
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        // 双缓冲位图缓存：拖拽/滚动时高频重绘，不再每次新建销毁整窗位图（仅尺寸变化时重建）
        static HDC s_mem = nullptr;
        static HBITMAP s_bmp = nullptr;
        static int s_w = 0, s_h = 0;
        if (!s_mem) s_mem = CreateCompatibleDC(dc);
        if (!s_bmp || s_w != rc.right || s_h != rc.bottom) {
            if (s_bmp) DeleteObject(s_bmp);
            s_bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            s_w = rc.right;
            s_h = rc.bottom;
        }
        HBITMAP ob = (HBITMAP)SelectObject(s_mem, s_bmp);
        paint_workspace(s_mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, s_mem, 0, 0, SRCCOPY);
        SelectObject(s_mem, ob);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        auto* dis = (DRAWITEMSTRUCT*)lp;
        if (dis->hwndItem == g_sidebar) { draw_sidebar_item(dis); return TRUE; }
        if (dis->CtlID == IDC_BTN_ADD || dis->CtlID == IDC_BTN_ADDDIR || dis->CtlID == IDC_BTN_CLEAR) { draw_tool_btn(dis); return TRUE; }
        if (dis->CtlID == IDC_BTN_RUN || dis->CtlID == IDC_BTN_RUN_SEL) { draw_run_btn(dis); return TRUE; }
        if (dis->CtlID >= 40 && dis->CtlID <= 47) {
            // 水印颜色色块（BS_OWNERDRAW）：色块本体 + 选中深色描边
            HDC dc = dis->hDC;
            RECT rc = dis->rcItem;
            WmColor wc = wm_color_by_index(dis->CtlID - 40);
            fill_rect(dc, rc, C_WHITE);
            bool sel = ((int)dis->CtlID - 40 == g_wm_color_sel);
            RECT chip = rc;
            if (sel) {
                InflateRect(&chip, -S(3), -S(3)); // 选中：留白给描边
                fill_round(dc, chip, RGB(wc.r, wc.g, wc.b), S(4));
                HPEN pen = CreatePen(PS_SOLID, S(2), C_PURPLE);
                HPEN op = (HPEN)SelectObject(dc, pen);
                HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
                RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, S(8), S(8));
                SelectObject(dc, op); SelectObject(dc, ob);
                DeleteObject(pen);
            } else {
                fill_round(dc, chip, RGB(wc.r, wc.g, wc.b), S(4));
                HPEN pen = CreatePen(PS_SOLID, 1, C_CARD_BD);
                HPEN op = (HPEN)SelectObject(dc, pen);
                HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
                RoundRect(dc, chip.left, chip.top, chip.right, chip.bottom, S(4), S(4));
                SelectObject(dc, op); SelectObject(dc, ob);
                DeleteObject(pen);
            }
            return TRUE;
        }
        if (dis->hwndItem == g_param_panel) { draw_panel(dis); return TRUE; }
        break;
    }
    case WM_MEASUREITEM: {
        auto* mis = (MEASUREITEMSTRUCT*)lp;
        if (mis->CtlID == IDC_SIDEBAR) {
            mis->itemHeight = (int)mis->itemData == SB_SPACER ? S(10) : S(30);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_TEXT2);
        if (GetParent(ctl) == g_param_panel && GetDlgCtrlID(ctl) == 35)
            return (LRESULT)(INT_PTR)g_brush_sep; // 参数面板分组横线
        if (ctl == g_param_panel) return (LRESULT)(INT_PTR)g_brush_panel; // 面板自身擦除用面板色，防白闪
        if (GetParent(ctl) == g_param_panel) return (LRESULT)(INT_PTR)g_brush_white; // 参数控件在白色卡片内
        return (LRESULT)(INT_PTR)g_brush_white;
    }
    case WM_CTLCOLORBTN:
        return (LRESULT)(INT_PTR)g_brush_white;
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)(INT_PTR)g_brush_side;
    case WM_NOTIFY: {
        auto nm = (LPNMHDR)lp;
        if (nm->idFrom == IDC_LIST && nm->code == NM_CUSTOMDRAW) {
            auto lvd = (LPNMLVCUSTOMDRAW)lp;
            DWORD stage = lvd->nmcd.dwDrawStage;
            if (stage == CDDS_PREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
            // 行级 ITEMPREPAINT：
            // 列表模式 → 开启逐子项通知（状态列文字色）+ 行级 POSTPAINT；
            // 图标视图 → 只请求行级 POSTPAINT。图标视图没有子项（微软文档明确），
            // 返回 NOTIFYSUBITEMDRAW 会让 comctl32 走子项绘制序列，触发异常的重复绘制（✕ 成对出现）
            if (stage == CDDS_ITEMPREPAINT)
                return g_view_mode == VM_LIST ? (CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT)
                                              : CDRF_NOTIFYPOSTPAINT;
            if (stage == (CDDS_SUBITEM | CDDS_ITEMPREPAINT)) {
                int row = (int)lvd->nmcd.dwItemSpec;
                if (row < 0 || row >= (int)g_files.size()) return CDRF_DODEFAULT;
                // 0=格式徽章、6=移除✕：两列在 POSTPAINT 叠画，需各自订阅 NOTIFYPOSTPAINT
                if (lvd->iSubItem == 0) {
                    lvd->rcText.left += S(26); // 文件名右移，给格式徽章让位
                    return CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT;
                }
                if (lvd->iSubItem == 6)
                    return CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT;
                if (lvd->iSubItem == 4) {
                    lvd->clrText = state_color(g_files[row].state); // 状态列：纯文字（状态色）
                    return CDRF_NEWFONT;
                }
                lvd->clrText = C_TEXT;
                return CDRF_NEWFONT;
            }
            if (stage == (CDDS_SUBITEM | CDDS_ITEMPOSTPAINT)) {
                int row = (int)lvd->nmcd.dwItemSpec;
                if (row < 0 || row >= (int)g_files.size()) return CDRF_DODEFAULT;
                HDC dc = lvd->nmcd.hdc;
                RECT rc = lvd->nmcd.rc;
                if (lvd->iSubItem == 6 && g_view_mode == VM_LIST) {
                    // 移除 ✕（列表模式第 7 列）：居中 S(22) 按钮块（与命中区同一来源），悬停凸显
                    RECT xb;
                    if (x_rect_for_row(g_list, row, &xb))
                        draw_x_button(dc, xb, row == g_x_hover_row);
                }
                return CDRF_DODEFAULT;
            }
            if (stage == CDDS_ITEMPOSTPAINT) {
                // 缩略图模式：在图标右上角画移除 ✕（悬停凸显）
                if (g_view_mode != VM_LIST) {
                    int row = (int)lvd->nmcd.dwItemSpec;
                    HDC dcp = lvd->nmcd.hdc;
                    RECT xb;
                    if (row >= 0 && row < (int)g_files.size() && x_rect_for_row(g_list, row, &xb))
                        draw_x_button(dcp, xb, row == g_x_hover_row);
                }
                return CDRF_DODEFAULT;
            }
            return CDRF_DODEFAULT;
        }
        // 选中变化 → 刷新结果预览（仅状态位变化时，文本更新不触发）
        if (nm->idFrom == IDC_LIST && nm->code == LVN_ITEMCHANGED) {
            auto nmlv = (LPNMLISTVIEW)lp;
            if (nmlv->uChanged & LVIF_STATE) refresh_preview();
            return 0;
        }
        // Del 键移除选中项（列表有焦点时键盘事件走 LVN_KEYDOWN）
        if (nm->idFrom == IDC_LIST && nm->code == LVN_KEYDOWN) {
            auto kd = (LPNMLVKEYDOWN)lp;
            if (kd->wVKey == VK_DELETE && !g_files.empty() && !g_pipe.running() && !g_stitch_running)
                remove_selected_files();
            return 0;
        }
        // 点击 ✕ 移除：列表模式=第7列；缩略图模式=缩略图右上角
        if (nm->idFrom == IDC_LIST && nm->code == NM_CLICK) {
            auto nmv = (LPNMITEMACTIVATE)lp;
            if (nmv->iItem >= 0 && nmv->iItem < (int)g_files.size() && !g_pipe.running() && !g_stitch_running) {
                bool hit_x = false;
                // 点击命中：严格限制在 ✕ 按钮方块内（列表/缩略图统一走 x_rect_for_row）
                RECT xb;
                hit_x = x_rect_for_row(g_list, nmv->iItem, &xb) && PtInRect(&xb, nmv->ptAction) != FALSE;
                if (hit_x) {
                    int row = nmv->iItem;
                    g_files.erase(g_files.begin() + row);
                    ListView_DeleteItem(g_list, row);
                    if (g_view_mode != VM_LIST) rebuild_thumbs();
                    update_est_column();
                    refresh_preview();
                    InvalidateRect(g_hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        // 双击文件（列表模式行 / 缩略图模式图标）→ 打开原图放大预览弹窗
        if (nm->idFrom == IDC_LIST && nm->code == NM_DBLCLK) {
            auto nmv = (LPNMITEMACTIVATE)lp;
            if (nmv->iItem >= 0 && nmv->iItem < (int)g_files.size()) {
                if (!(ListView_GetItemState(g_list, nmv->iItem, LVIS_SELECTED) & LVIS_SELECTED)) {
                    ListView_SetItemState(g_list, (DWORD)-1, 0, LVIS_SELECTED);
                    ListView_SetItemState(g_list, nmv->iItem, LVIS_SELECTED, LVIS_SELECTED);
                }
                refresh_preview(); // 同步设置 g_pv.item（路径），原图预览只依赖它
                open_preview_popup(false);
            }
            return 0;
        }
        // 右键文件：仅处理该文件 / 移出列表（多选时作用于所有选中行）
        if (nm->idFrom == IDC_LIST && nm->code == NM_RCLICK) {
            auto nmv = (LPNMITEMACTIVATE)lp;
            if (nmv->iItem < 0 || g_pipe.running() || g_stitch_running) return 0; // 空白处 / 处理中不弹菜单
            if (!(ListView_GetItemState(g_list, nmv->iItem, LVIS_SELECTED) & LVIS_SELECTED)) {
                // 右击了未选中的行：把选择收敛到该行
                ListView_SetItemState(g_list, (DWORD)-1, 0, LVIS_SELECTED);
                ListView_SetItemState(g_list, nmv->iItem, LVIS_SELECTED, LVIS_SELECTED);
            }
            auto sel = selected_indices();
            HMENU menu = CreatePopupMenu();
            wchar_t t1[64];
            if (sel.size() > 1) swprintf(t1, 64, L"仅处理所选文件（%d 张）", (int)sel.size());
            else wcscpy_s(t1, L"仅处理该文件");
            AppendMenuW(menu, MF_STRING, 1, t1);
            AppendMenuW(menu, MF_STRING, 2, sel.size() > 1 ? L"移出列表（所选）" : L"移出列表");
            AppendMenuW(menu, MF_STRING, 3, L"打开图片所在位置");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 4, L"处理后输出到位置...");
            { // 每个菜单项前显示程序图标
                static const UINT item_ids[] = {1, 2, 3, 4};
                attach_menu_icon(menu, item_ids, 4);
            }
            POINT pt = nmv->ptAction;
            ClientToScreen(g_list, &pt);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) start_processing(sel);
            else if (cmd == 2) remove_selected_files();
            else if (cmd == 3) {
                // 资源管理器定位到文件（多选时定位第一个选中项）
                if (!sel.empty()) {
                    std::wstring p = g_files[sel[0]].path;
                    std::wstring args = L"/select,\"" + p + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                }
            }
            else if (cmd == 4) {
                // 选择输出目录，处理后结果写入该目录（文件名不变，同名覆盖）
                wchar_t dir[MAX_PATH] = {0};
                BROWSEINFOW bi = {};
                bi.hwndOwner = g_hwnd;
                bi.lpszTitle = L"选择处理后的输出位置";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                PIDLIST_ABSOLUTE rl = SHBrowseForFolderW(&bi);
                if (rl) {
                    if (SHGetPathFromIDListW(rl, dir)) start_processing(sel, dir);
                    CoTaskMemFree(rl);
                }
            }
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        // 光标悬停在侧边栏 → 自绘滚动条滚动
        DWORD ppos0 = GetMessagePos();
        POINT pt0 = {(short)LOWORD(ppos0), (short)HIWORD(ppos0)};
        HWND under0 = WindowFromPoint(pt0);
        if (under0 == g_sidebar || under0 == hwnd) {
            ScreenToClient(hwnd, &pt0);
            RECT s = side_sb_strip();
            if (pt0.x < s.right) { side_wheel_scroll((short)HIWORD(wp)); return 0; }
        }
        // 光标悬停在参数面板（或其子控件）上 → 参数区滚动
        DWORD ppos = GetMessagePos();
        POINT pt = {(short)LOWORD(ppos), (short)HIWORD(ppos)};
        HWND under = WindowFromPoint(pt);
        if (under == g_param_panel || IsChild(g_param_panel, under)) {
            if (param_wheel((short)HIWORD(wp))) return 0;
        }
        break;
    }
    case WM_DROPFILES: {
        dbg_log(L"WM_DROPFILES enter");
        HDROP drop = (HDROP)wp;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH] = {};
            DragQueryFileW(drop, i, path, MAX_PATH);
            DWORD attr = GetFileAttributesW(path);
            if (attr & FILE_ATTRIBUTE_DIRECTORY) add_directory_recursive(path);
            else add_file_path(path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        // 侧边栏滚动条命中：滑块=开始拖拽，轨道=整页翻滚（严格限制在条带内，
        // 否则条带右侧整个中间区的点击都会被吞掉——切换条按钮失效的根因）
        RECT sstrip = side_sb_strip();
        if (pt.x >= sstrip.left && pt.x <= sstrip.right && pt.y < sstrip.bottom) {
            RECT track, thumb;
            if (side_thumb_geom(&track, &thumb)) {
                if (pt.y < thumb.top) { // 上翻一页
                    RECT s = side_sb_strip();
                    int off = 0;
                    for (int i = 0, top = ListBox_GetTopIndex(g_sidebar); i < top; ++i) off += side_item_h(i);
                    side_scroll_to_offset(std::max(0, off - (int)(s.bottom - s.top)));
                } else if (pt.y > thumb.bottom) { // 下翻一页
                    RECT s = side_sb_strip();
                    int off = 0;
                    for (int i = 0, top = ListBox_GetTopIndex(g_sidebar); i < top; ++i) off += side_item_h(i);
                    side_scroll_to_offset(off + (s.bottom - s.top));
                } else { // 抓住滑块拖拽
                    g_sb_drag = true;
                    g_sb_grab = pt.y - thumb.top;
                    SetCapture(hwnd);
                }
                return 0;
            }
            // 内容不超高（无滑块）：不拦截，落入后续命中逻辑
        }
        // 表头列边界命中：开始拖拽调整列宽（捕获鼠标，实时改列宽）
        RECT crc0;
        GetClientRect(hwnd, &crc0);
        int hcol = header_hit_col(pt, crc0.right);
        if (hcol >= 0) {
            g_col_drag = hcol;
            g_col_drag_x0 = pt.x;
            g_col_drag_w0 = g_cw[hcol];
            SetCapture(hwnd);
            return 0;
        }
        // 显示方式切换条命中（列表/缩略图 小·中·大）
        RECT crc;
        GetClientRect(hwnd, &crc);
        int seg = hit_view_seg(crc, pt);
        if (seg >= 0 && seg != g_view_mode) set_view_mode(seg);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT crc;
        GetClientRect(hwnd, &crc);
        if (g_sb_drag) { // 滑块拖拽：像素偏移反推 top 索引
            RECT track, thumb;
            if (side_thumb_geom(&track, &thumb)) {
                int tl = track.bottom - track.top, th = thumb.bottom - thumb.top;
                int content = side_content_h(), view = side_view_h();
                int off = (int)((double)(pt.y - g_sb_grab - track.top) * (content - view) / std::max(1, tl - th) + 0.5);
                side_scroll_to_offset(std::max(0, std::min(off, content - view)));
            }
            return 0;
        }
        if (g_col_drag >= 0) { // 拖拽中：列宽 = 起始宽 + 鼠标位移（不低于最小宽）
            apply_col_width(g_col_drag, g_col_drag_w0 + (pt.x - g_col_drag_x0));
            return 0;
        }
        if (header_hit_col(pt, crc.right) >= 0)
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_sb_drag) {
            g_sb_drag = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            side_invalidate();
            return 0;
        }
        if (g_col_drag >= 0) {
            g_col_drag = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED: // 捕获被抢走（如弹出对话框）：强制结束拖拽
        g_sb_drag = false;
        g_col_drag = -1;
        return 0;
    case WM_SETCURSOR: {
        // 表头列边界显示左右调整光标（拖拽中保持）
        if (LOWORD(lp) == HTCLIENT && g_view_mode == VM_LIST) {
            DWORD ppos = GetMessagePos();
            POINT pt = {(short)LOWORD(ppos), (short)HIWORD(ppos)};
            ScreenToClient(hwnd, &pt);
            RECT crc;
            GetClientRect(hwnd, &crc);
            if (g_col_drag >= 0 || header_hit_col(pt, crc.right) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_BTN_CLEAR) dbg_log(L"CMD clear");
        if (id == IDC_BTN_RUN) dbg_log(L"CMD run");
        if (id == IDC_BTN_ADD) {
            wchar_t file_buf[8192] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof ofn;
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"图片\0*.jpg;*.jpeg;*.png;*.gif;*.bmp;*.webp;*.tif;*.tiff;*.pdf;*.heic;*.heif\0所有文件\0*.*\0";
            ofn.lpstrFile = file_buf;
            ofn.nMaxFile = 8192;
            ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                wchar_t* p = file_buf;
                std::wstring dir = p;
                p += wcslen(p) + 1;
                if (*p) {
                    while (*p) { add_file_path(std::wstring(dir) + L"\\" + p); p += wcslen(p) + 1; }
                } else {
                    add_file_path(dir);
                }
            }
        } else if (id == IDC_BTN_ADDDIR) {
            wchar_t dir[MAX_PATH] = {};
            BROWSEINFOW bi = {};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = L"选择图片文件夹";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                SHGetPathFromIDListW(pidl, dir);
                CoTaskMemFree(pidl);
                add_directory_recursive(dir);
            }
        } else if (id == IDC_BTN_CLEAR) {
            if (!g_pipe.running() && !g_stitch_running) clear_files();
        } else if (id == IDC_BTN_RUN) {
            start_processing({});
        } else if (id == IDC_BTN_RUN_SEL) {
            auto sel = selected_indices();
            if (sel.empty()) {
                MessageBoxW(g_hwnd, L"请先在文件列表中选中要处理的图片", L"SimplePic", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            start_processing(sel);
        } else if (id == IDC_SIDEBAR && HIWORD(wp) == LBN_SELCHANGE) {
            int sel = (int)ListBox_GetCurSel(g_sidebar);
            int mi = (int)ListBox_GetItemData(g_sidebar, sel);
            if (mi < 0) { // 分组标题/间隔行不可选，恢复上次有效选中
                ListBox_SetCurSel(g_sidebar, g_sidebar_sel);
                InvalidateRect(g_sidebar, nullptr, FALSE);
                side_invalidate(); // SetCurSel 可能滚动，同步滚动条滑块
                return 0;
            }
            if (mi < (int)(sizeof kModules / sizeof kModules[0])) {
                g_sidebar_sel = sel;
                g_module = kModules[mi].id;
                rebuild_param_panel();
                update_est_column();
                refresh_preview();      // 切模块：实时预览同步换算子
                InvalidateRect(g_param_panel, nullptr, TRUE);
                InvalidateRect(hwnd, nullptr, FALSE); // 刷新模块头部/统计卡
            }
        } else if (id == 529) { // 水印图片选择按钮
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"图片\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                g_wm_img_path = file;
                for (auto& c : g_ctls)
                    if (c.type == CTL_LABEL && c.module == 29)
                        SetWindowTextW(c.hwnd, g_wm_img_path.c_str());
                update_est_column();
                refresh_preview();
            }
        } else if (HIWORD(wp) == CBN_SELCHANGE || HIWORD(wp) == EN_CHANGE ||
                   (HIWORD(wp) == BN_CLICKED && id < 100)) {
            // 参数面板控件变动：下拉(id=base+500)/输入框(id=base+500)/勾选框(id=base)
            int base = id - 500;
            if (HIWORD(wp) == CBN_SELCHANGE && base == 1) update_idphoto_custom_state();
            if (HIWORD(wp) == CBN_SELCHANGE && base == 6) update_crop_custom_state();
            if (HIWORD(wp) == CBN_SELCHANGE && base == 7) update_pdfmerge_state();
            if (HIWORD(wp) == CBN_SELCHANGE && base == 2) update_target_custom_state();
            if (HIWORD(wp) == EN_CHANGE && base == 16) { // 自定义体积 <1KB 自动退回 1KB（空框不干预，避免打断输入）
                for (auto& e : g_ctls)
                    if (e.type == CTL_EDIT && e.module == 16) {
                        wchar_t b2[64] = {};
                        GetWindowTextW(e.hwnd, b2, 64);
                        if (b2[0] && _wtoi(b2) < 1) SetWindowTextW(e.hwnd, L"1");
                    }
            }
            // 水印颜色色块：点击选择（id=40..47），刷新所有色块
            if (HIWORD(wp) == BN_CLICKED && id >= 40 && id <= 47) {
                g_wm_color_sel = id - 40;
                for (auto& c : g_ctls)
                    if (c.type == CTL_COLORS && c.module >= 40)
                        InvalidateRect(c.hwnd, nullptr, TRUE);
            }
            // 右键菜单设置：勾选即时写入/移除注册表（勾选框 HMENU = base，无 +500 偏移）
            if (HIWORD(wp) == BN_CLICKED && (id == 32 || id == 33)) {
                for (auto& c : g_ctls)
                    if (c.type == CTL_CHECK && (c.module == 32 || c.module == 33))
                        set_shell_menu(c.module == 33,
                                       SendMessageW(c.hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
            }
            update_est_column();
            if (g_module == MOD_COMPRESS_LIVE) update_live_label();
            refresh_preview();          // 参数变动：实时预览即时预测
            InvalidateRect(hwnd, nullptr, FALSE); // 统计卡"预计输出"同步
        }
        return 0;
    }
    case WM_HSCROLL: {
        // 智能压缩质量滑块：拖动即时刷新读数/估算/预览
        if ((HWND)lp) {
            for (auto& c : g_ctls)
                if (c.type == CTL_SLIDER && c.hwnd == (HWND)lp) {
                    update_est_column();
                    update_live_label();
                    refresh_preview();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
        }
        return 0;
    }
    case WM_SP_ITEM: {
        auto* pair = (std::pair<int, ItemResult>*)lp;
        int idx = pair->first;
        ItemState st = (ItemState)wp;
        const ItemResult& res = pair->second;
        if (idx >= 0 && idx < (int)g_files.size()) {
            g_files[idx].state = st;
            const wchar_t* s = L"等待中";
            if (st == ItemState::Running) s = L"处理中…";
            else if (st == ItemState::Done) s = L"已完成";
            else if (st == ItemState::Failed) s = L"失败";
            ListView_SetItemText(g_list, idx, 4, (LPWSTR)s);
            if (st == ItemState::Done || st == ItemState::Failed) {
                wchar_t buf[96];
                if (st == ItemState::Done && res.out_bytes > 0) {
                    fmt_size(res.out_bytes, buf, 96);
                    ListView_SetItemText(g_list, idx, 5, buf);
                } else if (st == ItemState::Failed) {
                    wcsncpy_s(buf, res.error.c_str(), 30);
                    ListView_SetItemText(g_list, idx, 5, buf);
                }
            }
        }
        // 进度按本次提交集合计数（子集处理时不被上次残留状态干扰）
        int done = 0;
        for (int i : g_active_idx)
            if (i < (int)g_files.size() &&
                (g_files[i].state == ItemState::Done || g_files[i].state == ItemState::Failed)) ++done;
        SendMessageW(g_progress, PBM_SETPOS, done, 0);
        if (idx == -1) { // 完成哨兵：管线全部 worker 收尾
            // 多图转 PDF 合并：worker 已全部收尾 → 分片齐备，合成单个 PDF
            if (g_pdf_merge_pending) {
                g_pdf_merge_pending = false;
                long long bytes = sp::pdf_merge_finish();
                if (bytes >= 0) {
                    for (int i : g_active_idx)
                        if (i < (int)g_files.size()) {
                            g_files[i].state = ItemState::Done;
                            g_files[i].result.ok = true;
                            g_files[i].result.out_path = g_pdf_merge_dst;
                            g_files[i].result.out_bytes = bytes;
                            ListView_SetItemText(g_list, i, 4, (LPWSTR)L"已合并");
                            wchar_t b[64];
                            fmt_size(bytes, b, 64);
                            ListView_SetItemText(g_list, i, 5, b);
                        }
                }
            }
            int ok = 0, fail = 0;
            for (int i : g_active_idx)
                if (i < (int)g_files.size()) (g_files[i].state == ItemState::Done) ? ++ok : ++fail;
            wchar_t buf[160];
            if (!g_pdf_merge_dst.empty() && ok > 0)
                swprintf(buf, 160, L"完成：成功 %d · 失败 %d → %s", ok, fail, g_pdf_merge_dst.c_str());
            else
                swprintf(buf, 160, L"完成：成功 %d · 失败 %d", ok, fail);
            SetWindowTextW(g_status, buf);
            g_pdf_merge_dst.clear();
            // 自动预览最后一张已处理的文件（选中变化 → LVN_ITEMCHANGED → refresh_preview）
            if (!g_active_idx.empty()) {
                int last = g_active_idx.back();
                if (last >= 0 && last < (int)g_files.size()) {
                    ListView_SetItemState(g_list, (DWORD)-1, 0, LVIS_SELECTED);
                    ListView_SetItemState(g_list, last, LVIS_SELECTED, LVIS_SELECTED);
                }
            }
            g_active_idx.clear();
        }
        delete pair;
        return 0;
    }
    case WM_SP_STITCH_DONE: {
        auto* res = (ItemResult*)lp;
        g_stitch_running = false;
        if (res->ok) {
            for (int i : g_active_idx)
                if (i < (int)g_files.size()) {
                    g_files[i].state = ItemState::Done;
                    ListView_SetItemText(g_list, i, 4, (LPWSTR)L"已完成");
                }
            int last = g_active_idx.empty() ? -1 : g_active_idx.back();
            if (last >= 0 && last < (int)g_files.size()) {
                g_files[last].result = *res; // 输出指向长图，预览区可显示成品
                wchar_t b[64];
                fmt_size(res->out_bytes, b, 64);
                ListView_SetItemText(g_list, last, 5, b);
                ListView_SetItemState(g_list, (DWORD)-1, 0, LVIS_SELECTED);
                ListView_SetItemState(g_list, last, LVIS_SELECTED, LVIS_SELECTED);
            }
            SetWindowTextW(g_status, (L"拼接完成 → " + res->out_path).c_str());
        } else {
            for (int i : g_active_idx)
                if (i < (int)g_files.size()) {
                    g_files[i].state = ItemState::Failed;
                    ListView_SetItemText(g_list, i, 4, (LPWSTR)L"失败");
                }
            SetWindowTextW(g_status, (L"拼接失败：" + res->error).c_str());
        }
        g_active_idx.clear();
        delete res;
        return 0;
    }
    case WM_SP_PREVIEW_DECODED: {
        // 后台解码完成：入 LRU 缓存；若仍是当前选中文件则填充预览并重跑算子（代价极低）
        auto* r = (PreviewDecodeResult*)lp;
        {
            std::lock_guard<std::mutex> lk(g_pv_inflight_mu);
            g_pv_inflight.erase(r->path);
        }
        preview_cache_put(r->path, r->img, r->ok); // 失败结果也缓存，避免反复重试同一坏文件
        int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
        if (sel >= 0 && sel == g_pv.item && sel < (int)g_files.size() && g_pv.src_path == r->path) {
            g_pv.src = r->img;
            g_pv.src_ok = r->ok;
            g_pv_decoding = false;
            refresh_preview();
        }
        delete r;
        return 0;
    }
    case WM_SP_LIVE_EST: {
        // 智能压缩逐文件实测完成：入缓存（失败也缓存防重试），刷新该行与统计卡
        auto* r = (LiveEstResult*)lp;
        long long kb = -1;
        bool found = false;
        for (auto& f : g_files) {
            if (f.path != r->path || f.w <= 0 || f.h <= 0) continue;
            found = true;
            if (r->ok && r->w720 > 0 && r->h720 > 0) {
                double bpp = (double)r->bytes720 / ((double)r->w720 * r->h720);
                kb = std::max<long long>(1, (long long)((double)f.w * f.h * bpp / 1024.0));
            }
            break;
        }
        if (found) g_est_cache[r->path] = { r->q, kb }; // kb<0 表示实测失败（同质量不再重试）
        else g_est_cache.erase(r->path);                 // 文件已移除：不留脏缓存
        {
            std::lock_guard<std::mutex> lk(g_est_mu);
            g_est_inflight.erase(r->path);
        }
        int idx = -1;
        for (int i = 0; i < (int)g_files.size(); ++i)
            if (g_files[i].path == r->path) { idx = i; break; }
        delete r;
        if (idx >= 0) {
            update_est_column();                    // 重刷整列（新值可能已到）
            InvalidateRect(hwnd, nullptr, FALSE);   // 统计卡"预计输出"同步
            if (g_module == MOD_COMPRESS_LIVE) update_live_label();
        }
        return 0;
    }
    case WM_SP_THUMB_READY: {
        // 缩略图异步解码完成：入缓存；世代/视图/尺寸仍匹配时按索引替换 ImageList
        auto r = (ThumbResult*)lp;
        if (r) {
            HBITMAP hb = dib_from_image(r->img);
            if (hb) {
                if (g_thumb_cache.size() > 500) g_thumb_cache.clear(); // 粗略上限，防会话级缓存无限膨胀
                g_thumb_cache[r->path][r->sz] = hb;
                if ((int)(INT_PTR)wp == g_thumb_gen.load() && g_view_mode != VM_LIST
                    && g_imglist && thumb_px_size() == r->sz) {
                    int idx = -1;
                    for (size_t i = 0; i < g_files.size(); ++i)
                        if (g_files[i].path == r->path) { idx = (int)i; break; }
                    if (idx >= 0 && idx < (int)g_files.size()
                        && idx < ListView_GetItemCount(g_list)) {
                        ImageList_Replace(g_imglist, idx, hb, nullptr);
                        RECT ir;
                        if (ListView_GetItemRect(g_list, idx, &ir, LVIR_BOUNDS))
                            InvalidateRect(g_list, &ir, TRUE); // 仅刷新该格，擦除白块占位
                    }
                }
            }
            delete r;
        }
        return 0;
    }
    case WM_DESTROY: {
        dbg_log(L"WM_DESTROY");
        thumb_shutdown();                      // 停掉缩略图解码线程
        if (g_pv_pop) DestroyWindow(g_pv_pop); // 主窗口关闭时顺带关闭大图预览弹窗
        if (g_imglist) { ImageList_Destroy(g_imglist); g_imglist = nullptr; }
        g_pipe.cancel();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND create_main_window(HINSTANCE hInst) {
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    migrate_shell_menu(); // 旧版全文件类型右键注册 → 按扩展名 + 图标，启动即自愈

    // Per-Monitor DPI 感知：避免系统缩放把布局拉变形
    using FnSetCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        auto set_ctx = (FnSetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (set_ctx && !set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"SimplePicMain";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1)); // src/app.rc：1 ICON app.ico
    // g_dpi 必须在创建窗口【之前】就绪：系统在 WM_CREATE 前就发第一条
    // WM_GETMINMAXINFO，若此时 g_dpi 还是 96，之后变真实 DPI，
    // 最小尺寸会突变 → 拉伸时窗口"跳大"
    {
        using FnGetSysDpi = UINT(WINAPI*)();
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
            auto get_sys_dpi = (FnGetSysDpi)GetProcAddress(u32, "GetDpiForSystem");
            if (get_sys_dpi) g_dpi = (int)get_sys_dpi();
        }
        if (g_dpi <= 96) {
            HDC dc = GetDC(nullptr);
            g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(nullptr, dc);
        }
    }

    RegisterClassW(&wc);

    // 初始尺寸 = 最小尺寸（最小客户区 1280×762 + 非客户区外框，开窗即完整显示侧栏列表）；
    // 屏幕更小时按工作区收，避免开窗超屏。
    // 固定窗口：去掉 WS_THICKFRAME/WS_MAXIMIZEBOX → 不可拖拽调整大小、不可最大化
    // （拖拽改变尺寸触发布局重算，真实拖拽下多次出现按钮错位等布局 bug，故整体锁定尺寸）
    RECT wa0 = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa0, 0);
    const DWORD kMainStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    int w0 = std::min(S(1280), (int)(wa0.right - wa0.left));
    int h0 = std::min(S(762) + nc_frame_cy(kMainStyle, g_dpi),
                      (int)(wa0.bottom - wa0.top));

    // 不用 WS_EX_COMPOSITED：与 WS_CLIPCHILDREN / LVS_EX_DOUBLEBUFFER 叠加时
    // 合成缓冲中未绘制区域呈黑色（右侧黑面板根因）；主窗口已有自绘双缓冲，无需它
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SimplePic 图片处理系统 v1.0",
                                kMainStyle,
                                CW_USEDEFAULT, CW_USEDEFAULT, w0, h0,
                                nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // 命令行文件参数：SimplePic.exe a.jpg b.png ...（调试复现与批量打开两用）
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            DWORD attr = GetFileAttributesW(argv[i]);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                add_directory_recursive(argv[i]);
            else
                add_file_path(argv[i]);
        }
        LocalFree(argv);
    }
    return hwnd;
}

Params current_params() {
    Params p;
    collect_params(p);
    return p;
}

} // namespace sp
