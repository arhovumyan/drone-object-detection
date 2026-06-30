// Real-time drone detection & tracking.
// Pipeline: camera -> detector -> tracker -> normalized error (ex,ey) + preview.
//
// The platform backend (camera + display) is selected by CMake at build time;
// at runtime we also report whether we are on a Jetson.

#include "platform.hpp"
#include "camera.hpp"
#include "detector.hpp"
#include "tracker.hpp"
#include "display.hpp"

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace dd;

int main(int argc, char** argv) {
    CameraConfig cam;
    bool headless = false;
    DetectorKind detKind = DetectorKind::Motion;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](int& dst) { if (i + 1 < argc) dst = std::atoi(argv[++i]); };
        if      (a == "--no-builtin") cam.builtInOnly = false;
        else if (a == "--device")     next(cam.deviceIndexOverride);
        else if (a == "--width")      next(cam.requestWidth);
        else if (a == "--height")     next(cam.requestHeight);
        else if (a == "--flip")       cam.flipVertical = true;
        else if (a == "--headless")   headless = true;
        else if (a == "--yolo")       detKind = DetectorKind::Yolo26;
        else if (a == "--help") {
            std::cout << "Usage: drone_detect [--no-builtin] [--device N] "
                         "[--width W] [--height H] [--flip] [--headless] [--yolo]\n";
            return 0;
        }
    }

    std::cout << "=== Drone Detect ===\n"
              << "Host: " << HostDescription() << "\n"
              << "OS backend: " << OSName()
              << (RuntimeIsJetson() ? "  [Jetson runtime]" : "") << "\n";

    // On a Jetson, prefer the YOLO26 detector automatically.
    if (RuntimeIsJetson() && detKind == DetectorKind::Motion)
        detKind = DetectorKind::Yolo26;

    auto camera = CreateCamera(cam);
    if (!camera) { std::cerr << "Camera init failed.\n"; return 1; }

    auto detector = CreateDetector(detKind);
    std::cout << "Detector: " << detector->name() << "\n";

    auto display = CreateDisplay(headless);
    Tracker tracker;

    Frame frame;
    auto last = std::chrono::steady_clock::now();
    double fps = 0;
    int printThrottle = 0;

    while (display->isOpen()) {
        if (!camera->grab(frame)) {
            std::cerr << "Frame grab failed; stopping.\n";
            break;
        }

        auto dets   = detector->detect(frame);
        auto target = tracker.update(dets, frame.width, frame.height);

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (dt > 0) fps = 0.9 * fps + 0.1 * (1.0 / dt);

        Overlay ov;
        ov.hasTarget = target.valid;
        ov.cx = target.cx; ov.cy = target.cy; ov.w = target.w; ov.h = target.h;
        ov.ex = target.ex; ov.ey = target.ey; ov.conf = target.conf;
        ov.fps = fps;
        display->present(frame, ov);

        if (++printThrottle >= 15) {
            printThrottle = 0;
            if (target.valid)
                std::printf("target  ex=%+.3f ey=%+.3f conf=%.2f  | %.1f fps\n",
                            target.ex, target.ey, target.conf, fps);
            else
                std::printf("searching...  | %.1f fps\n", fps);
            std::fflush(stdout);
        }
    }

    std::cout << "Bye.\n";
    return 0;
}
