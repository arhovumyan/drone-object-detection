// ONNX Runtime YOLO detector (default target: a single-class "drone" model).
//
// Works with Ultralytics YOLOv8/v11-style exports: input  (1,3,640,640) RGB,
// normalized 0..1, NCHW; output (1, 4+C, 8400) where the first 4 rows are box
// center/size in input-pixel space and the remaining C rows are per-class
// scores already in 0..1. We letterbox the frame to 640, run inference, decode,
// run NMS, and map boxes back to original frame pixels. Swapping in a different
// YOLO model (e.g. a drone-vs-bird model) needs no code change here.
//
// Compiled only when the build found ONNX Runtime (see CMakeLists.txt);
// otherwise detector_onnx_stub.cpp provides a CreateOnnxDetector that explains
// the model path is unavailable.

#include "detector.hpp"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace dd {
namespace {

constexpr int kInput = 640;  // square model input

struct Box { float x, y, w, h, score; };

float iou(const Box& a, const Box& b) {
    float ax2 = a.x + a.w, ay2 = a.y + a.h;
    float bx2 = b.x + b.w, by2 = b.y + b.h;
    float ix = std::max(0.f, std::min(ax2, bx2) - std::max(a.x, b.x));
    float iy = std::max(0.f, std::min(ay2, by2) - std::max(a.y, b.y));
    float inter = ix * iy;
    float uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0 ? inter / uni : 0.f;
}

class OnnxYoloDetector : public IDetector {
public:
    OnnxYoloDetector(const std::string& path, float conf)
        : conf_(conf),
          env_(ORT_LOGGING_LEVEL_WARNING, "drone_detect"),
          session_(nullptr) {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(4);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session_ = Ort::Session(env_, path.c_str(), opts);

        Ort::AllocatorWithDefaultOptions alloc;
        inName_  = session_.GetInputNameAllocated(0, alloc).get();
        outName_ = session_.GetOutputNameAllocated(0, alloc).get();
        std::cout << "[onnx] loaded " << path << "  in='" << inName_
                  << "' out='" << outName_ << "'\n";
    }

    std::vector<Detection> detect(const Frame& f) override {
        std::vector<Detection> result;
        if (f.empty()) return result;

        // ---- Letterbox the BGRA frame into a 640x640 RGB CHW float tensor.
        const int W = f.width, H = f.height;
        const float scale = std::min((float)kInput / W, (float)kInput / H);
        const int newW = (int)std::round(W * scale);
        const int newH = (int)std::round(H * scale);
        const float padX = (kInput - newW) * 0.5f;
        const float padY = (kInput - newH) * 0.5f;

        input_.assign((size_t)3 * kInput * kInput, 114.f / 255.f);  // gray pad
        const size_t plane = (size_t)kInput * kInput;
        for (int py = 0; py < newH; ++py) {
            float sy = (py + 0.5f) / scale - 0.5f;
            int y0 = std::clamp((int)std::floor(sy), 0, H - 1);
            int y1 = std::min(y0 + 1, H - 1);
            float fy = sy - y0;
            int oy = (int)(padY) + py;
            if (oy < 0 || oy >= kInput) continue;
            for (int px = 0; px < newW; ++px) {
                float sx = (px + 0.5f) / scale - 0.5f;
                int x0 = std::clamp((int)std::floor(sx), 0, W - 1);
                int x1 = std::min(x0 + 1, W - 1);
                float fx = sx - x0;
                int ox = (int)(padX) + px;
                if (ox < 0 || ox >= kInput) continue;

                auto px_at = [&](int x, int y, int c) -> float {
                    return f.bgra[((size_t)y * W + x) * 4 + c];
                };
                // bilinear per BGRA channel, then map B,G,R -> R,G,B planes.
                auto bilin = [&](int c) {
                    float top = px_at(x0, y0, c) * (1 - fx) + px_at(x1, y0, c) * fx;
                    float bot = px_at(x0, y1, c) * (1 - fx) + px_at(x1, y1, c) * fx;
                    return (top * (1 - fy) + bot * fy) / 255.f;
                };
                size_t off = (size_t)oy * kInput + ox;
                input_[0 * plane + off] = bilin(2);  // R
                input_[1 * plane + off] = bilin(1);  // G
                input_[2 * plane + off] = bilin(0);  // B
            }
        }

        // ---- Inference.
        std::array<int64_t, 4> shape{1, 3, kInput, kInput};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value in = Ort::Value::CreateTensor<float>(
            mem, input_.data(), input_.size(), shape.data(), shape.size());

        const char* inNames[]  = {inName_.c_str()};
        const char* outNames[] = {outName_.c_str()};
        auto outs = session_.Run(Ort::RunOptions{nullptr}, inNames, &in, 1, outNames, 1);

        const float* out = outs[0].GetTensorData<float>();
        auto oshape = outs[0].GetTensorTypeAndShapeInfo().GetShape();  // [1, 4+C, N]
        if (oshape.size() != 3) return result;
        const int attrs = (int)oshape[1];          // 4 + numClasses
        const int n     = (int)oshape[2];          // predictions (e.g. 8400)
        const int numClasses = attrs - 4;
        if (numClasses < 1) return result;

        // ---- Decode (output is channel-major: out[c*n + i]).
        std::vector<Box> cand;
        for (int i = 0; i < n; ++i) {
            float best = 0.f; int bestC = 0;
            for (int c = 0; c < numClasses; ++c) {
                float s = out[(size_t)(4 + c) * n + i];
                if (s > best) { best = s; bestC = c; }
            }
            if (best < conf_) continue;
            (void)bestC;  // single target class for now
            float cx = out[(size_t)0 * n + i];
            float cy = out[(size_t)1 * n + i];
            float bw = out[(size_t)2 * n + i];
            float bh = out[(size_t)3 * n + i];
            // letterbox -> original frame pixels.
            float x = (cx - bw * 0.5f - padX) / scale;
            float y = (cy - bh * 0.5f - padY) / scale;
            float w = bw / scale;
            float h = bh / scale;
            x = std::clamp(x, 0.f, (float)W);
            y = std::clamp(y, 0.f, (float)H);
            w = std::min(w, W - x);
            h = std::min(h, H - y);
            if (w > 1 && h > 1) cand.push_back({x, y, w, h, best});
        }

        // ---- NMS.
        std::sort(cand.begin(), cand.end(),
                  [](const Box& a, const Box& b) { return a.score > b.score; });
        std::vector<bool> dead(cand.size(), false);
        for (size_t i = 0; i < cand.size(); ++i) {
            if (dead[i]) continue;
            Detection d;
            d.x = cand[i].x; d.y = cand[i].y; d.w = cand[i].w; d.h = cand[i].h;
            d.score = cand[i].score; d.label = 0;
            result.push_back(d);
            for (size_t j = i + 1; j < cand.size(); ++j)
                if (!dead[j] && iou(cand[i], cand[j]) > kNmsIou) dead[j] = true;
        }
        return result;
    }

    const char* name() const override { return "onnx-yolo (drone)"; }

private:
    static constexpr float kNmsIou = 0.45f;
    float conf_;
    Ort::Env     env_;
    Ort::Session session_;
    std::string  inName_, outName_;
    std::vector<float> input_;
};

} // namespace

std::unique_ptr<IDetector> CreateOnnxDetector(const std::string& modelPath, float conf) {
    try {
        return std::make_unique<OnnxYoloDetector>(modelPath, conf);
    } catch (const std::exception& e) {
        std::cerr << "[onnx] Failed to load model '" << modelPath << "': "
                  << e.what() << "\n";
        return nullptr;
    }
}

} // namespace dd
