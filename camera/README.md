# Camera layer (computer vision)

Watches the webcam, computes a **flame score 0..1** and POSTs it with a small annotated frame to the server
(`/api/cv`, signed with HMAC-SHA256). The camera can only push EMBER to **WARNING**; ALARM needs the sensors to agree
(fusion runs on the ESP32).

## Setup
```
pip install opencv-python numpy requests matplotlib
```
`EMBER_KEY` and `SERVER_IP` come from the team chat. Pass them as environment variables, never write them in a file.

## Run (from any folder)
```
EMBER_KEY=... SERVER_IP=... python camera/cv_flame.py --yolo     # live camera, YOLO score, send to server
python camera/cv_flame.py --yolo --no-send                       # local only
python camera/cv_flame.py --tune                                 # colour-mask trackbars (no YOLO)
```
Keys: `q` quit, `s` print the mask parameters.

## The score
| Mode | Score |
|---|---|
| default | colour mask + flicker (hand-tuned) |
| `--yolo` | best fire confidence of YOLOv8n, smoothed (EMA 0.3), flame when >= 0.5 in 3 of the last 5 frames |
| `--model cv_model.joblib` | logistic regression from `cv_train.py` |

The YOLO model is `fire_yolov8n.onnx`, a fine-tune of YOLOv8n on the D-Fire dataset
(<https://huggingface.co/rabahdev/fire-smoke-yolov8n>, licence **AGPL-3.0**), exported to ONNX and run with
`cv2.dnn`, so torch is not needed at runtime. About 27 fps on a laptop CPU.

## Test the camera model (confusion matrix)
```
python camera/cv_record.py --yolo   # f = flame clip, n = no-flame clip, space = stop, q = quit  -> data/cv_yolo_data.csv
python camera/cv_eval.py            # accuracy / precision / recall / per clip  -> results/cv_yolo_confusion.png
```
A window is 10 frames. YOLO calls a window "flame" when at least 3 of its frames show fire.

## Results so far (749 windows, 10 clips: 5 flame, 5 no-flame; one phone, one room)
| | accuracy | precision | recall |
|---|---|---|---|
| YOLO | 0.86 | 0.80 | 0.90 |
| colour + flicker | 0.74 | 0.63 | 1.00 |

Known weak spot: orange moving scenes (a rotated sunset video caused 64 of the 74 false alarms). The sample is small
(10 clips); windows from one clip are near copies.

## Files
- `cv_flame.py` live detector and sender. `cv_record.py` records labelled clips. `cv_eval.py` scores YOLO against the colour rule.
- `cv_train.py` trains the small logistic-regression model on colour/flicker features (`cv_record.py` without `--yolo`).
- `data/` recorded windows. `results/` confusion matrix PNG.
