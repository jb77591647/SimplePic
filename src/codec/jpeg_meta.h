#pragma once
// JPEG 字节级元数据手术：剥离 / 填充 / DPI —— 不重编码、像素零改动
#include <cstdint>
#include <vector>
#include <string>

namespace sp {

struct SegmentInfo {
    std::string name;   // "EXIF" / "XMP" / "ICC" / "COM" / "JFIF" / "SOF0" / "SOS" ...
    size_t offset;      // 段起始（含 FFE0 标记）
    size_t size;        // 段总大小（含标记与长度字节）
    int marker;         // 0xFFE1 等
};

// 解析 JPEG 段结构；SOS 之后的熵编码数据视为一个段 "SCAN"
std::vector<SegmentInfo> jpeg_segments(const std::vector<uint8_t>& j);

// 剥离元数据：去掉全部 APPn（可保留 JFIF APP0）与 COM，保留 DQT/DHT/SOF/SOS/SCAN
std::vector<uint8_t> jpeg_strip(const std::vector<uint8_t>& j, bool keep_jfif = true);

// 填充 COM 注释段（随机字节），使文件增加 target_extra 字节；返回填充后的文件
std::vector<uint8_t> jpeg_fill_com(const std::vector<uint8_t>& j, size_t target_extra);

// 读取 JFIF APP0 中的 DPI（无则返回 0）
int jpeg_get_dpi(const std::vector<uint8_t>& j);

// 设置 DPI：重写/插入 JFIF APP0（units=1 英寸），像素尺寸不变
std::vector<uint8_t> jpeg_set_dpi(const std::vector<uint8_t>& j, int dpi);

// PNG tEXt 填充（功能 4 的 PNG 分支）
std::vector<uint8_t> png_fill_text(const std::vector<uint8_t>& p, size_t target_extra);
// PNG 剥离 tEXt/iTXt/zTXt 元数据
std::vector<uint8_t> png_strip_meta(const std::vector<uint8_t>& p);
// PNG 设置 DPI：写 pHYs chunk（像素/米），不改像素尺寸
std::vector<uint8_t> png_set_dpi(const std::vector<uint8_t>& p, int dpi);

} // namespace sp
