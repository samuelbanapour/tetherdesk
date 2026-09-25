#include "encoder.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "rd_codec.h"

namespace td {

WorkerPool::WorkerPool(unsigned threads) {
    for (unsigned i = 0; i < threads; i++) workers_.emplace_back([this, i] { worker_main(i + 1); });
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        quit_ = true;
    }
    cv_start_.notify_all();
    for (auto &t : workers_) t.join();
}

void WorkerPool::worker_main(unsigned index) {
    uint64_t seen = 0;
    for (;;) {
        const std::function<void(unsigned)> *job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_start_.wait(lock, [&] { return quit_ || generation_ != seen; });
            if (quit_) return;
            seen = generation_;
            job = job_;
        }
        (*job)(index);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (--pending_ == 0) cv_done_.notify_one();
        }
    }
}

void WorkerPool::run_all(const std::function<void(unsigned)> &fn) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        job_ = &fn;
        pending_ = unsigned(workers_.size());
        generation_++;
    }
    cv_start_.notify_all();
    fn(0);
    std::unique_lock<std::mutex> lock(mu_);
    cv_done_.wait(lock, [&] { return pending_ == 0; });
}

FrameEncoder::FrameEncoder(WorkerPool &pool) : pool_(pool), scratch_(pool.size()) {
    for (auto &b : scratch_) rd_buf_init(&b);
}

FrameEncoder::~FrameEncoder() {
    for (auto &b : scratch_) rd_buf_free(&b);
}

EncodeStats FrameEncoder::encode(const Frame &f, std::vector<uint8_t> &shadow, bool full, int quality, rd_buf &out) {
    const int tiles_x = (f.width + RD_TILE - 1) / RD_TILE;
    const int tiles_y = (f.height + RD_TILE - 1) / RD_TILE;
    const int total = tiles_x * tiles_y;
    const size_t shadow_stride = size_t(f.width) * 4;
    std::atomic<int> next{0};
    std::vector<EncodeStats> stats(scratch_.size());

    pool_.run_all([&](unsigned wi) {
        rd_buf &buf = scratch_[wi];
        rd_buf_clear(&buf);
        EncodeStats &st = stats[wi];
        // Grab tiles a row-chunk at a time to keep cache locality.
        for (int t; (t = next.fetch_add(4)) < total;) {
            for (int k = t; k < t + 4 && k < total; k++) {
                const int tx = (k % tiles_x) * RD_TILE, ty = (k / tiles_x) * RD_TILE;
                const int tw = std::min(RD_TILE, f.width - tx), th = std::min(RD_TILE, f.height - ty);
                const uint8_t *src = f.pixels.data() + size_t(ty) * f.stride + size_t(tx) * 4;
                uint8_t *sh = shadow.data() + size_t(ty) * shadow_stride + size_t(tx) * 4;
                bool dirty = full;
                for (int y = 0; y < th && !dirty; y++)
                    dirty = std::memcmp(src + size_t(y) * f.stride, sh + size_t(y) * shadow_stride, size_t(tw) * 4) != 0;
                if (!dirty) continue;
                for (int y = 0; y < th; y++)
                    std::memcpy(sh + size_t(y) * shadow_stride, src + size_t(y) * f.stride, size_t(tw) * 4);

                rd_buf_put_u16(&buf, uint16_t(tx));
                rd_buf_put_u16(&buf, uint16_t(ty));
                rd_buf_put_u16(&buf, uint16_t(tw));
                rd_buf_put_u16(&buf, uint16_t(th));
                const size_t enc_at = buf.len;
                rd_buf_put_u8(&buf, 0);
                rd_buf_put_u32(&buf, 0);
                const size_t start = buf.len;
                int enc = rd_encode_tile(src, size_t(f.stride), tw, th, quality, &buf);
                buf.data[enc_at] = uint8_t(enc);
                rd_buf_patch_u32(&buf, enc_at + 1, uint32_t(buf.len - start));
                st.tiles++;
                st.by_encoding[enc]++;
            }
        }
    });

    EncodeStats total_stats;
    for (size_t i = 0; i < scratch_.size(); i++) {
        rd_buf_put(&out, scratch_[i].data, scratch_[i].len);
        total_stats.tiles += stats[i].tiles;
        total_stats.bytes += scratch_[i].len;
        for (int e = 0; e < 4; e++) total_stats.by_encoding[e] += stats[i].by_encoding[e];
    }
    return total_stats;
}

}  // namespace td
