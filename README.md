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

## Build — macOS (Apple Silicon or Intel)

Requires CMake and the Xcode command-line tools (`xcode-select --install`):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/drone_detect              # Cocoa preview window, built-in camera, ESC to quit
./build/drone_detect --headless   # no window, prints (ex,ey) to console
```

Output: `build/drone_detect`. The camera backend uses **AVFoundation** (built-in
FaceTime camera by default; `--device N` selects another, `--no-builtin` has no
special effect here) and the preview is a **Cocoa** window matching the Windows
overlay. The first run triggers a macOS camera-permission prompt — grant access
to your terminal app under *System Settings → Privacy & Security → Camera*. The
same flags as Windows apply (`--width/--height` map to the nearest capture preset).

## Build — Linux / Jetson

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/drone_detect --headless
```

CMake auto-detects a Jetson (`/proc/device-tree/model`) and defines `DD_JETSON`. The Linux
camera backend uses V4L2 (YUYV); for Jetson CSI/Argus sensors, wire GStreamer
(`nvarguscamerasrc`) into `camera_linux.cpp`.

## Detector: ready-made YOLO drone model (ONNX)

The default detector is motion (frame-diff). For real drone detection, pass an
ONNX YOLO model:

```bash
bash models/export_drone_model.sh          # fetch + export models/drone_yolo11.onnx (one-time)
./build/drone_detect --model models/drone_yolo11.onnx
./build/drone_detect --model models/drone_yolo11.onnx --conf 0.35 --headless
```

The bundled choice is **doguilmak/Drone-Detection-YOLOv11x** (single `drone`
class, MIT license). The detector (`src/detector_onnx.cpp`) takes any Ultralytics
YOLOv8/v11-style export — input `(1,3,640,640)`, output `(1,4+C,8400)` — so
swapping in a different/finetuned model (e.g. drone-vs-bird) needs **no code
change**, just a different `--model` file.

ONNX Runtime is found automatically (`brew install onnxruntime` on macOS; set
`CMAKE_PREFIX_PATH`/`ONNXRUNTIME_ROOT` on Linux/Jetson). If it isn't found, the
build still works and `--model` falls back to the motion detector.

> Note: `YOLO11x` is the heavy variant (~1 fps on CPU) — great for validating the
> pipeline, too slow for real-time. For deployment use a smaller variant
> (`n`/`s`), CoreML on macOS, or a TensorRT engine on the Jetson.

Model weights (`*.pt`, `*.onnx`) are gitignored; regenerate them with
`models/export_drone_model.sh`.

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
src/camera_macos.mm     AVFoundation capture (macOS)
src/detector.hpp      Detection + IDetector + CreateDetector()
src/detector_motion.cpp frame-diff detector (default)
src/detector_onnx.cpp   ONNX Runtime YOLO detector (--model), letterbox+NMS
models/export_drone_model.sh  fetch + export the ready-made drone ONNX model
src/tracker.{hpp,cpp} single-target select + EMA smoothing + (ex,ey)
src/display.hpp       Overlay + IDisplay + CreateDisplay()
src/display_windows.cpp GDI preview window
src/display_macos.mm    Cocoa preview window (macOS)
src/display_null.cpp    headless no-op display
src/main.cpp          pipeline + CLI
```
