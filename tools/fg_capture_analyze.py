#!/usr/bin/env python3
"""Classify captured frame-generation pairs.

Reads the raw planes written by the in-game diagnostics capture (Diagnostics/FrameCaptureKey) and
answers the questions a screenshot cannot: whether a generated image is duplicated, interpolated or
blended; whether its depth/motion guides share its row orientation; whether the motion sign is right;
whether the pre-overlay plane really excludes the UI; and whether the player's own silhouette carries
per-object motion.

Usage:
    python tools/fg_capture_analyze.py <capture-dir>

<capture-dir> is normally BepInEx/BigScale/fg-capture under the game root.
Writes PNG previews to <capture-dir>/analysis/ and prints one block per analysable pair plus one
summary line per provider.
"""

import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

GENERATED = "generated.rgba8"
REAL = "real.rgba8"
HUDLESS = "hudless.rgba8"
DEPTH = "depth.r32f"
MOTION = "motion.rg16f"


def read_rgba(path: Path, width: int, height: int) -> np.ndarray:
    data = np.fromfile(path, dtype=np.uint8)
    expected = width * height * 4
    if data.size != expected:
        raise ValueError(f"{path.name}: {data.size} bytes, expected {expected}")
    return data.reshape(height, width, 4).astype(np.float32) / 255.0


def read_float(path: Path, width: int, height: int) -> np.ndarray:
    data = np.fromfile(path, dtype=np.float32)
    if data.size != width * height:
        raise ValueError(f"{path.name}: {data.size} floats, expected {width * height}")
    return data.reshape(height, width)


def read_half2(path: Path, width: int, height: int) -> np.ndarray:
    data = np.fromfile(path, dtype=np.float16)
    if data.size != width * height * 2:
        raise ValueError(f"{path.name}: {data.size} halfs, expected {width * height * 2}")
    return data.reshape(height, width, 2).astype(np.float32)


def luma(image: np.ndarray) -> np.ndarray:
    return 0.2126 * image[..., 0] + 0.7152 * image[..., 1] + 0.0722 * image[..., 2]


def bilinear(image: np.ndarray, xs: np.ndarray, ys: np.ndarray) -> np.ndarray:
    height, width = image.shape[:2]
    x0 = np.floor(xs).astype(np.int32)
    y0 = np.floor(ys).astype(np.int32)
    fx = (xs - x0)[..., None]
    fy = (ys - y0)[..., None]
    x0c = np.clip(x0, 0, width - 1)
    x1c = np.clip(x0 + 1, 0, width - 1)
    y0c = np.clip(y0, 0, height - 1)
    y1c = np.clip(y0 + 1, 0, height - 1)
    top = image[y0c, x0c] * (1.0 - fx) + image[y0c, x1c] * fx
    bottom = image[y1c, x0c] * (1.0 - fx) + image[y1c, x1c] * fx
    return top * (1.0 - fy) + bottom * fy


def box_downsample(image: np.ndarray, width: int, height: int) -> np.ndarray:
    source_height, source_width = image.shape[:2]
    if (source_height, source_width) == (height, width):
        return image
    block_y = max(1, source_height // height)
    block_x = max(1, source_width // width)
    if source_height % height == 0 and source_width % width == 0:
        return image.reshape(height, block_y, width, block_x, *image.shape[2:]).mean(axis=(1, 3))
    ys = (np.arange(height) * source_height // height).astype(np.int32)
    xs = (np.arange(width) * source_width // width).astype(np.int32)
    return image[ys][:, xs]


def correlation(left: np.ndarray, right: np.ndarray) -> float:
    a = left.ravel().astype(np.float64)
    b = right.ravel().astype(np.float64)
    a -= a.mean()
    b -= b.mean()
    denominator = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / denominator) if denominator > 0 else 0.0


def upsample_motion(motion: np.ndarray, width: int, height: int) -> np.ndarray:
    render_height, render_width = motion.shape[:2]
    ys = np.clip((np.arange(height) * render_height // height), 0, render_height - 1)
    xs = np.clip((np.arange(width) * render_width // width), 0, render_width - 1)
    return motion[ys][:, xs]


def warp(image: np.ndarray, displacement: np.ndarray) -> np.ndarray:
    height, width = image.shape[:2]
    ys, xs = np.mgrid[0:height, 0:width].astype(np.float32)
    return bilinear(image, xs + displacement[..., 0], ys + displacement[..., 1])


def reprojection_error(previous: np.ndarray, current: np.ndarray, motion: np.ndarray,
                       sigma: tuple) -> np.ndarray:
    signed = motion * np.array([sigma[0], sigma[1]], dtype=np.float32)
    warped = warp(previous, signed)
    return np.abs(warped - current).mean(axis=2)


def gradient_magnitude(plane: np.ndarray) -> np.ndarray:
    dy = np.zeros_like(plane)
    dx = np.zeros_like(plane)
    dy[1:-1, :] = plane[2:, :] - plane[:-2, :]
    dx[:, 1:-1] = plane[:, 2:] - plane[:, :-2]
    return np.hypot(dx, dy)


def save_png(path: Path, rgb: np.ndarray) -> None:
    Image.fromarray((np.clip(rgb, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)).save(path)


def grey(plane: np.ndarray) -> np.ndarray:
    clipped = np.clip(plane, 0.0, 1.0)
    return np.stack([clipped, clipped, clipped], axis=2)


class Pair:
    def __init__(self, directory: Path, index: int, meta: dict):
        self.directory = directory
        self.index = index
        self.meta = meta
        self.output_width = int(meta.get("outputWidth", 0))
        self.output_height = int(meta.get("outputHeight", 0))
        self.render_width = int(meta.get("renderWidth", 0))
        self.render_height = int(meta.get("renderHeight", 0))
        self.real = read_rgba(directory / f"{index:02d}-{REAL}", self.output_width, self.output_height)
        hudless_path = directory / f"{index:02d}-{HUDLESS}"
        self.hudless = (read_rgba(hudless_path, self.output_width, self.output_height)
                        if hudless_path.exists() else None)
        generated_path = directory / f"{index:02d}-{GENERATED}"
        self.generated = (read_rgba(generated_path, self.output_width, self.output_height)
                          if generated_path.exists() else None)
        self.depth = read_float(directory / f"{index:02d}-{DEPTH}", self.render_width, self.render_height)
        self.motion = read_half2(directory / f"{index:02d}-{MOTION}", self.render_width, self.render_height)
        # Stored motion is a UV displacement, so the pixel displacement is that value times the
        # extent it is expressed in. The transport hands previous-to-current vectors in screen rows,
        # so the current-to-previous displacement is the stored value times the negative extent.
        self.motion_pixels = -self.motion * np.array(
            [float(self.render_width), float(self.render_height)], dtype=np.float32)
        upsampled = upsample_motion(self.motion, self.output_width, self.output_height)
        self.motion_output = -upsampled * np.array(
            [float(self.output_width), float(self.output_height)], dtype=np.float32)

    @property
    def source_frame(self) -> int:
        return int(self.meta.get("sourceFrame", -1))

    @property
    def reset(self) -> bool:
        return bool(self.meta.get("reset", False))

    @property
    def provider(self) -> str:
        return str(self.meta.get("provider", "unknown"))


def load_pairs(directory: Path) -> list:
    pairs = {}
    for meta_path in sorted(directory.glob("*-meta.json")):
        try:
            index = int(meta_path.name.split("-")[0])
        except ValueError:
            continue
        with open(meta_path, "rb") as handle:
            meta = json.load(handle)
        try:
            pairs[index] = Pair(directory, index, meta)
        except (OSError, ValueError) as error:
            print(f"pair {index:02d}: skipped ({error})")
    return [pairs[index] for index in sorted(pairs)]


def analyse(pair: Pair, previous: Pair, analysis: Path) -> dict:
    width, height = pair.output_width, pair.output_height
    result = {"provider": pair.provider}
    prefix = f"pair {pair.index:02d} frame={pair.source_frame}"
    if pair.generated is None:
        print(f"{prefix}: no generated plane, skipped")
        return result

    real_previous = previous.real
    generated = pair.generated
    real = pair.real
    mask_rgb = slice(0, 3)

    # M1 - duplicate class.
    distance = float(np.abs(real - real_previous)[..., mask_rgb].mean())
    to_current = float(np.abs(generated - real)[..., mask_rgb].mean())
    to_previous = float(np.abs(generated - real_previous)[..., mask_rgb].mean())
    blend_reference = 0.5 * (real + real_previous)
    to_blend = float(np.abs(generated - blend_reference)[..., mask_rgb].mean())
    if distance < 0.002:
        duplicate = "static-inconclusive"
    elif to_current < 0.15 * distance:
        duplicate = "duplicate-current"
    elif to_previous < 0.15 * distance:
        duplicate = "duplicate-previous"
    elif to_blend < 0.15 * distance:
        duplicate = "blend"
    else:
        duplicate = "distinct"
    result["duplicate"] = duplicate

    # M2 - midpoint warp, restricted to pixels where the pre-overlay plane equals the real colour.
    # The stored motion points from the current frame to the previous one (mv = cur - prev), so the
    # half-time image is the previous frame warped half a step forward: previous(x + 0.5*mv).
    hudless = pair.hudless if pair.hudless is not None else real
    static_mask = np.abs(hudless - real).max(axis=2) <= (3.0 / 255.0)
    half = 0.5 * pair.motion_output
    warped_previous = warp(real_previous, half)
    difference_mid = np.abs(generated - warped_previous).mean(axis=2)
    difference_current = np.abs(generated - real).mean(axis=2)
    if static_mask.any():
        d_mid = float(difference_mid[static_mask].mean())
        d_cur = float(difference_current[static_mask].mean())
        interpolated = d_mid < d_cur
    else:
        d_mid = d_cur = float("nan")
        interpolated = None
    result["midpoint"] = (d_mid, d_cur, interpolated)

    # M3 - guide orientation: depth edges must share the pre-overlay rows.
    depth_edges = gradient_magnitude(np.log(pair.depth + 1e-7))
    luma_small = box_downsample(luma(hudless), pair.render_width, pair.render_height)
    luma_edges = gradient_magnitude(luma_small)
    corr_same = correlation(depth_edges, luma_edges)
    corr_flip = correlation(depth_edges, luma_edges[::-1])
    result["orientation"] = (corr_same, corr_flip, corr_same > corr_flip + 0.05)

    # M4 - motion sign. err_s = |previous(x + s*mv) - current|.
    current_small = box_downsample(real, pair.render_width, pair.render_height)
    previous_small = box_downsample(real_previous, pair.render_width, pair.render_height)
    errors = {}
    for sigma in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
        errors[sigma] = float(reprojection_error(
            previous_small, current_small, pair.motion_pixels, sigma).mean())
    zero_error = float(np.abs(previous_small - current_small).mean())
    best = min(errors, key=errors.get)
    result["sign"] = (best, errors[best], zero_error,
                      best == (1, 1) and errors[best] < 0.8 * zero_error)

    # M5 - HUD-less extraction.
    if pair.hudless is not None:
        ui_fraction = float((np.abs(pair.hudless - real).max(axis=2) > (3.0 / 255.0)).mean())
        skew = float(np.abs(pair.hudless - real_previous).mean()) < float(
            np.abs(pair.hudless - real).mean())
        result["hudless"] = (ui_fraction, skew, ui_fraction < 0.15 and not skew)
    else:
        result["hudless"] = None

    # M6 - body box: lower-middle of the frame, where a first-person silhouette appears.
    top = int(0.6 * pair.render_height)
    left = int(0.3 * pair.render_width)
    right = int(0.7 * pair.render_width)
    bottom = pair.render_height
    magnitude = np.hypot(pair.motion_pixels[..., 0], pair.motion_pixels[..., 1])
    box_magnitude = magnitude[top:bottom, left:right]
    box_mean = float(box_magnitude.mean())
    frame_mean = float(magnitude.mean())
    best_error = reprojection_error(previous_small, current_small, pair.motion_pixels, best)
    box_best = float(best_error[top:bottom, left:right].mean())
    box_zero = float(np.abs(previous_small - current_small)[top:bottom, left:right].mean())
    suspicious = box_best > 1.25 * box_zero and box_mean > 2.0
    result["body"] = (frame_mean, box_mean, box_best, box_zero, suspicious)

    stem = f"{pair.index:02d}"
    save_png(analysis / f"{stem}-real.png", real)
    if pair.hudless is not None:
        save_png(analysis / f"{stem}-hudless.png", pair.hudless)
    save_png(analysis / f"{stem}-generated.png", generated)
    percentile = float(np.percentile(magnitude, 99.0)) if magnitude.size else 1.0
    save_png(analysis / f"{stem}-motion.png", grey(magnitude / max(percentile, 1e-6)))
    save_png(analysis / f"{stem}-reproj-error.png", grey(4.0 * best_error))

    print(f"{prefix} provider={pair.provider}")
    print(f"  M1 duplicate={duplicate} dE={distance:.4f} toCurrent={to_current:.4f} "
          f"toPrevious={to_previous:.4f} toBlend={to_blend:.4f}")
    print(f"  M2 midpoint dMid={d_mid:.4f} dCur={d_cur:.4f} verdict="
          f"{'interpolated' if interpolated else 'not-interpolated' if interpolated is not None else 'n/a'}")
    print(f"  M3 orientation corrSame={corr_same:.3f} corrFlip={corr_flip:.3f} "
          f"verdict={'pass' if result['orientation'][2] else 'FAIL'}")
    print(f"  M4 motionSign best={best} errBest={errors[best]:.4f} errZero={zero_error:.4f} "
          f"verdict={'pass' if result['sign'][3] else 'FAIL'}")
    if result["hudless"] is None:
        print("  M5 hudless n/a (no pre-overlay plane)")
    else:
        ui_fraction, skew, ok = result["hudless"]
        print(f"  M5 hudless uiFraction={ui_fraction:.4f} skew={skew} "
              f"verdict={'pass' if ok else 'FAIL'}")
    print(f"  M6 body |mv|frame={frame_mean:.2f}px |mv|box={box_mean:.2f}px "
          f"errBestBox={box_best:.4f} errZeroBox={box_zero:.4f} "
          f"flag={'body-mv-suspect' if suspicious else 'clean'}")
    return result


def summarise(provider: str, results: list) -> None:
    if not results:
        return
    duplicate = [r.get("duplicate") for r in results]
    midpoint = [r["midpoint"][2] for r in results if r.get("midpoint")]
    orientation = [r["orientation"][2] for r in results if r.get("orientation")]
    sign = [r["sign"][3] for r in results if r.get("sign")]
    hudless = [r["hudless"][2] for r in results if r.get("hudless")]
    body = [r["body"][4] for r in results if r.get("body")]

    def verdict(flags):
        return "n/a" if not flags else f"{sum(1 for flag in flags if flag)}/{len(flags)}"

    print(f"{provider}: pairs={len(results)} "
          f"M1={dict((name, duplicate.count(name)) for name in sorted(set(duplicate)))} "
          f"M2_interpolated={verdict(midpoint)} M3_pass={verdict(orientation)} "
          f"M4_pass={verdict(sign)} M5_pass={verdict(hudless)} "
          f"M6_suspect={sum(1 for flag in body if flag)}/{len(body)}")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    directory = Path(sys.argv[1])
    if not directory.is_dir():
        print(f"{directory} is not a capture directory")
        return 2
    pairs = load_pairs(directory)
    if len(pairs) < 2:
        print(f"{directory}: {len(pairs)} pair(s); at least two consecutive pairs are needed")
        return 1
    analysis = directory / "analysis"
    analysis.mkdir(exist_ok=True)
    by_provider = {}
    analysed = 0
    for current, previous in zip(pairs[1:], pairs[:-1]):
        if current.reset or previous.reset:
            continue
        if current.source_frame != previous.source_frame + 1:
            continue
        if current.provider != previous.provider:
            continue
        result = analyse(current, previous, analysis)
        if not result:
            continue
        analysed += 1
        by_provider.setdefault(current.provider, []).append(result)
    print()
    for provider, results in by_provider.items():
        summarise(provider, results)
    print(f"analysed {analysed} pair(s) from {len(pairs)} capture(s) in {directory}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
