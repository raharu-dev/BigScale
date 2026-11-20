#!/usr/bin/env python3
"""Reads the identity bars out of the capture BMPs and asserts the eye they belong to.

Mirrors StageProbe::ClassifyIdentityMarker: a magenta blob that is taller than twice its width is the
eye-0 vertical bar, wider than twice its height is the eye-1 horizontal bar, anything else is "none".
"""
import struct
import sys
import pathlib


def read_bmp(path):
    data = pathlib.Path(path).read_bytes()
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    if bits != 24:
        raise ValueError(f"unsupported {bits}-bit BMP")
    top_down = height < 0
    height = abs(height)
    stride = (width * 3 + 3) & ~3
    rows = []
    for y in range(height):
        start = offset + y * stride
        row = data[start:start + width * 3]
        rows.append([(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]) for x in range(width)])  # RGB
    if not top_down:
        rows.reverse()
    return width, height, rows


def classify(width, height, rows):
    min_x = min_y = 1 << 30
    max_x = max_y = -1
    count = 0
    for y, row in enumerate(rows):
        for x, (r, g, b) in enumerate(row):
            if r > 128 and g < 64 and b > 128:
                count += 1
                min_x, max_x = min(min_x, x), max(max_x, x)
                min_y, max_y = min(min_y, y), max(max_y, y)
    if count < 32:
        return "none", None
    bar_w = max_x - min_x + 1
    bar_h = max_y - min_y + 1
    box = (min_x, min_y, max_x, max_y)
    if bar_h >= 2 * bar_w:
        return "vertical", box
    if bar_w >= 2 * bar_h:
        return "horizontal", box
    return "none", box


def main(paths):
    expected = {"eye0": "vertical", "eye1": "horizontal"}
    failures = 0
    for path in sorted(paths):
        width, height, rows = read_bmp(path)
        shape, box = classify(width, height, rows)
        name = pathlib.Path(path).name.lower()
        eye = "eye0" if "-eye0-" in name else ("eye1" if "-eye1-" in name else "?")
        tagged = "tagged" in name and "untagged" not in name
        reference = expected.get(eye, "none")
        verdict = "tag-absent"
        if tagged:
            verdict = "ok" if shape == reference else f"MISMATCH (want {reference})"
            if shape != reference:
                failures += 1
        elif shape != "none":
            verdict = "unexpected tag in untagged frame"
            failures += 1
        print(f"{pathlib.Path(path).name}: {width}x{height} eye={eye} tagged={int(tagged)} "
              f"shape={shape} box={box} {verdict}")
    print(f"{len(paths)} images, {failures} mismatch(es)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
