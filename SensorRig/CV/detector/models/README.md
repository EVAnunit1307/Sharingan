# Selected model and provenance

`yolox_nano.onnx` is the unmodified YOLOX nano ONNX export from
[Megvii's official 0.1.1rc0 release](https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_nano.onnx),
linked by the [upstream ONNX Runtime example](https://github.com/Megvii-BaseDetection/YOLOX/blob/main/demo/ONNXRuntime/README.md).

A fresh download was compared to the deployed file on 2026-09-19 and matched:

```text
SHA-256 c789161ed43c8269fcd4e67c67eeeb4e80c622da2eb296a20bc6007bd18a0b7d
```

Input: batch 1, three channels, 416×416, BGR 0–255 with aspect-preserving padding.
The runtime selects the person class. Weights were not retrained during this work;
thresholds, capture/preprocessing and temporal tracking were adjusted.

The model is small enough to be stored directly in Git (about 3.5 MB); it is not
an LFS pointer. Other names in the detector registry are historical comparisons
and do not imply that their model files are included.

The upstream project is distributed under Apache License 2.0. Its license is
preserved as [LICENSE-YOLOX.txt](LICENSE-YOLOX.txt), obtained from the
[upstream license](https://github.com/Megvii-BaseDetection/YOLOX/blob/main/LICENSE).
