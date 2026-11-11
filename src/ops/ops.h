#pragma once
// 各功能模块的具体处理逻辑
#include <string>
#include "../codec/codec.h"
#include "../pipeline/pipeline.h"

namespace sp {

// 模块 ID（与 UI 侧栏一致）
enum ModuleId {
    MOD_ID_PHOTO = 1,
    MOD_COMPRESS_TARGET = 2,
    MOD_COMPRESS_LIVE = 3,
    MOD_FILL_SIZE = 4,
    MOD_SET_DPI = 5,
    MOD_CENTER_CROP = 6,
    MOD_FORMAT_CONVERT = 7,
    MOD_FLIP = 8,
    MOD_WATERMARK = 9,
    MOD_WATERMARK_IMG = 13, // 图片水印（独立模块，与文字水印完全分离）
    MOD_ROTATE = 14,        // 旋转（保持原格式输出，仅旋转角度）
    MOD_STITCH = 10,
    MOD_ICON_EXPORT = 11,
    MOD_STRIP_META = 12,
};

// 单文件处理入口（管线调用）；复用 pipeline.h 的 ItemResult，避免双类型转换
ItemResult run_op(const std::wstring& src, const Params& p, const std::wstring& out_dir);

// ---- 多图转 PDF 合并 ----
// 开始一次合并会话：ordered_paths 为 UI 列表顺序（决定 PDF 页序），dst 为合并输出路径。
// 会话激活后，格式转换算子遇到 PDF 输出时不再逐张写文件，改为登记 JPEG 分片。
void pdf_merge_begin(const std::vector<std::wstring>& ordered_paths, const std::wstring& dst);
// 完成哨兵时调用：把已登记分片按会话顺序合成单个多页 PDF 并写盘。
// 部分文件失败/取消时按已登记分片写出。返回输出字节数；无分片返回 -1（会话结束）。
long long pdf_merge_finish();

// ---- 细粒度 API（供智能压缩预览等复用）----

// 压缩到目标字节：二分质量；SSIM 简化为质量下限保护（阶段 1 用 quality 下限替代）
std::vector<uint8_t> compress_to_size(const Image& img, long long target_bytes,
                                      int min_q, int max_q, long long* final_size,
                                      int* final_q, bool* hit_target);

// 智能压缩计划：滑块质量 → (分辨率缩放比 k, 实际编码质量 eff_q)
// q>=40：k=1 纯质量调节；q<40：eff_q=40 + 等比降分辨率 k=q/40（下限 0.10）。
// 极端低质量量化表会产生大色块伪影且体积效率差，改为保质量降分辨率，
// 与"压缩到指定大小"的降分辨率策略对齐。k<1 时调用方需按 k 缩放后用 eff_q 编码。
void smart_compress_plan(int q, double& k, int& eff_q);

// ---- 文字水印 ----
// 用 GDI 将文字渲染为 8bit 覆盖遮罩（白色=笔画），支持旋转
bool render_text_mask(const std::wstring& text, int px_size, int angle_deg,
                      int& mw, int& mh, std::vector<uint8_t>& mask);
// 将遮罩按颜色/透明度混合到图像（layout: 0=平铺 1=居中 2=右下角）
void blend_watermark(Image& img, const std::vector<uint8_t>& mask, int mw, int mh,
                     int cr, int cg, int cb, int alpha_pct, int layout, int spacing);

// ---- 长图拼接（阻塞式，progress 回调返回 false 表示取消） ----
// mode: 0=纵向 1=横向 2=九宫格（3×3，取前 9 张，每张等比裁剪到统一格尺寸）
ItemResult stitch_files(const std::vector<std::wstring>& paths, const std::wstring& out_path,
                        int mode, int fmt /*0=PNG 1=JPG*/,
                        const std::function<bool(int done, int total)>& progress);

// ---- 图片水印：wm 按 scale%（相对宿主图宽）缩放、可旋转后混合
//      layout: 0=平铺 1=居中 2=右下角 3=左上角 4=右上角 5=左下角 ----
void blend_image_watermark(Image& img, const Image& wm,
                           int alpha_pct, int layout, int spacing, int scale_pct, int angle_deg);

// ---- 水印 16 色表（UI 下拉与 ops 共用索引） ----
struct WmColor { int r, g, b; };
WmColor wm_color_by_index(int idx);

} // namespace sp
