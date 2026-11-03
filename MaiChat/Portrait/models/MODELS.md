# Local portrait perception models

All four files are inference models derived from Google MediaPipe and distributed under
Apache-2.0. They locate facial landmarks, body pose and skin regions; image editing and
personal preference training are implemented separately by MaiChat. No identity recognition
or generative face replacement is performed by these models.

Run `python3 MaiChat/scripts/fetch-portrait-models.py` from the repository root. Binary model
files stay outside Git; `--check` verifies local files without accessing the network.

| File | Source / pinned revision | SHA-256 |
| --- | --- | --- |
| face_landmarks.onnx | [conversion](https://huggingface.co/naklitechie/face-landmarks-onnx), `575c33816c840c5b156398adb485e4f8d138adc2` | `f38c3321ceffbc9e95103480ad38cc3f52e7e1bde2bcee7cd9355d0b9138ac0c` |
| selfie_multiclass.onnx | [conversion](https://huggingface.co/senty-au/selfie_multiclass_256x256-ONNX), `6db8421a7150ac20558f2c24675078eb3a1a04d0` | `35ec1ecd9ee7f85073c99c00020b7f6751b69506eeacf683bc8665f6117f85b0` |
| person_detection.onnx | [OpenCV Zoo](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/person_detection_mediapipe) | `47fd5599d6fa17608f03e0eb0ae230baa6e597d7e8a2c8199fe00abea55a701f` |
| pose_estimation.onnx | [OpenCV Zoo](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/pose_estimation_mediapipe) | `9d89c599319a18fb7d2e28451a883476164543182bafca5f09eb2cf767ed2f3f` |

The face model requires an aligned 256x256 RGB crop in [0,1] and predicts 478 3D points.
The six-class segmentation model takes a stretched 256x256 RGB image in [0,1] and returns
logits. The person detector uses padded 224x224 CHW RGB in [-1,1]; the pose model consumes an
aligned 256x256 RGB crop and returns 33 body plus six auxiliary landmarks. Preprocessing,
score interpretation, anchors and inverse coordinate transforms are part of the C++ engine.

Original model documentation: [face](https://ai.google.dev/edge/mediapipe/solutions/vision/face_landmarker),
[segmentation](https://ai.google.dev/edge/mediapipe/solutions/vision/image_segmenter),
[pose](https://ai.google.dev/edge/mediapipe/solutions/vision/pose_landmarker).
