#pragma once
// SimplePic 核心图像类型与编解码（stb 实现，后续 libjpeg-turbo 可替换实现层）
#include <cstdint>
#include <vector>
#include <string>

namespace sp {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px; // RGBA, 4ch, 行优先
    Image() = default;
    Image(int W, int H) : w(W), h(H), px((size_t)W * H * 4, 255) {}
    uint8_t* row(int y) { return px.data() + (size_t)y * w * 4; }
    const uint8_t* row(int y) const { return px.data() + (size_t)y * w * 4; }
    bool ok() const { return w > 0 && h > 0 && !px.empty(); }
};

enum class Fmt { JPG, PNG, BMP, WEBP, PDF, ICO, TIFF, GIF, TGA };

// 读取任意支持的格式（内部统一转 RGBA），失败返回 false
// PDF：Windows.Data.Pdf 整页渲染第 1 页（全格式支持），失败回退内嵌 DCTDecode 提取
bool load_image(const std::wstring& path, Image& out, std::string* err = nullptr);

// 内存解码（RGBA，仅 stb 支持的格式）：用于资源内嵌小图
bool load_image_from_memory(const uint8_t* data, size_t len, Image& out);

// 保存到文件（JPG/PNG/BMP/TIFF/GIF/TGA；WebP/PDF/ICO 由专用模块处理）
bool save_image(const std::wstring& path, const Image& img, Fmt fmt, int jpg_quality);

// 内存编码
std::vector<uint8_t> encode_jpg(const Image& img, int quality);
std::vector<uint8_t> encode_png(const Image& img);
std::vector<uint8_t> encode_bmp(const Image& img);
std::vector<uint8_t> encode_tiff(const Image& img); // 基线 TIFF：RGB 无压缩
std::vector<uint8_t> encode_gif(const Image& img);  // GIF89a：256 色量化 + LZW
std::vector<uint8_t> encode_tga(const Image& img);
std::vector<uint8_t> encode_webp(const Image& img, int quality); // libwebp 有损

bool read_file(const std::wstring& path, std::vector<uint8_t>& out);
bool write_file(const std::wstring& path, const std::vector<uint8_t>& data);

// 像素几何操作（供 ops 使用）
void resize_image(const Image& src, Image& dst);                 // 双三次/SRGB 高质量缩放
void rotate_image(const Image& src, Image& dst, int deg);        // 90 / 180 / 270
void flip_image(const Image& src, Image& dst, bool horizontal);  // 水平/垂直镜像

// 等比缩放 + 裁剪：输出恰好 tw x th（主体不变形）
// va=Center：垂直居中裁（通用）；va=Top：顶部对齐只裁底部；
// va=Split25：溢出部分顶部保 25%、底部裁 75%（证件照兼顾头顶留白与头位规范）
enum class VAnchor { Center, Top, Split25 };
Image fit_center_crop(const Image& src, int tw, int th, VAnchor va = VAnchor::Center);

// 获取图像尺寸（不解码像素，仅读头）
bool probe_image(const std::wstring& path, int& w, int& h);

} // namespace sp
