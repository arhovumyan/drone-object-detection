#pragma once
#include "camera.hpp"
#include <memory>
#include <vector>

namespace dd {

// Axis-aligned box in frame pixel coordinates.
struct Detection {
    float x = 0, y = 0, w = 0, h = 0;   // top-left + size
    float score = 0;
    int   label = 0;
};

class IDetector {
public:
    virtual ~IDetector() = default;
    virtual std::vector<Detection> detect(const Frame& frame) = 0;
    virtual const char* name() const = 0;
};

enum class DetectorKind {
    Motion,   // default: frame-differencing, no model needed (works everywhere now)
    Yolo26    // TensorRT YOLO26 on Jetson (drops in here later)
};

std::unique_ptr<IDetector> CreateDetector(DetectorKind kind);

} // namespace dd
