#pragma once
#include "camera.hpp"
#include <memory>

namespace dd {

// What to draw on top of the frame.
struct Overlay {
    bool  hasTarget = false;
    float cx = 0, cy = 0, w = 0, h = 0;  // target box (frame pixels)
    float ex = 0, ey = 0;                // normalized error
    float conf = 0;
    double fps = 0;
};

class IDisplay {
public:
    virtual ~IDisplay() = default;
    virtual bool isOpen() const = 0;
    virtual void present(const Frame& frame, const Overlay& ov) = 0;
};

// Windows -> GDI preview window. Linux/headless -> no-op display.
std::unique_ptr<IDisplay> CreateDisplay(bool headless);

} // namespace dd
