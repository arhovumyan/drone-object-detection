#!/usr/bin/env bash
# Fetch a ready-made drone detector and export it to ONNX for the C++ pipeline.
#
# Model: doguilmak/Drone-Detection-YOLOv11x (single class "drone", MIT license).
# Output: models/drone_yolo11.onnx  (used via: drone_detect --model <that file>)
#
# Requires Python (3.11/3.12 recommended; very new Pythons may lack torch wheels).
# Usage:  bash models/export_drone_model.sh
set -euo pipefail
cd "$(dirname "$0")/.."

PT=models/drone_yolo11.pt
ONNX=models/drone_yolo11.onnx
URL=https://huggingface.co/doguilmak/Drone-Detection-YOLOv11x/resolve/main/weight/best.pt

# Isolated venv so we never touch the system Python.
VENV=${VENV:-.export-venv}
if [ ! -d "$VENV" ]; then
  echo ">> creating venv ($VENV)"
  python3 -m venv "$VENV"
fi
"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet ultralytics onnx onnxslim

if [ ! -f "$PT" ]; then
  echo ">> downloading weights"
  curl -L --fail -o "$PT" "$URL"
fi

echo ">> exporting to ONNX (imgsz=640)"
"$VENV/bin/python" - "$PT" <<'PY'
import sys
from ultralytics import YOLO
m = YOLO(sys.argv[1])
print("classes:", m.names)
m.export(format="onnx", imgsz=640, opset=12, simplify=True, dynamic=False)
PY

echo ">> done: $ONNX"
