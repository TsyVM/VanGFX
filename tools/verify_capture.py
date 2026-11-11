"""
Checks the files tests/capture_writer_tests.cpp wrote, with decoders VanGFX did
not write: every EXR through OpenCV's OpenEXR, every PNG through PIL, each
compared with the exact decoded values in the matching .ref file.

    py tools/verify_capture.py [folder]      # default %TEMP%\\vgx_capture_writers

Needs numpy, opencv-python (built with OpenEXR) and Pillow. Exit code: the
number of files that failed.
"""

import os
import struct
import sys

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2          # noqa: E402
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def load_ref(path):
    with open(path, "rb") as f:
        w, h = struct.unpack("<II", f.read(8))
        data = np.frombuffer(f.read(), dtype=np.float32).reshape(h, w, 4)
    return data


# How each test format decodes: channels, and whether it is stored sRGB.
FORMATS = {
    "rgba8": (4, False), "rgba8_srgb": (4, True), "bgra8": (4, False), "a8r8g8b8": (4, False),
    "x8r8g8b8": (3, False), "r8": (1, False), "rg8": (2, False), "r10g10b10a2": (4, False),
    "rgba16f": (4, False), "rgba32f": (4, False), "rg16f": (2, False), "r32f": (1, False),
    "r16f": (1, False), "r16_unorm": (1, False), "rgba16_unorm": (4, False), "rgba16_snorm": (4, False),
    "r11g11b10f": (3, False), "d24s8": (1, False), "d32f": (1, False), "d16": (1, False),
    "r8_uint": (1, False),
}
HALF = {"rgba8", "rgba8_srgb", "bgra8", "a8r8g8b8", "x8r8g8b8", "r8", "rg8", "r10g10b10a2", "rgba16f",
        "rg16f", "r16f", "r16_unorm", "rgba16_unorm", "rgba16_snorm", "r11g11b10f"}


def check_exr(path, name, ref):
    channels, srgb = FORMATS[name]
    img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if img is None:
        return "OpenCV could not read it"
    want = ref.copy()
    if srgb:
        want[..., :3] = srgb_to_linear(want[..., :3])
    # OpenCV gives B,G,R(,A) for colour, one plane for Y/Z.
    if img.ndim == 2:
        got = img[..., None]
        want = want[..., :1]
    else:
        got = img[..., [2, 1, 0] + ([3] if img.shape[2] == 4 else [])]
        want = want[..., :got.shape[2]]
        if channels == 2:           # R, G and a B OpenCV fills with 0
            want = want.copy()
            want[..., 2] = 0
    if got.shape != want.shape:
        return f"shape {got.shape} != {want.shape}"
    tol = np.maximum(np.abs(want) * (1 / 1024 if name in HALF else 1e-6), 1e-4 if name in HALF else 1e-6)
    bad = np.abs(got.astype(np.float64) - want.astype(np.float64)) > tol
    if bad.any():
        idx = np.argwhere(bad)[0]
        return f"{int(bad.sum())} samples off, first at {tuple(idx)}: {got[tuple(idx)]} vs {want[tuple(idx)]}"
    return None


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, 12.92 * c, 1.055 * c ** (1 / 2.4) - 0.055)


def stretch(c):
    lo, hi = np.nanmin(c), np.nanmax(c)
    return (c - lo) / (hi - lo) if hi > lo else c


# The conversion write_png applies to sources deeper than 8 bits.
UNORM = {"r10g10b10a2", "rgba16_unorm", "r16_unorm"}
FLOAT_COLOUR = {"rgba16f", "rgba32f", "r11g11b10f"}
SNORM = {"rgba16_snorm"}


def check_png(path, name, ref):
    channels, _ = FORMATS[name]
    # OpenCV, not PIL: PIL has no 16-bit RGB mode and would narrow the samples.
    raw = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if raw is None:
        return "OpenCV could not read it"
    img = raw if raw.ndim == 2 else raw[..., [2, 1, 0] + ([3] if raw.shape[2] == 4 else [])]
    if img.shape[0] != ref.shape[0] or img.shape[1] != ref.shape[1]:
        return f"size {img.shape} vs {ref.shape}"
    eight = img.dtype == np.uint8
    if img.ndim == 2:
        img = img[..., None]
    got = img.astype(np.float64) / (255.0 if eight else 65535.0)

    want = ref.astype(np.float64).copy()
    if name in ("rgba8", "rgba8_srgb", "bgra8", "a8r8g8b8", "x8r8g8b8", "r8", "rg8"):
        if not eight:
            return "8-bit source not written as 8-bit"
        tol = 0.5 / 255 + 1e-6
    else:
        if eight:
            return "deep source written as 8-bit"
        tol = 1.0 / 65535 + 1e-6
        if name in FLOAT_COLOUR:
            want[..., :3] = linear_to_srgb(want[..., :3])
            tol = 2e-5
        elif name in SNORM:
            want = want * 0.5 + 0.5
        elif name not in UNORM:
            for c in range(max(channels, 1)):
                want[..., c] = stretch(want[..., c])
    want = np.clip(want, 0.0, 1.0)
    if channels == 2:
        want[..., 2] = 0
    want = want[..., :got.shape[2]]
    diff = np.abs(got - want)
    if diff.max() > tol:
        idx = np.unravel_index(np.argmax(diff), diff.shape)
        return f"max error {diff.max():.3g} at {idx}: {got[idx]} vs {want[idx]}"
    return None


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ["TEMP"], "vgx_capture_writers")
    failures = 0
    checked = 0
    for name in FORMATS:
        ref_path = os.path.join(folder, name + ".ref")
        if not os.path.exists(ref_path):
            print(f"MISSING {name}.ref")
            failures += 1
            continue
        ref = load_ref(ref_path)
        for comp in ("none", "rle", "zips", "zip"):
            p = os.path.join(folder, f"{name}.{comp}.exr")
            err = check_exr(p, name, ref)
            checked += 1
            if err:
                failures += 1
                print(f"FAIL {name}.{comp}.exr: {err}")
        p = os.path.join(folder, name + ".png")
        err = check_png(p, name, ref)
        checked += 1
        if err:
            failures += 1
            print(f"FAIL {name}.png: {err}")
    print(f"{checked - failures} of {checked} files check out")
    return failures


if __name__ == "__main__":
    sys.exit(main())
