# Shared YOLOX person detector

See [the camera README](../README.md) for running, evaluation, and limitations.

`PersonDetector.detect(frame, timestamp=...)` expects an upright BGR image and a
strictly increasing monotonic capture timestamp. Results include `id`, `box` in
xyxy pixels, `score`, `observed`, `misses`, and `age_ms`. Render observed people and
held tracks differently. Never count a held box as fresh model evidence.

`update(detections, timestamp=...)` runs the identical tracking stage on cached
observations. `reset()` separates independent clips or camera connections. Model
inference uses ONNX Runtime CPU with thread spinning disabled to leave capacity
for capture and streaming. Preprocessing follows the model registry; do not
normalize official YOLOX inputs to 0–1 or swap BGR channels.

Thresholds are configuration, not learned weights. The development evaluation
cannot establish new-room, two-person, or small/distant-person accuracy. The
saved clips do not contain box labels, so the reported recall is presence recall,
not localization mAP. Keep any future hold-out recordings separate from tuning.
