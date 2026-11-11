#include "pipeline.h"
#include "../ops/ops.h"
#include <filesystem>
#include <algorithm>

namespace sp {

unsigned Pipeline::hardware_threads() {
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 4;
}

void Pipeline::start(std::vector<FileItem> files, const std::vector<int>& only_idx,
                     const Params& p, std::wstring out_dir, ProgressFn on_item) {
    if (running_.load()) return;
    items_ = std::move(files);
    params_ = p;
    out_dir_ = std::move(out_dir);
    on_item_ = std::move(on_item);
    cancel_.store(false);

    std::filesystem::create_directories(std::filesystem::path(out_dir_));

    while (!queue_.empty()) queue_.pop();
    if (only_idx.empty()) {
        // 全量
        for (size_t i = 0; i < items_.size(); ++i) {
            items_[i].state = ItemState::Waiting;
            queue_.push(i);
        }
    } else {
        // 子集：只入队选中索引（回调 index 仍是全量索引，UI 行对应关系不变）
        for (int i : only_idx)
            if (i >= 0 && i < (int)items_.size()) {
                items_[(size_t)i].state = ItemState::Waiting;
                queue_.push((size_t)i);
            }
    }
    running_.store(true);

    unsigned n = std::min<unsigned>(hardware_threads(), (unsigned)queue_.size());
    for (unsigned i = 0; i < n; ++i)
        workers_.emplace_back(&Pipeline::worker_loop, this);

    // 汇收线程：join 全部 worker 后结束
    std::thread([this] {
        for (auto& t : workers_) if (t.joinable()) t.join();
        workers_.clear();
        running_.store(false);
        if (on_item_) on_item_(-1, ItemState::Done, {}); // 完成哨兵，通知 UI 收尾
    }).detach();
}

void Pipeline::cancel() { cancel_.store(true); cv_.notify_all(); }

void Pipeline::worker_loop() {
    // 任务队列有限：排空即退出（不阻塞等待），否则 running_ 永远无法归位
    for (;;) {
        size_t idx;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (cancel_.load() || queue_.empty()) return;
            idx = queue_.front();
            queue_.pop();
        }
        items_[idx].state = ItemState::Running;
        if (on_item_) on_item_((int)idx, ItemState::Running, {});
        ItemResult r = process_one(items_[idx]);
        items_[idx].state = r.ok ? ItemState::Done : ItemState::Failed;
        items_[idx].result = r;
        if (on_item_) on_item_((int)idx, items_[idx].state, r);
    }
}

ItemResult Pipeline::process_one(const FileItem& item) {
    ItemResult r;
    try {
        r = sp::run_op(item.path, params_, out_dir_);
    } catch (const std::exception& e) {
        r.ok = false;
        std::string es = e.what();
        r.error = std::wstring(es.begin(), es.end());
    } catch (...) {
        r.ok = false;
        r.error = L"unknown error";
    }
    return r;
}

} // namespace sp
