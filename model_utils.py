import os
import uuid
from PIL import Image, ImageDraw, ImageFont
from ultralytics import YOLO
import csv
from datetime import datetime

# ============================
# MODEL LOADING UTILITY
# ============================
def load_model(model_path="model/best_model.pt", class_names=None):
    """
    Load YOLO model as a reusable bundle.

    Args:
        model_path (str): Path to the YOLO .pt model file.
        class_names (dict): Optional dict mapping class indices to names.

    Returns:
        dict: Bundle containing 'framework', 'model', and 'names'.
    """
    if not os.path.exists(model_path):
        raise FileNotFoundError(f"❌ Model file not found at {model_path}")
    
    model = YOLO(model_path)
    names = class_names or {0: "cataract", 1: "normal"}
    
    print(f"✅ YOLO model loaded from {model_path}")
    print(f"🧠 Classes: {names}")
    
    return {"framework": "yolo", "model": model, "names": names}


# ============================
# RUN INFERENCE UTILITY
# ============================
def run_inference(model_bundle, image_path, imgsz=640, conf_thresh=0.25, save_log=True):
    """
    Run inference on an image using YOLO bundle.

    Args:
        model_bundle (dict): YOLO bundle from load_model().
        image_path (str): Path to the input image.
        imgsz (int): Resize image to this size for inference.
        conf_thresh (float): Confidence threshold for predictions.
        save_log (bool): Whether to log results to CSV.

    Returns:
        dict: {
            'label': str,
            'confidence': float,
            'boxes': list of dicts,
            'names': dict
        }
    """
    model = model_bundle["model"]
    names = model_bundle.get("names", {})

    results = model(image_path, imgsz=imgsz, conf=conf_thresh)
    r = results[0]
    boxes_out = []

    if hasattr(r, "boxes") and len(r.boxes):
        for box in r.boxes.data.tolist():
            x1, y1, x2, y2, conf, cls = box
            boxes_out.append({
                "x1": float(x1),
                "y1": float(y1),
                "x2": float(x2),
                "y2": float(y2),
                "confidence": float(conf),
                "class": int(cls)
            })

    if boxes_out:
        # Select the box with the highest confidence
        top = max(boxes_out, key=lambda b: b["confidence"])
        top_label = names.get(top["class"], str(top["class"]))
        top_conf = float(top["confidence"])
    else:
        top_label = "no-detection"
        top_conf = 0.0

    print(f"🔍 Prediction: {top_label} ({top_conf:.3f}) — {len(boxes_out)} boxes detected.")

    # Optional logging to CSV for audit/future improvement
    if save_log:
        log_dir = "logs"
        os.makedirs(log_dir, exist_ok=True)
        log_file = os.path.join(log_dir, "predictions.csv")
        with open(log_file, "a", newline="") as f:
            writer = csv.writer(f)
            writer.writerow([datetime.now(), image_path, top_label, top_conf, len(boxes_out)])
    
    return {
        "label": top_label,
        "confidence": top_conf,
        "boxes": boxes_out,
        "names": names
    }


# ============================
# DRAW ANNOTATIONS UTILITY
# ============================
def draw_boxes_on_image(src_image_path, boxes, dst_image_path, labels=None, thickness=2):
    """
    Draw bounding boxes with confidence labels on an image.

    Args:
        src_image_path (str): Path to source image.
        boxes (list): List of box dicts (x1, y1, x2, y2, confidence, class).
        dst_image_path (str): Path to save annotated image.
        labels (dict): Optional mapping of class indices to names.
        thickness (int): Thickness of the bounding box lines.

    Returns:
        str: Path to saved annotated image.
    """
    img = Image.open(src_image_path).convert("RGB")
    draw = ImageDraw.Draw(img)
    W, H = img.size

    try:
        font_size = max(14, int(min(W, H) * 0.03))
        font = ImageFont.truetype("arial.ttf", size=font_size)
    except Exception:
        font = ImageFont.load_default()

    for b in boxes:
        x1, y1, x2, y2 = b['x1'], b['y1'], b['x2'], b['y2']
        cls_id = b.get('class', '')
        conf = b.get('confidence', 0)
        label_text = f"{labels[cls_id] if labels and cls_id in labels else cls_id}: {conf:.2f}"

        # Draw bounding box
        draw.rectangle([x1, y1, x2, y2], outline="red", width=thickness)

        # Draw label background
        try:
            bbox = draw.textbbox((0, 0), label_text, font=font)
            text_w, text_h = bbox[2] - bbox[0], bbox[3] - bbox[1]
        except Exception:
            text_w, text_h = font.getsize(label_text)
        
        draw.rectangle([x1, y1 - text_h - 4, x1 + text_w + 4, y1], fill="red")
        draw.text((x1 + 2, y1 - text_h - 2), label_text, fill="white", font=font)

    img.save(dst_image_path)
    print(f"🖼️ Annotated image saved: {dst_image_path}")
    return dst_image_path