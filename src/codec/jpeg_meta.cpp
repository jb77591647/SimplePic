#include "jpeg_meta.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sp {

static bool is_marker_start(const std::vector<uint8_t>& j, size_t pos) {
    return pos + 3 < j.size() && j[pos] == 0xFF && j[pos + 1] != 0x00 && j[pos + 1] != 0xFF;
}

static std::string marker_name(int m, const uint8_t* body, size_t body_len) {
    char buf[16];
    switch (m) {
    case 0xE0: return "JFIF";
    case 0xE1: return body_len > 5 && memcmp(body, "Exif\0\0", 6) == 0 ? "EXIF" : "APP1"; // XMP 会以 http://ns.adobe.com 开头
    case 0xE2: return body_len > 11 && memcmp(body, "ICC_PROFILE", 11) == 0 ? "ICC" : "APP2";
    case 0xED: return "Photoshop";
    case 0xEE: return "Adobe";
    case 0xFE: return "COM";
    default:
        if (m >= 0xE0 && m <= 0xEF) { sprintf_s(buf, sizeof buf, "APP%d", m - 0xE0); return buf; }
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) return "SOF";
        if (m == 0xC4) return "DHT";
        if (m == 0xDB) return "DQT";
        if (m == 0xDD) return "DRI";
        if (m == 0xDA) return "SOS";
        if (m == 0xD9) return "EOI";
        sprintf_s(buf, sizeof buf, "FF%02X", m);
        return buf;
    }
}

std::vector<SegmentInfo> jpeg_segments(const std::vector<uint8_t>& j) {
    std::vector<SegmentInfo> out;
    if (j.size() < 4 || j[0] != 0xFF || j[1] != 0xD8) return out;
    SegmentInfo soi{"SOI", 0, 2, 0xD8};
    out.push_back(soi);
    size_t pos = 2;
    while (pos + 1 < j.size()) {                   // EOI 可能在文件最后 2 字节（pos+3 条件会漏掉它）
        if (j[pos] != 0xFF) break;
        int m = j[pos + 1];
        if (m == 0xFF) { ++pos; continue; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) { ++pos; continue; } // SOI/RST（RST 属扫描数据，异常出现时跳过）
        if (m == 0xD9) { out.push_back({"EOI", pos, j.size() - pos, m}); break; }
        if (m == 0xDA) { // SOS：头部 + 熵编码数据，直到 EOI
            if (pos + 4 > j.size()) break;
            size_t seg_start = pos;
            size_t len = ((size_t)j[pos + 2] << 8) | j[pos + 3];
            size_t p = pos + 2 + len;
            while (p + 1 < j.size()) {
                if (j[p] == 0xFF && j[p + 1] != 0x00 && j[p + 1] != 0xFF) {
                    if (j[p + 1] >= 0xD0 && j[p + 1] <= 0xD7) { p += 2; continue; } // RSTn：扫描数据的一部分，跳过
                    break;
                }
                ++p;
            }
            out.push_back({"SCAN", seg_start, p - seg_start, m});
            pos = p;
            continue;
        }
        if (pos + 4 > j.size()) break;
        size_t len = ((size_t)j[pos + 2] << 8) | j[pos + 3];
        if (len < 2) break;
        size_t seg_size = len + 2;
        if (pos + seg_size > j.size()) break;
        out.push_back({marker_name(m, j.data() + pos + 4, len - 2), pos, seg_size, m});
        pos += seg_size;
    }
    return out;
}

static bool keep_segment(int marker, bool keep_jfif) {
    if (marker == 0xE0) return keep_jfif;
    if (marker >= 0xE0 && marker <= 0xEF) return false; // APPn 全剥
    if (marker == 0xFE) return false;                    // COM 剥（COM 是我们的填充位）
    if (marker == 0xDA) return true;                     // SOS+SCAN
    return true;                                         // DQT/DHT/SOF/DRI 等
}

std::vector<uint8_t> jpeg_strip(const std::vector<uint8_t>& j, bool keep_jfif) {
    auto segs = jpeg_segments(j);
    std::vector<uint8_t> out;
    out.reserve(j.size());
    for (auto& s : segs) {
        int m = s.marker;
        bool is_app_or_com = (m >= 0xE0 && m <= 0xEF) || m == 0xFE;
        if (!is_app_or_com || (m == 0xE0 && keep_jfif)) {
            out.insert(out.end(), j.begin() + s.offset, j.begin() + s.offset + s.size);
        }
    }
    return out;
}

static void put_random(uint8_t* p, size_t n) {
    // 简单随机填充：无需密码学强度
    static uint32_t s = (uint32_t)(GetTickCount64() ^ 0x5EED2026u);
    for (size_t i = 0; i < n; ++i) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        p[i] = (uint8_t)(0x20 + (s % 0x5F)); // 可打印 ASCII，避免奇怪字符
    }
}

std::vector<uint8_t> jpeg_fill_com(const std::vector<uint8_t>& j, size_t target_extra) {
    std::vector<uint8_t> out;
    if (j.size() < 2 || target_extra == 0) return j;
    out.insert(out.end(), j.begin(), j.begin() + 2); // SOI
    while (target_extra > 0) {
        size_t payload = std::min<size_t>(target_extra, 65533);
        size_t seg_len = payload + 2;
        out.push_back(0xFF); out.push_back(0xFE);
        out.push_back((uint8_t)(seg_len >> 8)); out.push_back((uint8_t)(seg_len & 0xFF));
        size_t at = out.size();
        out.resize(out.size() + payload);
        put_random(out.data() + at, payload);
        target_extra -= payload;
    }
    out.insert(out.end(), j.begin() + 2, j.end());
    return out;
}

// ---------- EXIF / XMP / Photoshop APP13 分辨率修补 ----------
// 背景：很多查看器（Photoshop、部分 WIC 流程）优先读 EXIF 或 Photoshop IRB 里的
// 分辨率，而不是 JFIF APP0 —— 只写 JFIF 会出现"改了 DPI 但查看器显示旧值"。
// 三处存储（如存在）必须一起改：
//   1) JFIF APP0   units + X/Y density
//   2) EXIF APP1   TIFF IFD0 的 XResolution(0x011A)/YResolution(0x011B)/ResolutionUnit(0x0128)
//   3) APP13 Photoshop 8BIM 0x03ED ResolutionInfo（16.16 定点 dpi）
//      以及 XMP APP1 的 <tiff:XResolution>/<tiff:YResolution>/<tiff:ResolutionUnit> 文本

// 就地修补 EXIF payload（"Exif\0\0"+TIFF）的分辨率 tag；成功返回 true。
// 没有分辨率 tag 时也返回 true（无旧值残留，JFIF 值即生效）；结构异常返回 false（调用方剥除该段）。
static bool exif_patch_dpi(std::vector<uint8_t>& seg, int dpi) {
    if (seg.size() < 6 + 8 || memcmp(seg.data(), "Exif\0\0", 6) != 0) return false;
    uint8_t* t = seg.data() + 6;
    size_t tn = seg.size() - 6;
    bool le = (t[0] == 'I' && t[1] == 'I');
    auto rd16 = [&](size_t o)->int { return le ? (t[o] | (t[o + 1] << 8)) : ((t[o] << 8) | t[o + 1]); };
    auto rd32 = [&](size_t o)->uint32_t {
        return le ? ((uint32_t)t[o] | ((uint32_t)t[o + 1] << 8) | ((uint32_t)t[o + 2] << 16) | ((uint32_t)t[o + 3] << 24))
                  : (((uint32_t)t[o] << 24) | ((uint32_t)t[o + 1] << 16) | ((uint32_t)t[o + 2] << 8) | (uint32_t)t[o + 3]);
    };
    auto wr32 = [&](size_t o, uint32_t v) {
        if (le) { t[o] = (uint8_t)v; t[o + 1] = (uint8_t)(v >> 8); t[o + 2] = (uint8_t)(v >> 16); t[o + 3] = (uint8_t)(v >> 24); }
        else    { t[o] = (uint8_t)(v >> 24); t[o + 1] = (uint8_t)(v >> 16); t[o + 2] = (uint8_t)(v >> 8); t[o + 3] = (uint8_t)v; }
    };
    auto wr16 = [&](size_t o, int v) {
        if (le) { t[o] = (uint8_t)v; t[o + 1] = (uint8_t)(v >> 8); }
        else    { t[o] = (uint8_t)(v >> 8); t[o + 1] = (uint8_t)v; }
    };
    uint32_t ifd0 = rd32(4);
    if (ifd0 + 2 > tn) return false;
    int n = rd16(ifd0);
    for (int k = 0; k < n; ++k) {
        size_t e = ifd0 + 2 + (size_t)k * 12;
        if (e + 12 > tn) return false;
        int tag = rd16(e), typ = rd16(e + 2);
        if (tag == 0x011A || tag == 0x011B) {           // X/YResolution: RATIONAL
            if (typ != 5) return false;
            uint32_t vo = rd32(e + 8);
            if (vo + 8 > tn) return false;
            wr32(vo, (uint32_t)dpi);                    // num = dpi
            wr32(vo + 4, 1);                            // den = 1
        } else if (tag == 0x0128) {                     // ResolutionUnit: SHORT（内联值）
            if (typ != 3) return false;
            wr16(e + 8, 2);                             // 2 = 英寸
        }
    }
    return true; // 不存在分辨率 tag 也算成功（无旧值）
}

// 读取 EXIF payload 的 XResolution（失败返回 0）
static int exif_read_dpi(const std::vector<uint8_t>& seg) {
    if (seg.size() < 6 + 8 || memcmp(seg.data(), "Exif\0\0", 6) != 0) return 0;
    const uint8_t* t = seg.data() + 6;
    size_t tn = seg.size() - 6;
    bool le = (t[0] == 'I' && t[1] == 'I');
    auto rd16 = [&](size_t o)->int { return le ? (t[o] | (t[o + 1] << 8)) : ((t[o] << 8) | t[o + 1]); };
    auto rd32 = [&](size_t o)->uint32_t {
        return le ? ((uint32_t)t[o] | ((uint32_t)t[o + 1] << 8) | ((uint32_t)t[o + 2] << 16) | ((uint32_t)t[o + 3] << 24))
                  : (((uint32_t)t[o] << 24) | ((uint32_t)t[o + 1] << 16) | ((uint32_t)t[o + 2] << 8) | (uint32_t)t[o + 3]);
    };
    uint32_t ifd0 = rd32(4);
    if (ifd0 + 2 > tn) return 0;
    int n = rd16(ifd0), x = 0;
    for (int k = 0; k < n && x == 0; ++k) {
        size_t e = ifd0 + 2 + (size_t)k * 12;
        if (e + 12 > tn) break;
        int tag = rd16(e), typ = rd16(e + 2);
        if (tag == 0x011A && typ == 5) {
            uint32_t vo = rd32(e + 8);
            if (vo + 8 <= tn) {
                uint32_t num = rd32(vo), den = rd32(vo + 4);
                if (den) x = (int)((num + den / 2) / den);
            }
        }
    }
    return x;
}

// 就地修补 Photoshop IRB（"Photoshop 3.0\0"+8BIM 记录）的 ResolutionInfo(0x03ED)。
// 结构异常返回 false（调用方剥除该段防旧值残留）；没有 0x03ED 返回 true（无旧值）。
static bool psir_patch_dpi(std::vector<uint8_t>& seg, int dpi) {
    if (seg.size() < 14 || memcmp(seg.data(), "Photoshop 3.0\0", 14) != 0) return false;
    size_t i = 14;
    bool saw_8bim = false;
    while (i + 12 <= seg.size()) {
        if (memcmp(seg.data() + i, "8BIM", 4) != 0) return false; // 未知签名：保守失败
        saw_8bim = true;
        size_t pos = i + 6;
        uint8_t nl = seg[pos];                       // Pascal 名（长度字节+名字，整体补齐到偶数）
        pos += 1 + nl;
        if ((1 + nl) % 2 != 0) pos += 1;             // 长度为奇数补 1 字节对齐
        if (pos + 4 > seg.size()) return false;
        uint32_t sz = ((uint32_t)seg[pos] << 24) | ((uint32_t)seg[pos + 1] << 16) |
                      ((uint32_t)seg[pos + 2] << 8) | (uint32_t)seg[pos + 3];
        pos += 4;
        if (pos + sz > seg.size()) return false;
        uint16_t rid = ((uint16_t)seg[i + 4] << 8) | seg[i + 5];
        if (rid == 0x03ED && sz >= 16) {             // ResolutionInfo：hDpi(16.16) hUnit hWUnit vDpi(16.16) vUnit vHUnit
            uint8_t* d = seg.data() + pos;
            uint32_t fx = (uint32_t)dpi << 16;
            for (int k = 0; k < 4; ++k) { d[k] = (uint8_t)(fx >> (24 - k * 8)); d[8 + k] = (uint8_t)(fx >> (24 - k * 8)); }
            d[4] = 0; d[5] = 1;                      // hResUnit = 1 (ppi)
            d[12] = 0; d[13] = 1;                    // vResUnit = 1 (ppi)
        }
        i = pos + sz + (sz & 1);                     // 数据补齐到偶数
    }
    return saw_8bim || seg.size() == 14;             // 空资源区也视为合法
}

// 修补 XMP payload 里的 <tiff:XResolution>/<tiff:YResolution>/<tiff:ResolutionUnit> 文本。
// 没有（或替换不动）时保持原样返回 true；发生替换时重建 payload。
static bool xmp_patch_dpi(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, int dpi) {
    std::string s(in.begin(), in.end());
    auto replace_tag = [&](const char* tag, const std::string& val)->bool {
        std::string open = std::string("<") + tag + ">";
        size_t p = s.find(open);
        if (p == std::string::npos) return false;
        size_t vs = p + open.size(), ve = s.find('<', vs);
        if (ve == std::string::npos) return false;
        if (s.compare(vs, ve - vs, val) != 0) s.replace(vs, ve - vs, val);
        return true;
    };
    char nbuf[32];
    snprintf(nbuf, sizeof nbuf, "%d/1", dpi);
    bool has_x = replace_tag("tiff:XResolution", nbuf);
    bool has_y = replace_tag("tiff:YResolution", nbuf);
    bool has_u = replace_tag("tiff:ResolutionUnit", "2");
    (void)has_u;
    out.assign(s.begin(), s.end());
    return !(has_x || has_y) || (has_x && has_y); // 只改了一半时宁可保留（调用方仍写 JFIF）
}

int jpeg_get_dpi(const std::vector<uint8_t>& j) {
    int exif_dpi = 0;
    for (auto& s : jpeg_segments(j)) {
        if (s.marker == 0xE0 && s.size >= 18) {
            const uint8_t* b = j.data() + s.offset + 4;
            if (memcmp(b, "JFIF\0", 5) == 0 && b[7] == 1) {
                int x = (b[8] << 8) | b[9]; // Xdensity（b[7]=units）
                int y = (b[12] << 8) | b[13];
                return x; // 假定 X=Y
            }
        }
        // 无 JFIF 时回退 EXIF（记录最后一个有效值）
        if (s.marker == 0xE1 && s.size > 10) {
            std::vector<uint8_t> seg(j.begin() + s.offset + 4, j.begin() + s.offset + s.size);
            int v = exif_read_dpi(seg);
            if (v) exif_dpi = v;
        }
    }
    return exif_dpi;
}

std::vector<uint8_t> jpeg_set_dpi(const std::vector<uint8_t>& j, int dpi) {
    if (dpi < 1) dpi = 72;
    if (dpi > 65535) dpi = 65535;
    // JFIF APP0 精确 18 字节：FF E0 00 10 + 'JFIF\0'(5) + 版本(2) + units(1)
    //   + Xdensity(2) + Ydensity(2) + 缩略图(2)；len=0x10=16 恰为 18-2
    uint8_t app0[18] = {
        0xFF, 0xE0, 0x00, 0x10, 'J','F','I','F',0,
        1, 1,                       // 版本 1.1
        1,                          // units=1（dpi）
        (uint8_t)(dpi >> 8), (uint8_t)(dpi & 0xFF),
        (uint8_t)(dpi >> 8), (uint8_t)(dpi & 0xFF),
        0, 0                        // 无缩略图
    };
    std::vector<uint8_t> out;
    out.reserve(j.size() + 16);
    out.push_back(0xFF); out.push_back(0xD8); // SOI
    out.insert(out.end(), app0, app0 + sizeof app0);
    bool first = true;
    for (auto& s : jpeg_segments(j)) {
        if (s.offset == 0) continue;               // SOI 已写
        if (s.marker == 0xE0 && first) continue;   // 跳过旧 APP0
        first = false;
        size_t body_off = s.offset + 4, body_len = s.size - 4;
        if (s.marker == 0xE1 && body_len > 6 &&
            memcmp(j.data() + body_off, "Exif\0\0", 6) == 0) {
            // EXIF：就地改 X/YResolution + ResolutionUnit；改不动则剥掉防旧值残留
            std::vector<uint8_t> seg(j.begin() + body_off, j.begin() + body_off + body_len);
            if (exif_patch_dpi(seg, dpi)) {
                out.push_back(0xFF); out.push_back(0xE1);
                out.push_back((uint8_t)((seg.size() + 2) >> 8)); out.push_back((uint8_t)(seg.size() + 2));
                out.insert(out.end(), seg.begin(), seg.end());
                continue;
            }
            continue; // 剥除
        }
        if (s.marker == 0xE1 && body_len > 4) {
            // XMP：改 tiff:*Resolution 文本（若有）
            std::vector<uint8_t> seg2, patched;
            seg2.assign(j.begin() + body_off, j.begin() + body_off + body_len);
            if (seg2.size() >= 29 && memcmp(seg2.data(), "http://ns.adobe.com/xap/1.0/\0", 29) == 0 &&
                xmp_patch_dpi(std::vector<uint8_t>(seg2.begin() + 29, seg2.end()), patched, dpi)) {
                seg2.resize(29);
                seg2.insert(seg2.end(), patched.begin(), patched.end());
                out.push_back(0xFF); out.push_back(0xE1);
                out.push_back((uint8_t)((seg2.size() + 2) >> 8)); out.push_back((uint8_t)(seg2.size() + 2));
                out.insert(out.end(), seg2.begin(), seg2.end());
                continue;
            }
            // 原样保留
        }
        if (s.marker == 0xED && body_len >= 14 &&
            memcmp(j.data() + body_off, "Photoshop 3.0\0", 14) == 0) {
            // Photoshop IRB：改 ResolutionInfo；改不动则剥掉防旧值残留
            std::vector<uint8_t> seg(j.begin() + body_off, j.begin() + body_off + body_len);
            if (psir_patch_dpi(seg, dpi)) {
                out.push_back(0xFF); out.push_back(0xED);
                out.push_back((uint8_t)((seg.size() + 2) >> 8)); out.push_back((uint8_t)(seg.size() + 2));
                out.insert(out.end(), seg.begin(), seg.end());
                continue;
            }
            continue; // 剥除
        }
        out.insert(out.end(), j.begin() + s.offset, j.begin() + s.offset + s.size);
    }
    return out;
}

// ---------- PNG ----------

static uint32_t png_chunk_crc(const std::vector<uint8_t>& data, size_t type_off, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < 4; ++i) c = table[(c ^ data[type_off + i]) & 0xFF] ^ (c >> 8);
    for (size_t i = 0; i < len; ++i) c = table[(c ^ data[type_off + 4 + i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static bool chunk_is(const std::vector<uint8_t>& d, size_t type_off, const char* name) {
    return memcmp(d.data() + type_off, name, 4) == 0;
}

std::vector<uint8_t> png_strip_meta(const std::vector<uint8_t>& p) {
    std::vector<uint8_t> out;
    if (p.size() < 8 || memcmp(p.data(), "\x89PNG\r\n\x1a\n", 8) != 0) return p;
    out.insert(out.end(), p.begin(), p.begin() + 8);
    size_t pos = 8;
    bool first_chunk = true;
    while (pos + 8 <= p.size()) {
        uint32_t len = ((uint32_t)p[pos] << 24) | ((uint32_t)p[pos + 1] << 16) | ((uint32_t)p[pos + 2] << 8) | p[pos + 3];
        size_t type_off = pos + 4;
        size_t total = len + 12;
        if (pos + total > p.size()) break;
        bool keep = true;
        if (chunk_is(p, type_off, "tEXt") || chunk_is(p, type_off, "iTXt") || chunk_is(p, type_off, "zTXt") ||
            chunk_is(p, type_off, "tIME") || chunk_is(p, type_off, "eXIf"))
            keep = false;
        if (chunk_is(p, type_off, "IHDR") && !first_chunk) keep = false; // 防御
        if (keep) out.insert(out.end(), p.begin() + pos, p.begin() + pos + total);
        if (chunk_is(p, type_off, "IEND")) break;
        first_chunk = false;
        pos += total;
    }
    return out;
}

std::vector<uint8_t> png_fill_text(const std::vector<uint8_t>& p, size_t target_extra) {
    std::vector<uint8_t> out;
    if (p.size() < 8 || memcmp(p.data(), "\x89PNG\r\n\x1a\n", 8) != 0 || target_extra == 0) return p;
    // 找 IEND 前的插入点
    size_t pos = 8, iend_pos = p.size();
    while (pos + 8 <= p.size()) {
        uint32_t len = ((uint32_t)p[pos] << 24) | ((uint32_t)p[pos + 1] << 16) | ((uint32_t)p[pos + 2] << 8) | p[pos + 3];
        if (memcmp(p.data() + pos + 4, "IEND", 4) == 0) { iend_pos = pos; break; }
        pos += len + 12;
    }
    out.insert(out.end(), p.begin(), p.begin() + iend_pos);
    while (target_extra > 0) {
        const char keyword[] = "comment";
        size_t data_max = target_extra + 1000;
        size_t payload = std::min<size_t>({target_extra, 65533 - 8 - sizeof keyword});
        // tEXt: keyword\0text
        uint32_t clen = (uint32_t)(sizeof keyword + payload);
        out.push_back((uint8_t)(clen >> 24)); out.push_back((uint8_t)(clen >> 16));
        out.push_back((uint8_t)(clen >> 8));  out.push_back((uint8_t)clen);
        out.insert(out.end(), keyword, keyword + sizeof keyword); // 含 \0
        size_t at = out.size();
        out.resize(out.size() + payload);
        put_random(out.data() + at, payload);
        uint32_t crc = png_chunk_crc(out, out.size() - clen - 4, clen);
        out.push_back((uint8_t)(crc >> 24)); out.push_back((uint8_t)(crc >> 16));
        out.push_back((uint8_t)(crc >> 8));  out.push_back((uint8_t)crc);
        target_extra -= payload;
    }
    out.insert(out.end(), p.begin() + iend_pos, p.end());
    return out;
}

std::vector<uint8_t> png_set_dpi(const std::vector<uint8_t>& p, int dpi) {
    if (p.size() < 8 || memcmp(p.data(), "\x89PNG\r\n\x1a\n", 8) != 0) return p;
    uint32_t ppm = (uint32_t)llround(dpi * 39.3701);
    // pHYs chunk: len=9, X ppm, Y ppm, unit=1(meter)
    std::vector<uint8_t> phys;
    uint32_t len = 9;
    phys.push_back((uint8_t)(len >> 24)); phys.push_back((uint8_t)(len >> 16));
    phys.push_back((uint8_t)(len >> 8));  phys.push_back((uint8_t)len);
    const char type[4] = {'p','H','Y','s'};
    phys.insert(phys.end(), type, type + 4);
    phys.push_back((uint8_t)(ppm >> 24)); phys.push_back((uint8_t)(ppm >> 16));
    phys.push_back((uint8_t)(ppm >> 8));  phys.push_back((uint8_t)ppm);
    phys.push_back((uint8_t)(ppm >> 24)); phys.push_back((uint8_t)(ppm >> 16));
    phys.push_back((uint8_t)(ppm >> 8));  phys.push_back((uint8_t)ppm);
    phys.push_back(1);
    uint32_t crc = png_chunk_crc(phys, 4, 9);
    phys.push_back((uint8_t)(crc >> 24)); phys.push_back((uint8_t)(crc >> 16));
    phys.push_back((uint8_t)(crc >> 8));  phys.push_back((uint8_t)crc);

    std::vector<uint8_t> out;
    out.insert(out.end(), p.begin(), p.begin() + 8); // 签名
    // IHDR（33 字节固定）之后插入 pHYs；跳过已有 pHYs
    size_t pos = 8;
    if (pos + 8 <= p.size()) {
        uint32_t ihdr_len = ((uint32_t)p[pos] << 24) | ((uint32_t)p[pos + 1] << 16) | ((uint32_t)p[pos + 2] << 8) | p[pos + 3];
        size_t after_ihdr = pos + ihdr_len + 12;
        out.insert(out.end(), p.begin() + pos, p.begin() + after_ihdr);
        pos = after_ihdr;
        while (pos + 8 <= p.size()) {
            uint32_t clen = ((uint32_t)p[pos] << 24) | ((uint32_t)p[pos + 1] << 16) | ((uint32_t)p[pos + 2] << 8) | p[pos + 3];
            if (memcmp(p.data() + pos + 4, "pHYs", 4) == 0) { pos += clen + 12; continue; } // 去旧
            if (memcmp(p.data() + pos + 4, "IDAT", 4) == 0) break; // pHYs 必须在 IDAT 前
            out.insert(out.end(), p.begin() + pos, p.begin() + pos + clen + 12);
            pos += clen + 12;
        }
    }
    out.insert(out.end(), phys.begin(), phys.end());
    out.insert(out.end(), p.begin() + pos, p.end());
    return out;
}

} // namespace sp
