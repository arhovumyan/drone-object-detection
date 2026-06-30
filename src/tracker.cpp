#include "tracker.hpp"
#include <cmath>
#include <limits>

namespace dd {

Target Tracker::update(const std::vector<Detection>& dets, int frameW, int frameH) {
    Target t;
    if (frameW <= 0 || frameH <= 0) return t;

    const Detection* best = nullptr;
    if (!dets.empty()) {
        if (has_) {
            // Pick the detection closest to the last known target center.
            float bestD = std::numeric_limits<float>::max();
            for (const auto& d : dets) {
                float dcx = d.x + d.w * 0.5f, dcy = d.y + d.h * 0.5f;
                float dist = (dcx - cx_) * (dcx - cx_) + (dcy - cy_) * (dcy - cy_);
                if (dist < bestD) { bestD = dist; best = &d; }
            }
        } else {
            // No prior target: pick the largest detection.
            float bestA = -1.f;
            for (const auto& d : dets) {
                float a = d.w * d.h;
                if (a > bestA) { bestA = a; best = &d; }
            }
        }
    }

    if (best) {
        float dcx = best->x + best->w * 0.5f;
        float dcy = best->y + best->h * 0.5f;
        if (has_) {
            cx_ += kAlpha * (dcx - cx_);
            cy_ += kAlpha * (dcy - cy_);
            w_  += kAlpha * (best->w - w_);
            h_  += kAlpha * (best->h - h_);
        } else {
            cx_ = dcx; cy_ = dcy; w_ = best->w; h_ = best->h;
        }
        conf_ = best->score;
        has_ = true;
        missed_ = 0;
    } else if (has_) {
        if (++missed_ > kMaxMissed) { has_ = false; }
    }

    if (has_) {
        t.valid = true;
        t.cx = cx_; t.cy = cy_; t.w = w_; t.h = h_;
        t.conf = conf_;
        t.ex = (cx_ - frameW * 0.5f) / (frameW * 0.5f);
        t.ey = (cy_ - frameH * 0.5f) / (frameH * 0.5f);
    }
    return t;
}

} // namespace dd
