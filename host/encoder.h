// encoder.h - turns captured frames into TetherDesk FRAME messages.
//
// Each viewer keeps a "shadow" copy of what it has been sent. Encoding a
// frame compares every tile against the shadow, encodes only tiles that
// differ, and updates the shadow. Tiles are processed in parallel on a small
// worker pool; the codec (rd_codec.c) is reentrant.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "platform.h"
#include "rd_bytes.h"

namespace td {

class WorkerPool {
public:
    explicit WorkerPool(unsigned threads);
    ~WorkerPool();
    unsigned size() const { return unsigned(workers_.size()) + 1; }
    // Runs fn(worker_index) on every worker plus the calling thread; returns
    // when all have finished.
    void run_all(const std::function<void(unsigned)> &fn);

private:
    void worker_main(unsigned index);
    std::vector<std::thread> workers_;
    std::mutex mu_;
    std::condition_variable cv_start_, cv_done_;
    const std::function<void(unsigned)> *job_ = nullptr;
    uint64_t generation_ = 0;
    unsigned pending_ = 0;
    bool quit_ = false;
};

struct EncodeStats {
    int tiles = 0;
    int refined = 0;   // lossy tiles re-sent losslessly
    size_t bytes = 0;
    int by_encoding[4] = {0, 0, 0, 0};
};

class FrameEncoder {
public:
    explicit FrameEncoder(WorkerPool &pool);
    ~FrameEncoder();
    // Appends the tile records for every changed tile (all tiles if `full`)
    // to `out` and updates `shadow` (w*h*4 bytes, same layout as the frame).
    // `tile_q` (optional, one entry per tile) records the quality each tile
    // was last sent at; up to `refine_budget` unchanged tiles that were sent
    // lossy are re-sent losslessly, so still areas sharpen automatically.
    EncodeStats encode(const Frame &f, std::vector<uint8_t> &shadow, bool full, int quality, rd_buf &out,
                       std::vector<uint8_t> *tile_q = nullptr, int refine_budget = 0);

private:
    WorkerPool &pool_;
    std::vector<rd_buf> scratch_;  // one per worker
};

}  // namespace td
