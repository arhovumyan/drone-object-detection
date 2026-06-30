# Real-Time Drone Detection & Tracking — Project Plan

**Scope of this document:** A real-time computer-vision system that detects a drone in a
camera feed, tracks it frame-to-frame, and outputs *where the target is on screen* plus a
*normalized error from image center*. Target platform: NVIDIA Jetson Orin Nano. Language:
C++. Cross-platform (Linux + Windows) with runtime OS detection.

> **Boundary note.** This system is a **perception + tracking** module. Its output is a
> clean target signal (bounding box, centroid, normalized x/y error from center). It can
> drive a **camera gimbal** to keep a subject centered (surveillance / cinematography
> pattern). It deliberately does **not** include guidance/pursuit logic for a flying
> vehicle's flight controller. Whatever consumes the error signal downstream is a separate,
> independently-authorized concern and is out of scope here.

---

## 1. What the system does (data flow)

```
Camera ──> Capture ──> Pre-process ──> YOLO26 (TensorRT) ──> Detections
                                                                │
                                                                ▼
                                                          Tracker (pick + hold ID)
                                                                │
                                                                ▼
                                              Target centroid (cx, cy) + bbox
                                                                │
                                                                ▼
                              Normalized error  ex = (cx - W/2)/(W/2),  ey = (cy - H/2)/(H/2)
                                                                │
                                                                ▼
                              Output: log / UDP-JSON / optional gimbal (pan-tilt) command
```

The whole point is to get `(ex, ey)` in range `[-1, 1]` at high frame rate, where `(0,0)`
means the target is dead-center. Everything else is plumbing.

---

## 2. Hardware

### Compute
- **NVIDIA Jetson Orin Nano (8 GB) — the "Super" Developer Kit** (launched Dec 2024). The
  Super firmware/power mode raises GPU clocks and gives ~67 TOPS (INT8), a big jump over the
  original Orin Nano. Make sure JetPack is updated so the Super power mode is available.
- Run in **MAXN / 25W power mode** for max FPS; have a fan/heatsink — sustained inference
  will thermal-throttle a passively cooled board.
- Boot from **NVMe SSD**, not microSD. The SD card will bottleneck model loading and logging.

### Camera (pick based on how you'll mount it)
- **Global-shutter** strongly preferred over rolling-shutter. A fast-moving platform +
  rolling shutter = skewed/jello frames that wreck small-object detection.
- **CSI camera** (e.g. IMX296 global shutter) → lowest latency via Jetson's hardware ISP
  (Argus/`nvarguscamerasrc`).
- **USB/GS camera** → simpler, more portable to the Windows dev box, slightly higher latency.
- Wide FOV finds the target faster; narrow FOV tracks it more precisely at distance. A
  ~60–90° lens is a reasonable starting compromise.
- Frame rate: aim for a camera that does **≥60 FPS** at your chosen resolution so the camera
  isn't the limiter.

### Optional actuation (in scope)
- A **2-axis pan/tilt gimbal** (serial or PWM servo controller) if you want the camera to
  physically center the target. This is the legitimate "keep subject in frame" use.

---

## 3. Software stack

| Layer | Choice | Why |
|---|---|---|
| Model | **YOLO26** (nano/small scale) | Newest Ultralytics model (Sept 2025). **NMS-free + end-to-end** → no NMS code in C++. Edge-optimized; 40.9–57.5 mAP COCO. |
| Inference | **NVIDIA TensorRT** (FP16, then INT8) | The fastest path on Jetson. Layer fusion + precision calibration. Community C++ YOLO+TensorRT impls report single-digit-ms inference on Orin. |
| Camera / video | **GStreamer** (via OpenCV `VideoCapture` with the GStreamer backend) | One API that maps to `nvarguscamerasrc` (CSI, Linux) and to USB/`mfvideosrc` (Windows). HW-accelerated decode on Jetson. |
| Image ops | **OpenCV 4.x** (CUDA build on Jetson) | Resize/letterbox/color-convert, drawing overlays, capture fallback. |
| Tracking | **ByteTrack**-style or a lightweight Kalman + IoU tracker (C++) | Holds a stable target ID across frames; smooths jitter; bridges missed detections. |
| Build | **CMake** (single tree, both OSes) | Detects platform and links the right deps. |
| Config | small **YAML/JSON** file | Model path, thresholds, camera URI, output mode — no recompiling to retune. |
| IPC / output | **UDP JSON** (or shared memory) | Decouples perception from any consumer. Easy to test with `nc`/a Python listener. |

### Why YOLO26 specifically for C++ on edge
The NMS-free design matters more than it sounds. With older YOLO you ship and tune a
non-max-suppression pass in C++ (IoU loops, score thresholds, per-class handling). YOLO26's
end-to-end head emits final detections directly, so the post-processing you maintain shrinks
to "decode tensor → boxes," which is less code and less latency.

---

## 4. Cross-platform strategy (Linux + Windows, with OS detection)

Two layers of "OS awareness":

**A. Compile-time** (the build) — CMake picks dependencies and the capture backend:
```cmake
if(WIN32)
    # Windows: USB camera via MediaFoundation; TensorRT from CUDA toolkit install
elseif(UNIX)
    # Linux/Jetson: CSI via nvarguscamerasrc; TensorRT from JetPack
endif()
```

**B. Run-time** (the binary) — a thin `Platform` abstraction chooses the camera pipeline
string and any OS-specific paths at startup:
```cpp
#if defined(_WIN32)
    constexpr Platform kOS = Platform::Windows;
#elif defined(__linux__)
    constexpr Platform kOS = Platform::Linux;
#endif

std::string CameraPipeline() {
    if constexpr (kOS == Platform::Linux)   // Jetson CSI, HW ISP
        return "nvarguscamerasrc ! video/x-raw(memory:NVMM),width=1280,height=720,"
               "framerate=60/1 ! nvvidconv ! video/x-raw,format=BGRx ! videoconvert ! "
               "video/x-raw,format=BGR ! appsink drop=1 max-buffers=1";
    else                                     // Windows dev box, USB cam
        return "0";  // OpenCV device index / MediaFoundation
}
```

**Practical division of labor:** develop and debug the detector/tracker on **Windows with a
USB webcam** (fast iteration, full IDE), then deploy the *same* C++ source to the **Jetson**
where it builds against JetPack's TensorRT and the CSI pipeline. The OS abstraction is the
only thing that differs.

> Reality check: TensorRT engine files are **not portable** across different GPUs/TensorRT
> versions. You build the `.onnx` once, but you must run the TensorRT *engine build step* on
> each target machine (the Windows dev GPU and the Jetson separately). Plan for a small
> "build engine on first run if missing" step.

---

## 5. Pipeline implementation notes

1. **Capture** — open via the platform pipeline string. Use `drop=1 max-buffers=1` so you
   always process the *latest* frame, never a stale backlog. Latency > throughput here.
2. **Pre-process** — letterbox to the model's input size (e.g. 640×640), normalize, HWC→CHW.
   Do this on GPU (OpenCV CUDA or a CUDA kernel) to avoid a CPU bottleneck.
3. **Inference** — TensorRT execution context, FP16 first. Bind input/output device buffers
   once and reuse; don't malloc per frame.
4. **Decode** — convert YOLO26 output tensor to boxes (no NMS needed). Filter by confidence
   and, optionally, by a "drone" class if you've trained/fine-tuned for it.
5. **Track** — feed boxes to the tracker; select the target (highest confidence, or closest
   to last known position, or largest). Maintain its ID; coast through brief dropouts via the
   Kalman prediction.
6. **Error signal** — compute `(ex, ey)` from the tracked centroid. Optionally low-pass
   filter to avoid twitchy output.
7. **Output** — emit JSON `{ "ts", "found", "cx","cy","w","h","ex","ey","conf" }` over UDP,
   and/or send a pan/tilt command to the gimbal, and/or draw an overlay window for debugging.

---

## 6. Model: getting a *drone* detector

Out-of-the-box COCO YOLO26 doesn't have a "drone" class. Options, in order of effort:

1. **Quick start:** use a pretrained drone/UAV YOLO weight set (several public datasets exist
   — e.g. "Drone-vs-Bird", anti-UAV datasets) and export to ONNX → TensorRT.
2. **Better:** fine-tune YOLO26-n on a drone dataset (Ultralytics training in Python, then
   export). Small models fine-tune fast and run faster on the Orin.
3. **Best for your camera:** collect footage from *your actual camera/lens at realistic
   ranges* (drones are small, distant, low-contrast against sky) and fine-tune on that. Small
   far-away objects are the hard case; STAL in YOLO26 helps small-object recall, but matched
   training data helps more.

Export path: `Ultralytics .pt → ONNX → TensorRT engine` (FP16, then calibrate INT8 if you
need more FPS and can spare a little accuracy).

---

## 7. Performance targets & tactics

- **Goal:** ≥30 FPS end-to-end at the camera resolution, with detection latency in the low
  single-digit to ~10 ms range (achievable with a nano/small YOLO + TensorRT FP16 on Orin).
- Levers if you're short on FPS: smaller model scale (n vs s), smaller input (640→512/416),
  INT8 quantization, GPU pre-processing, MAXN power mode, drop-to-latest frame policy.
- **Measure** three numbers separately: capture latency, inference latency, total
  frame-to-output latency. Optimize the biggest one, not the one that's easiest.

---

## 8. Suggested repo layout

```
objectDetection/
├─ CMakeLists.txt            # platform detection, deps
├─ config/
│   └─ default.yaml          # model path, thresholds, camera, output mode
├─ models/
│   ├─ drone_yolo26n.onnx
│   └─ drone_yolo26n.engine  # built per-machine, git-ignored
├─ src/
│   ├─ main.cpp
│   ├─ platform.hpp          # OS detection + camera pipeline
│   ├─ capture.{hpp,cpp}     # GStreamer/OpenCV capture
│   ├─ detector.{hpp,cpp}    # TensorRT YOLO26 wrapper
│   ├─ tracker.{hpp,cpp}     # Kalman+IoU / ByteTrack-style
│   ├─ target.{hpp,cpp}      # select target, compute (ex,ey)
│   └─ output.{hpp,cpp}      # UDP JSON / gimbal / overlay
└─ tools/
    └─ export_engine.*       # onnx -> tensorrt engine builder
```

---

## 9. Build order (milestones)

1. **M0 – Skeleton:** CMake builds on both OSes; OS detection prints correct platform; open
   camera and show frames (USB on Windows, CSI on Jetson).
2. **M1 – Inference:** load a generic YOLO26 TensorRT engine, run on frames, draw boxes.
3. **M2 – Drone model:** swap in the fine-tuned drone detector.
4. **M3 – Tracking:** add tracker; stable single-target selection + ID hold.
5. **M4 – Error signal:** compute and emit `(ex, ey)`; visualize crosshair + target marker.
6. **M5 – Output/gimbal:** UDP JSON out; optional pan/tilt gimbal centering loop.
7. **M6 – Optimize:** INT8, latency budget, sustained-thermals test on Orin.

---

## Sources
- [Ultralytics YOLO26 — Docs](https://docs.ultralytics.com/models/yolo26)
- [YOLO26: Architectural Enhancements & Benchmarking (arXiv 2509.25164)](https://arxiv.org/abs/2509.25164)
- [Ultralytics YOLO Evolution: YOLO26/11/v8/v5 (arXiv 2510.09653)](https://arxiv.org/abs/2510.09653)
- [YOLO26 on NVIDIA Jetson — Setup & Benchmarks (Ultralytics Docs)](https://docs.ultralytics.com/guides/nvidia-jetson)
- [YOLO26 on Jetson: DeepStream & TensorRT (Ultralytics Docs)](https://docs.ultralytics.com/guides/deepstream-nvidia-jetson)
- [Pushing Limits: YOLOv8 vs v26 on Jetson Orin Nano (Hackster)](https://www.hackster.io/qwe018931/pushing-limits-yolov8-vs-v26-on-jetson-orin-nano-b89267)
- [YoloV8 TensorRT C++ on Jetson (Qengineering, GitHub)](https://github.com/Qengineering/YoloV8-TensorRT-Jetson_Nano)
