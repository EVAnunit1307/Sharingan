#!/usr/bin/env python3
"""
Pi-side camera + person/object detection streamer for the drone dashboard.

Captures from the CSI camera module via picamera2, runs a lightweight
MobileNet-SSD object detector (falls back automatically to OpenCV's built-in
HOG person detector if the model files aren't present, so this still runs
with zero downloads), draws boxes/labels on the frame, and serves it as an
MJPEG stream that imu_viz.py's top pane (#holoPane) displays live.

Run on the Pi:
    python3 pi_camera_stream.py

Then on your PC, imu_viz.py's top pane will show it automatically at
http://larp-pi.local:8766/stream (as long as PI_STREAM_URL in imu_viz.py
matches your Pi's hostname).
"""
import time
import threading

import cv2
from flask import Flask, Response

from picamera2 import Picamera2

FRAME_W, FRAME_H = 640, 480
STREAM_PORT = 8766
DETECT_EVERY_N_FRAMES = 1   # bump to 2 or 3 if the Pi struggles to keep up

# MobileNet-SSD model files (optional — see setup instructions). If these
# aren't present, we automatically fall back to HOG people-detection so the
# stream still works with nothing extra installed.
PROTOTXT = "models/MobileNetSSD_deploy.prototxt"
MODEL = "models/MobileNetSSD_deploy.caffemodel"
CLASSES = [
    "background", "aeroplane", "bicycle", "bird", "boat", "bottle", "bus",
    "car", "cat", "chair", "cow", "diningtable", "dog", "horse", "motorbike",
    "person", "pottedplant", "sheep", "sofa", "train", "tvmonitor",
]

app = Flask(__name__)

picam2 = Picamera2()
config = picam2.create_video_configuration(main={"size": (FRAME_W, FRAME_H), "format": "RGB888"})
picam2.configure(config)
picam2.start()
time.sleep(1.0)  # let auto-exposure/white-balance settle

net = None
hog = None
try:
    net = cv2.dnn.readNetFromCaffe(PROTOTXT, MODEL)
    print("[detect] MobileNet-SSD loaded — general object detection (20 classes incl. person)")
except Exception as e:
    print(f"[detect] MobileNet-SSD not available ({e}) — falling back to HOG person detector")
    hog = cv2.HOGDescriptor()
    hog.setSVMDetector(cv2.HOGDescriptor_getDefaultPeopleDetector())

latest_jpeg = None
frame_lock = threading.Lock()


def detect_and_draw(frame):
    h, w = frame.shape[:2]
    if net is not None:
        blob = cv2.dnn.blobFromImage(frame, 0.007843, (300, 300), 127.5)
        net.setInput(blob)
        detections = net.forward()
        for i in range(detections.shape[2]):
            conf = float(detections[0, 0, i, 2])
            if conf < 0.5:
                continue
            idx = int(detections[0, 0, i, 1])
            box = detections[0, 0, i, 3:7] * [w, h, w, h]
            x1, y1, x2, y2 = box.astype(int)
            label = f"{CLASSES[idx]}: {conf*100:.0f}%"
            color = (140, 220, 60) if CLASSES[idx] == "person" else (60, 180, 255)
            cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
            cv2.putText(frame, label, (x1, max(15, y1 - 6)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1, cv2.LINE_AA)
    else:
        boxes, _ = hog.detectMultiScale(frame, winStride=(8, 8), padding=(4, 4), scale=1.05)
        for (x, y, bw, bh) in boxes:
            cv2.rectangle(frame, (x, y), (x + bw, y + bh), (140, 220, 60), 2)
            cv2.putText(frame, "person", (x, max(15, y - 6)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, (140, 220, 60), 1, cv2.LINE_AA)
    return frame


def capture_loop():
    global latest_jpeg
    frame_i = 0
    while True:
        try:
            frame = picam2.capture_array()               # RGB888
            frame = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)
            if frame_i % DETECT_EVERY_N_FRAMES == 0:
                frame = detect_and_draw(frame)
            cv2.putText(frame, time.strftime("%H:%M:%S"), (8, FRAME_H - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.45, (200, 200, 200), 1, cv2.LINE_AA)
            ok, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 75])
            if ok:
                with frame_lock:
                    latest_jpeg = jpeg.tobytes()
            frame_i += 1
        except Exception as e:
            print(f"[capture] error: {e}", flush=True)
            time.sleep(0.5)


threading.Thread(target=capture_loop, daemon=True).start()


def mjpeg_generator():
    while True:
        with frame_lock:
            jpeg = latest_jpeg
        if jpeg is not None:
            yield (b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + jpeg + b"\r\n")
        time.sleep(0.03)


@app.route("/stream")
def stream():
    return Response(mjpeg_generator(), mimetype="multipart/x-mixed-replace; boundary=frame")


@app.route("/")
def index():
    return '<html><body style="margin:0;background:#000"><img src="/stream" style="width:100%"></body></html>'


if __name__ == "__main__":
    print(f"Streaming at http://<this-pi>:{STREAM_PORT}/stream")
    app.run(host="0.0.0.0", port=STREAM_PORT, threaded=True)
