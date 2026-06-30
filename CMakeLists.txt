cmake_minimum_required(VERSION 3.20)
project(drone_detect LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Cross-platform sources ----
add_executable(drone_detect
  src/main.cpp
  src/detector_motion.cpp
  src/tracker.cpp
)
target_include_directories(drone_detect PRIVATE src)

# ---- ONNX Runtime (optional) -------------------------------------------
# Enables the YOLO drone detector (--model). On macOS this is `brew install
# onnxruntime`; on Linux/Jetson point CMAKE_PREFIX_PATH at an ORT install.
# When not found we compile a stub so --model degrades gracefully to motion.
set(_ort_hint "")
if(APPLE)
  execute_process(COMMAND brew --prefix onnxruntime
    OUTPUT_VARIABLE _ort_hint OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h
  HINTS "${_ort_hint}/include" ENV ONNXRUNTIME_ROOT
  PATH_SUFFIXES include include/onnxruntime onnxruntime)
find_library(ORT_LIB onnxruntime
  HINTS "${_ort_hint}/lib" ENV ONNXRUNTIME_ROOT
  PATH_SUFFIXES lib)

if(ORT_INCLUDE_DIR AND ORT_LIB)
  message(STATUS "[drone_detect] ONNX Runtime found -> YOLO detector enabled")
  target_sources(drone_detect PRIVATE src/detector_onnx.cpp)
  target_include_directories(drone_detect PRIVATE "${ORT_INCLUDE_DIR}")
  target_link_libraries(drone_detect PRIVATE "${ORT_LIB}")
  target_compile_definitions(drone_detect PRIVATE DD_HAVE_ONNX)
else()
  message(STATUS "[drone_detect] ONNX Runtime not found -> YOLO detector disabled (motion only)")
  target_sources(drone_detect PRIVATE src/detector_onnx_stub.cpp)
endif()

# ---- Platform selection -------------------------------------------------
if(WIN32)
  message(STATUS "[drone_detect] Target: Windows  (built-in camera via Media Foundation)")
  target_sources(drone_detect PRIVATE
    src/camera_windows.cpp
    src/display_windows.cpp
  )
  target_compile_definitions(drone_detect PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(drone_detect PRIVATE
    mfplat mf mfreadwrite mfuuid ole32 Cfgmgr32 Gdi32 User32)

elseif(APPLE)
  message(STATUS "[drone_detect] Target: macOS  (AVFoundation camera, Cocoa preview)")
  target_sources(drone_detect PRIVATE
    src/camera_macos.mm
    src/display_macos.mm
  )
  # AVFoundation/Cocoa backends are Objective-C++ (.mm); ARC stays off so the
  # backends manage retain/release explicitly. Disable it defensively in case a
  # toolchain default flips it on.
  set_source_files_properties(src/camera_macos.mm src/display_macos.mm
    PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
  target_link_libraries(drone_detect PRIVATE
    "-framework AVFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework CoreGraphics"
    "-framework Cocoa"
    "-framework Foundation")

elseif(UNIX)
  message(STATUS "[drone_detect] Target: Linux/Jetson  (V4L2 camera, headless)")
  target_sources(drone_detect PRIVATE
    src/camera_linux.cpp
    src/display_null.cpp
  )
  target_link_libraries(drone_detect PRIVATE pthread)

  # Detect a Jetson board at configure time and flag it.
  if(EXISTS "/proc/device-tree/model")
    file(READ "/proc/device-tree/model" _dd_model)
    if(_dd_model MATCHES "Jetson|NVIDIA")
      message(STATUS "[drone_detect] NVIDIA Jetson detected -> DD_JETSON")
      target_compile_definitions(drone_detect PRIVATE DD_JETSON)
      # TODO (on-device): find_package(TensorRT) / CUDA and add Yolo26 detector + DeepStream/GStreamer capture.
    endif()
  endif()
endif()
