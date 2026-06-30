#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace dd {

// One captured frame, always stored top-down as 32-bit BGRA (B,G,R,X).
struct Frame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;   // size == width * height * 4
    bool empty() const { return width <= 0 || height <= 0 || bgra.empty(); }
};

struct CameraConfig {
    bool builtInOnly      = true;   // Windows: only accept the integrated camera
    int  requestWidth     = 1280;
    int  requestHeight    = 720;
    int  deviceIndexOverride = -1;  // -1 = auto-select
    bool flipVertical     = false;  // flip if the preview comes out upside down
};

class ICamera {
public:
    virtual ~ICamera() = default;
    virtual bool grab(Frame& out) = 0;   // blocking; fills the latest frame
    virtual int  width()  const = 0;
    virtual int  height() const = 0;
};

// Implemented per-platform (camera_windows.cpp / camera_linux.cpp).
// Returns nullptr on failure (after printing a diagnostic).
std::unique_ptr<ICamera> CreateCamera(const CameraConfig& cfg);

} // namespace dd
