// Fallback CreateOnnxDetector for builds without ONNX Runtime (e.g. a platform
// where ORT wasn't found at configure time). Keeps the link working and gives a
// clear message instead of a missing symbol. The real implementation lives in
// detector_onnx.cpp and is compiled when CMake finds ONNX Runtime.

#include "detector.hpp"
#include <iostream>

namespace dd {

std::unique_ptr<IDetector> CreateOnnxDetector(const std::string& modelPath, float) {
    std::cerr << "[onnx] This build has no ONNX Runtime support; cannot load '"
              << modelPath << "'. Falling back to the motion detector.\n";
    return nullptr;
}

} // namespace dd
