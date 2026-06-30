# Drone Detect

Real-time drone **detection & tracking**. Camera → detector → tracker → normalized
error `(ex, ey)` from image center (`(0,0)` = centered), plus a live preview overlay.
Cross-platform: the camera/display backend is chosen automatically per OS, and the binary
reports at runtime whether it is on a Jetson.

> Scope: perception + tracking only. It outputs *where the target is* and *how far from
> center*. It does not include flight-control/pursuit logic. See `PLAN.md`.

## Build — Windows (Visual Studio 2026)

From a **Developer PowerShell for VS** (or after running `Launch-VsDevShell.ps1`):

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build\drone_detect.exe`.

## Run

```powershell
.\build\drone_detect.exe            # preview window, built-in camera, ESC to quit
.\build\drone_detect.exe --headless # no window, prints (ex,ey) to console
```

Flags: `--no-builtin` (allow external/USB cams), `--device N` (force a device index),
`--width W --height H`, `--flip` (if preview is upside down), `--yolo` (use YOLO26 when
built on Jetson; falls back to motion otherwise).

On Windows only the **built-in** camera is used by default — external USB/LAN cameras are
detected but skipped (selected via the device's Windows removal policy).

## Build — Linux / Jetson

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/drone_detect --headless
```

CMake auto-detects a Jetson (`/proc/device-tree/model`) and defines `DD_JETSON`. The Linux
camera backend uses V4L2 (YUYV); for Jetson CSI/Argus sensors, wire GStreamer
(`nvarguscamerasrc`) into `camera_linux.cpp`.

## What plugs in next

The detector sits behind `IDetector` (`src/detector.hpp`). The current default is a
no-model **motion detector** so the pipeline runs anywhere today. On the Jetson, implement a
`Yolo26TensorRtDetector` against the same interface (TensorRT FP16/INT8 engine) and return it
from `CreateDetector(DetectorKind::Yolo26)` — nothing else in the pipeline changes.

## Layout

```
src/platform.hpp      OS detection + runtime Jetson check
src/camera.hpp        Frame + ICamera + CreateCamera()
src/camera_windows.cpp  Media Foundation, built-in-camera selection
src/camera_linux.cpp    V4L2 capture (Jetson/Linux)
src/detector.hpp      Detection + IDetector + CreateDetector()
src/detector_motion.cpp frame-diff detector (default)
src/tracker.{hpp,cpp} single-target select + EMA smoothing + (ex,ey)
src/display.hpp       Overlay + IDisplay + CreateDisplay()
src/display_windows.cpp GDI preview window
src/display_null.cpp    headless no-op display
src/main.cpp          pipeline + CLI
```
