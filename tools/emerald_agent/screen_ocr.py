#!/usr/bin/env python3
"""Preprocess a RetroArch screenshot and recognize its visible text."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image
import pytesseract
from pytesseract import Output


def preprocess(image: Image.Image, mode: str, threshold: int, scale: int) -> Image.Image:
    """Convert to grayscale or black/white, then enlarge pixel text cleanly."""
    image = image.convert("L")
    if scale != 1:
        image = image.resize(
            (image.width * scale, image.height * scale),
            Image.Resampling.NEAREST,
        )
    if mode == "binary":
        image = image.point(lambda value: 255 if value >= threshold else 0, mode="L")
    return image


def recognize(
    image: Image.Image,
    language: str,
    page_segmentation: int,
    coordinate_scale: int,
    offset: tuple[int, int],
) -> dict[str, object]:
    """Return recognized lines with bounding boxes and mean word confidence."""
    data = pytesseract.image_to_data(
        image,
        lang=language,
        config=f"--psm {page_segmentation}",
        output_type=Output.DICT,
    )
    grouped: dict[tuple[int, int, int], dict[str, object]] = {}
    for index, word in enumerate(data["text"]):
        word = word.strip()
        try:
            confidence = float(data["conf"][index])
        except (TypeError, ValueError):
            continue
        if not word or confidence < 0:
            continue

        key = (
            int(data["block_num"][index]),
            int(data["par_num"][index]),
            int(data["line_num"][index]),
        )
        left = int(data["left"][index])
        top = int(data["top"][index])
        right = left + int(data["width"][index])
        bottom = top + int(data["height"][index])
        line = grouped.setdefault(
            key,
            {"words": [], "confidences": [], "left": left, "top": top,
             "right": right, "bottom": bottom},
        )
        line["words"].append(word)
        line["confidences"].append(confidence)
        line["left"] = min(int(line["left"]), left)
        line["top"] = min(int(line["top"]), top)
        line["right"] = max(int(line["right"]), right)
        line["bottom"] = max(int(line["bottom"]), bottom)

    lines = []
    for line in grouped.values():
        confidences = line["confidences"]
        lines.append(
            {
                "text": " ".join(line["words"]),
                "confidence": round(sum(confidences) / len(confidences), 1),
                "box": {
                    "x": round(offset[0] + line["left"] / coordinate_scale, 1),
                    "y": round(offset[1] + line["top"] / coordinate_scale, 1),
                    "width": round(
                        (line["right"] - line["left"]) / coordinate_scale, 1
                    ),
                    "height": round(
                        (line["bottom"] - line["top"]) / coordinate_scale, 1
                    ),
                },
            }
        )

    text = "\n".join(line["text"] for line in lines)
    return {"text": text, "lines": lines}


def parse_crop(value: str) -> tuple[int, int, int, int]:
    try:
        crop = tuple(int(part) for part in value.split(","))
    except ValueError as exc:
        raise argparse.ArgumentTypeError("crop must be x,y,width,height") from exc
    if len(crop) != 4 or crop[2] <= 0 or crop[3] <= 0:
        raise argparse.ArgumentTypeError("crop must be x,y,width,height with positive size")
    return crop


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Preprocess a game screenshot and OCR its visible text"
    )
    parser.add_argument("image", type=Path, help="input screenshot path")
    parser.add_argument(
        "--crop", type=parse_crop,
        help="optional source-image region x,y,width,height (for example, a dialogue box)",
    )
    parser.add_argument(
        "--mode", choices=("grayscale", "binary"), default="grayscale",
        help="image passed to OCR (default: grayscale)",
    )
    parser.add_argument(
        "--threshold", type=int, default=160,
        help="binary cutoff from 0 to 255 (default: 160)",
    )
    parser.add_argument(
        "--scale", type=int, default=3,
        help="nearest-neighbor enlargement factor (default: 3)",
    )
    parser.add_argument("--lang", default="eng", help="installed Tesseract language (default: eng)")
    parser.add_argument(
        "--psm", type=int, default=6,
        help="Tesseract page segmentation mode (default: 6, text block)",
    )
    parser.add_argument(
        "--output-image", type=Path,
        help="optional path to save the preprocessed image",
    )
    parser.add_argument("--json", action="store_true", help="include line boxes and confidence")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not 0 <= args.threshold <= 255:
        print("error: --threshold must be between 0 and 255", file=sys.stderr)
        return 2
    if args.scale < 1:
        print("error: --scale must be at least 1", file=sys.stderr)
        return 2
    if not 0 <= args.psm <= 13:
        print("error: --psm must be between 0 and 13", file=sys.stderr)
        return 2

    try:
        with Image.open(args.image) as source:
            source_width, source_height = source.size
            offset = (0, 0)
            selected = source
            if args.crop:
                x, y, width, height = args.crop
                if x < 0 or y < 0 or x + width > source_width or y + height > source_height:
                    raise ValueError(
                        f"crop {args.crop} falls outside screenshot bounds "
                        f"({source_width}x{source_height})"
                    )
                selected = source.crop((x, y, x + width, y + height))
                offset = (x, y)
            image = preprocess(selected, args.mode, args.threshold, args.scale)
        if args.output_image:
            image.save(args.output_image)
        result = recognize(image, args.lang, args.psm, args.scale, offset)
    except pytesseract.TesseractNotFoundError:
        print(
            "error: Tesseract OCR engine was not found. Install it separately "
            "(macOS: brew install tesseract; Ubuntu: sudo apt install tesseract-ocr).",
            file=sys.stderr,
        )
        return 2
    except pytesseract.TesseractError as exc:
        print(f"error: Tesseract OCR failed: {exc}", file=sys.stderr)
        return 2
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(
            json.dumps(
                {
                    "image": str(args.image),
                    "crop": args.crop,
                    "preprocess": args.mode,
                    "threshold": args.threshold if args.mode == "binary" else None,
                    "scale": args.scale,
                    **result,
                },
                ensure_ascii=False,
                indent=2,
            )
        )
    else:
        print(result["text"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
