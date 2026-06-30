// Default detector: grid-based frame differencing. No model required, runs on
// any platform. Produces a single bounding box around the dominant moving
// region. This is the placeholder that the YOLO26/TensorRT detector replaces
// behind the same IDetector interface on the Jetson.

#include "detector.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace dd {
namespace {

class MotionDetector : public IDetector {
public:
    std::vector<Detection> detect(const Frame& f) override {
        std::vector<Detection> out;
        if (f.empty()) return out;

        // Average luma per grid cell (cheap, noise-robust).
        std::vector<float> cell(static_cast<size_t>(kCols) * kRows, 0.f);
        std::vector<int>   cnt(static_cast<size_t>(kCols) * kRows, 0);
        const int W = f.width, H = f.height;
        for (int y = 0; y < H; y += kSample) {
            int gy = y * kRows / H;
            const uint8_t* row = &f.bgra[static_cast<size_t>(y) * W * 4];
            for (int x = 0; x < W; x += kSample) {
                int gx = x * kCols / W;
                const uint8_t* p = row + static_cast<size_t>(x) * 4;
                float luma = 0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2]; // BGR
                size_t idx = static_cast<size_t>(gy) * kCols + gx;
                cell[idx] += luma;
                cnt[idx]++;
            }
        }
        for (size_t i = 0; i < cell.size(); ++i)
            if (cnt[i]) cell[i] /= cnt[i];

        if (prev_.size() != cell.size()) { prev_ = cell; return out; }

        // Bounding box over cells whose luma changed enough.
        int minX = kCols, minY = kRows, maxX = -1, maxY = -1, active = 0;
        for (int gy = 0; gy < kRows; ++gy) {
            for (int gx = 0; gx < kCols; ++gx) {
                size_t i = static_cast<size_t>(gy) * kCols + gx;
                if (std::fabs(cell[i] - prev_[i]) > kCellThresh) {
                    minX = std::min(minX, gx); maxX = std::max(maxX, gx);
                    minY = std::min(minY, gy); maxY = std::max(maxY, gy);
                    ++active;
                }
            }
        }
        prev_ = cell;

        if (active >= kMinActiveCells && maxX >= minX) {
            float cellW = static_cast<float>(W) / kCols;
            float cellH = static_cast<float>(H) / kRows;
            Detection d;
            d.x = minX * cellW;
            d.y = minY * cellH;
            d.w = (maxX - minX + 1) * cellW;
            d.h = (maxY - minY + 1) * cellH;
            d.score = std::min(1.0f, active / 20.0f);
            d.label = 0;
            out.push_back(d);
        }
        return out;
    }

    const char* name() const override { return "motion (frame-diff)"; }

private:
    // Finer grid + denser sampling so small/distant objects register.
    static constexpr int   kCols = 128;
    static constexpr int   kRows = 72;
    static constexpr int   kSample = 2;        // pixel step when sampling
    static constexpr float kCellThresh = 9.f;  // luma delta to count a cell
    static constexpr int   kMinActiveCells = 2;
    std::vector<float> prev_;
};

} // namespace

std::unique_ptr<IDetector> CreateDetector(DetectorKind kind) {
    if (kind == DetectorKind::Yolo26) {
        std::cerr << "[detector] YOLO26/TensorRT not built on this platform; "
                     "falling back to motion detector.\n";
    }
    return std::make_unique<MotionDetector>();
}

} // namespace dd
