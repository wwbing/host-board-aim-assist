import argparse
from pathlib import Path

import cv2
import numpy as np
from rknn.api import RKNN


OBJ_THRESH = 0.25
NMS_THRESH = 0.45
IMG_SIZE = (640, 640)
CLASSES = ("Head",)
IMG_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp"}


def letter_box(image, new_shape=(640, 640), pad_color=(0, 0, 0)):
    shape = image.shape[:2]
    if isinstance(new_shape, int):
        new_shape = (new_shape, new_shape)

    ratio = min(new_shape[1] / shape[0], new_shape[0] / shape[1])
    new_unpad = (int(round(shape[1] * ratio)), int(round(shape[0] * ratio)))
    dw = new_shape[0] - new_unpad[0]
    dh = new_shape[1] - new_unpad[1]
    dw /= 2
    dh /= 2

    if shape[::-1] != new_unpad:
        image = cv2.resize(image, new_unpad, interpolation=cv2.INTER_LINEAR)

    top = int(round(dh - 0.1))
    bottom = int(round(dh + 0.1))
    left = int(round(dw - 0.1))
    right = int(round(dw + 0.1))
    image = cv2.copyMakeBorder(image, top, bottom, left, right, cv2.BORDER_CONSTANT, value=pad_color)
    return image, ratio, (dw, dh)


def clip_boxes(boxes, shape):
    h, w = shape[:2]
    boxes[:, [0, 2]] = boxes[:, [0, 2]].clip(0, w)
    boxes[:, [1, 3]] = boxes[:, [1, 3]].clip(0, h)
    return boxes


def scale_boxes(boxes, ratio, dwdh, original_shape):
    scaled = boxes.copy()
    dw, dh = dwdh
    scaled[:, [0, 2]] -= dw
    scaled[:, [1, 3]] -= dh
    scaled[:, :4] /= ratio
    return clip_boxes(scaled, original_shape)


def filter_boxes(boxes, box_confidences, box_class_probs):
    box_confidences = box_confidences.reshape(-1)
    class_max_score = np.max(box_class_probs, axis=-1)
    classes = np.argmax(box_class_probs, axis=-1)

    keep = np.where(class_max_score * box_confidences >= OBJ_THRESH)
    scores = (class_max_score * box_confidences)[keep]
    boxes = boxes[keep]
    classes = classes[keep]
    return boxes, classes, scores


def nms_boxes(boxes, scores):
    x1 = boxes[:, 0]
    y1 = boxes[:, 1]
    x2 = boxes[:, 2]
    y2 = boxes[:, 3]

    areas = (x2 - x1) * (y2 - y1)
    order = scores.argsort()[::-1]
    keep = []

    while order.size > 0:
        i = order[0]
        keep.append(i)

        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])

        inter_w = np.maximum(0.0, xx2 - xx1)
        inter_h = np.maximum(0.0, yy2 - yy1)
        inter = inter_w * inter_h

        ovr = inter / (areas[i] + areas[order[1:]] - inter + 1e-6)
        inds = np.where(ovr <= NMS_THRESH)[0]
        order = order[inds + 1]

    return np.array(keep)


def dfl(position):
    n, c, h, w = position.shape
    p_num = 4
    mc = c // p_num
    y = position.reshape(n, p_num, mc, h, w)
    y = y - np.max(y, axis=2, keepdims=True)
    y = np.exp(y)
    y = y / np.sum(y, axis=2, keepdims=True)
    acc = np.arange(mc, dtype=np.float32).reshape(1, 1, mc, 1, 1)
    return (y * acc).sum(axis=2)


def box_process(position):
    grid_h, grid_w = position.shape[2:4]
    col, row = np.meshgrid(np.arange(grid_w), np.arange(grid_h))
    col = col.reshape(1, 1, grid_h, grid_w)
    row = row.reshape(1, 1, grid_h, grid_w)
    grid = np.concatenate((col, row), axis=1)
    stride = np.array([IMG_SIZE[1] // grid_h, IMG_SIZE[0] // grid_w], dtype=np.float32).reshape(1, 2, 1, 1)

    if position.shape[1] == 4:
        box_xy = grid + 0.5 - position[:, 0:2, :, :]
        box_xy2 = grid + 0.5 + position[:, 2:4, :, :]
    else:
        position = dfl(position)
        box_xy = grid + 0.5 - position[:, 0:2, :, :]
        box_xy2 = grid + 0.5 + position[:, 2:4, :, :]

    return np.concatenate((box_xy * stride, box_xy2 * stride), axis=1)


def _to_nchw(output):
    output = np.asarray(output)
    if output.ndim == 3:
        output = np.expand_dims(output, 0)
    if output.ndim != 4:
        raise ValueError(f"Unsupported output ndim: {output.ndim}, shape={output.shape}")
    if output.shape[1] == output.shape[2] and output.shape[2] != output.shape[3]:
        return output.transpose(0, 3, 1, 2)
    return output


def post_process(outputs):
    if len(outputs) == 1:
        out = np.asarray(outputs[0])
        if out.ndim == 3 and out.shape[1] == 5:
            rows = out[0].transpose(1, 0).astype(np.float32)
        elif out.ndim == 3 and out.shape[2] == 5:
            rows = out[0].astype(np.float32)
        else:
            raise ValueError(f"Unsupported output shape: {out.shape}")

        boxes = rows[:, :4]
        scores = rows[:, 4]
        classes = np.zeros(scores.shape[0], dtype=np.int32)

        keep = scores >= OBJ_THRESH
        boxes = boxes[keep]
        scores = scores[keep]
        classes = classes[keep]
        if boxes.size == 0:
            return None, None, None

        boxes_xyxy = boxes.copy()
        boxes_xyxy[:, 0] = boxes[:, 0] - boxes[:, 2] / 2.0
        boxes_xyxy[:, 1] = boxes[:, 1] - boxes[:, 3] / 2.0
        boxes_xyxy[:, 2] = boxes[:, 0] + boxes[:, 2] / 2.0
        boxes_xyxy[:, 3] = boxes[:, 1] + boxes[:, 3] / 2.0

        keep = nms_boxes(boxes_xyxy, scores)
        if len(keep) == 0:
            return None, None, None
        return boxes_xyxy[keep], classes[keep], scores[keep]

    outputs = [_to_nchw(output) for output in outputs]
    branch_num = 3
    pair_per_branch = len(outputs) // branch_num
    if pair_per_branch * branch_num != len(outputs):
        raise ValueError(f"Unexpected output count: {len(outputs)}, shapes={[np.asarray(o).shape for o in outputs]}")

    boxes, scores, classes_conf = [], [], []
    for i in range(branch_num):
        boxes.append(box_process(outputs[pair_per_branch * i]))
        classes_conf.append(outputs[pair_per_branch * i + 1])
        scores.append(np.ones_like(outputs[pair_per_branch * i + 1][:, :1, :, :], dtype=np.float32))

    def sp_flatten(data):
        ch = data.shape[1]
        data = data.transpose(0, 2, 3, 1)
        return data.reshape(-1, ch)

    boxes = np.concatenate([sp_flatten(v) for v in boxes], axis=0)
    classes_conf = np.concatenate([sp_flatten(v) for v in classes_conf], axis=0)
    scores = np.concatenate([sp_flatten(v) for v in scores], axis=0)

    boxes, classes, scores = filter_boxes(boxes, scores, classes_conf)
    if boxes.size == 0:
        return None, None, None

    nboxes, nclasses, nscores = [], [], []
    for cls in set(classes.tolist()):
        inds = np.where(classes == cls)
        b = boxes[inds]
        c = classes[inds]
        s = scores[inds]
        keep = nms_boxes(b, s)
        if len(keep) != 0:
            nboxes.append(b[keep])
            nclasses.append(c[keep])
            nscores.append(s[keep])

    if not nboxes:
        return None, None, None

    return np.concatenate(nboxes), np.concatenate(nclasses), np.concatenate(nscores)


def summarize_raw_outputs(outputs):
    if len(outputs) == 1:
        out = np.asarray(outputs[0])
        print("dtype:", out.dtype, "shape:", out.shape)
        print("min/max:", float(out.min()), float(out.max()))

        score = None
        if out.ndim == 3 and out.shape[1] > 4:
            score = out[0, 4, :]
        elif out.ndim == 3 and out.shape[2] > 4:
            score = out[0, :, 4]

        if score is None:
            print("score channel sample: unsupported shape")
            return None

        print("score channel sample:", score[:20])
        print("score channel min/max:", float(score.min()), float(score.max()))
        topk = np.argsort(score)[-5:][::-1]
        print("score top5:", [(int(i), float(score[i])) for i in topk])
        return float(score.max())

    print(f"output_count: {len(outputs)}")
    branch_num = 3
    pair_per_branch = len(outputs) // branch_num
    cls_max_values = []
    for i in range(branch_num):
        box_out = np.asarray(outputs[pair_per_branch * i])
        cls_out = np.asarray(outputs[pair_per_branch * i + 1])
        print(f"branch[{i}] box dtype/shape: {box_out.dtype} {box_out.shape} min/max: {float(box_out.min())} {float(box_out.max())}")
        print(f"branch[{i}] cls dtype/shape: {cls_out.dtype} {cls_out.shape} min/max: {float(cls_out.min())} {float(cls_out.max())}")
        cls_sample = cls_out.reshape(-1)[:20]
        print(f"branch[{i}] cls sample: {cls_sample}")
        cls_max_values.append(float(cls_out.max()))

    raw_max = max(cls_max_values) if cls_max_values else None
    print("raw cls max:", raw_max)
    return raw_max


def draw(image, boxes, scores, classes):
    for box, score, cls in zip(boxes, scores, classes):
        x1, y1, x2, y2 = [int(v) for v in box]
        label = f"{CLASSES[int(cls)]} {score:.3f}"
        print(f"{label} @ ({x1}, {y1}, {x2}, {y2})")
        cv2.rectangle(image, (x1, y1), (x2, y2), (0, 255, 0), 2)
        cv2.putText(image, label, (x1, max(0, y1 - 8)), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 255), 2)


def parse_args():
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="YOLOv6 RKNN inference for single-class Head detection")
    parser.add_argument("--model_path", type=Path, default=script_dir / "model" / "v6n_cs2_head_rk3588_i8_normal_layer_channel.rknn")
    parser.add_argument("--img_dir", type=Path, default=script_dir / "images")
    parser.add_argument("--output_dir", type=Path, default=script_dir / "result")
    parser.add_argument("--img_show", action="store_true", help="Show images with detection results")
    parser.add_argument("--img_save", action="store_true", default=True, help="Save images with detection results")
    parser.add_argument("--no_img_save", action="store_true", help="Disable saving result images")
    parser.add_argument("--eval_perf_each", action="store_true", help="Run eval_perf for every image instead of only the first image")
    parser.add_argument("--skip_eval_perf", action="store_true", help="Skip eval_perf to focus on raw output debugging")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.no_img_save:
        args.img_save = False

    model_path = args.model_path.resolve()
    img_dir = args.img_dir.resolve()
    output_dir = args.output_dir.resolve()

    if not model_path.exists():
        raise FileNotFoundError(f"Model not found: {model_path}")
    if not img_dir.exists():
        raise FileNotFoundError(f"Image directory not found: {img_dir}")

    image_paths = sorted(
        path for path in img_dir.iterdir()
        if path.is_file() and path.suffix.lower() in IMG_SUFFIXES
    )
    if not image_paths:
        raise FileNotFoundError(f"No images found in: {img_dir}")

    if args.img_save:
        output_dir.mkdir(parents=True, exist_ok=True)

    rknn = RKNN(verbose=True)
    try:
        print(f"--> Load RKNN model: {model_path}")
        ret = rknn.load_rknn(str(model_path))
        if ret != 0:
            raise RuntimeError(f"load_rknn failed, ret={ret}")

        print("--> Init runtime environment")
        ret = rknn.init_runtime(target='rk3588', perf_debug=True)
        if ret != 0:
            raise RuntimeError(f"init_runtime failed, ret={ret}")

        perf_done = False
        model_success = False
        for index, image_path in enumerate(image_paths, start=1):
            print(f"\n--> [{index}/{len(image_paths)}] {image_path.name}")
            img_src = cv2.imread(str(image_path))
            if img_src is None:
                print(f"Skip unreadable image: {image_path}")
                continue

            img, ratio, dwdh = letter_box(img_src.copy(), new_shape=IMG_SIZE, pad_color=(0, 0, 0))
            img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)

            if not args.skip_eval_perf and (args.eval_perf_each or not perf_done):
                print("--> Eval perf")
                rknn.eval_perf(is_print=True)
                perf_done = True

            print("--> Inference")
            outputs = rknn.inference(inputs=[img], data_format=['nhwc'])
            raw_score_max = summarize_raw_outputs(outputs)
            image_success = raw_score_max is not None and raw_score_max > 0
            print(f"RAW_STATUS: {'SUCCESS' if image_success else 'FAILURE'}")

            boxes, classes, scores = post_process(outputs)

            vis_img = img_src.copy()
            if boxes is not None:
                real_boxes = scale_boxes(boxes, ratio, dwdh, img_src.shape)
                draw(vis_img, real_boxes, scores, classes)
                model_success = True
            else:
                print("No detections.")

            if image_success:
                model_success = True

            if args.img_save:
                save_path = output_dir / image_path.name
                cv2.imwrite(str(save_path), vis_img)
                print(f"Saved: {save_path}")

            if args.img_show:
                cv2.imshow("yolov6_rknn_infer", vis_img)
                cv2.waitKeyEx(0)

        print(f"\nMODEL_STATUS: {'SUCCESS' if model_success else 'FAILURE'}")
    finally:
        rknn.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
