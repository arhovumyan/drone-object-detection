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
