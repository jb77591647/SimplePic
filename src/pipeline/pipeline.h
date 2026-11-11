#pragma once
// 批处理管线：文件队列 + 线程池 + 进度回调
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

namespace sp {

enum class ItemState { Waiting, Running, Done, Failed };

struct ItemResult {
    bool ok = false;
    std::wstring error;
    std::wstring out_path;
    long long out_bytes = 0;
};

// 用户参数（阶段 1：够用即可，随模块扩展）
struct Params {
    int module_id = 1;          // 当前选中的功能模块
    int target_kb = 500;        // 压缩目标体积
    bool fill_if_smaller = false; // 小于目标时：false=跳过 true=填充
    int id_photo = 0;           // 0=一寸 1=小一寸 2=二寸 3=小二寸 4=护照 5=自定义
    int id_custom_w = 295;      // 自定义宽（id_photo==5 时生效）
    int id_custom_h = 413;      // 自定义高
    int dpi = 300;              // 目标 DPI
    int out_fmt = 0;            // 0=JPG 1=PNG 2=BMP 3=保持原格式
    int rotate_deg = 0;         // 90/180/270/0
    int flip = 0;               // 0=无 1=水平 2=垂直
    int jpg_q = 85;
    bool strip_meta = true;     // 清除元数据（全局选项）
    int crop_ratio_idx = 0;     // 居中裁剪比例
    int crop_w = 800, crop_h = 600; // 自定义
    int fill_target_kb = 2000;  // 增体积目标
    int live_q = 80;            // 智能压缩：JPEG 质量 1~95
    int wm_font_size = 32;      // 水印字号 px
    int wm_alpha = 50;          // 水印不透明度 %
    std::wstring wm_text = L"SimplePic";
    int wm_color = 0;           // 8 色索引（见 ops.cpp kWmColors）
    int wm_angle = 0;           // 水印角度（度）
    int wm_layout = 0;          // 文字水印布局：0=平铺 1=居中单个 2=右下角单个
    int wm_spacing = 80;        // 文字水印平铺间距 px
    int stitch_dir = 0;         // 拼接：0=纵向 1=横向 2=九宫格3×3
    int stitch_fmt = 0;         // 拼接输出：0=PNG 1=JPG q95
    std::wstring wm_image_path; // 图片水印（空=不叠加）
    int wm_img_scale = 25;      // 图片水印宽度占宿主图宽度 %
    int wm_img_alpha = 50;      // 图片水印不透明度 %（独立于文字水印）
    int wm_img_layout = 0;      // 图片水印布局：0=平铺 1=居中 2=右下角 3=左上角 4=右上角 5=左下角
    int wm_img_angle = 0;       // 图片水印旋转角度（度，任意角，逆时针）
    int wm_img_spacing = 80;    // 图片水印平铺间距 px
    bool pdf_merge = false;     // 多图转 PDF：合并为单个 PDF 文件
};

struct FileItem {
    std::wstring path;
    int w = 0, h = 0;
    long long bytes = 0;
    ItemState state = ItemState::Waiting;
    ItemResult result;
};

using ProgressFn = std::function<void(int index, ItemState st, const ItemResult& res)>;

class Pipeline {
public:
    // files: 待处理列表（传全量，回调 index 即全量索引）；only_idx: 仅处理的索引子集，空=全部
    void start(std::vector<FileItem> files, const std::vector<int>& only_idx,
               const Params& p, std::wstring out_dir, ProgressFn on_item);
    void cancel();
    bool running() const { return running_.load(); }
    static unsigned hardware_threads();

private:
    void worker_loop();
    ItemResult process_one(const FileItem& item);

    std::vector<std::thread> workers_;
    std::queue<size_t> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};

    std::vector<FileItem> items_;
    Params params_;
    std::wstring out_dir_;
    ProgressFn on_item_;
};

} // namespace sp
