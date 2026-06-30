#pragma once
#include "detector.hpp"

namespace dd {

// The thing we steer toward keeping centered.
struct Target {
    bool  valid = false;
    float cx = 0, cy = 0;   // center in pixels
    float w = 0,  h = 0;
    float ex = 0, ey = 0;   // normalized error from image center, range [-1, 1]
    float conf = 0;
};

// Single-target selector + smoother. Picks the detection nearest the last
// known target (or the largest if none), EMA-smooths the center, and coasts
// through brief dropouts before declaring the target lost.
class Tracker {
public:
    Target update(const std::vector<Detection>& dets, int frameW, int frameH);

private:
    bool  has_ = false;
    float cx_ = 0, cy_ = 0, w_ = 0, h_ = 0, conf_ = 0;
    int   missed_ = 0;
    static constexpr float kAlpha = 0.5f;   // EMA smoothing
    static constexpr int   kMaxMissed = 15; // frames before dropping target
};

} // namespace dd
