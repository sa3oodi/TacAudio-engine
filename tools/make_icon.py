#!/usr/bin/env python3
"""Draws the TacAudio mark and writes the Windows icon + README logo.

    python tools/make_icon.py

Outputs
  apps/tacaudio_gui/res/tacaudio.ico   (16..256 px, PNG-compressed entries)
  docs/images/logo.png                 (512 px)

Only needs numpy (anti-aliasing by 4x4 supersampling; PNG written with zlib).
"""

import os
import struct
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BG = np.array([19, 22, 29], dtype=np.float64)
ORANGE = np.array([255, 122, 26], dtype=np.float64)


def render(size, background=True):
    ss = 4
    n = size * ss
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    u = (x + 0.5) / n  # 0..1
    v = (y + 0.5) / n
    rgb = np.zeros((n, n, 3))
    alpha = np.zeros((n, n))

    if background:  # rounded square
        r = 0.22
        dx = np.maximum(np.abs(u - 0.5) - (0.5 - r), 0)
        dy = np.maximum(np.abs(v - 0.5) - (0.5 - r), 0)
        inside = (dx * dx + dy * dy) <= r * r
        rgb[inside] = BG
        alpha[inside] = 1.0

    # Concentric arcs (same geometry as the sidebar logo), slightly left of centre.
    cx, cy = 0.46, 0.5
    du, dv = u - cx, v - cy
    dist = np.hypot(du, dv)
    ang = np.arctan2(dv, du)  # screen coordinates: clockwise from +x
    stroke = 0.075 if size >= 32 else 0.10
    for i in range(3):
        radius = 0.13 + 0.115 * i
        a0 = -np.pi * (0.25 + 0.1 * i)
        span = np.pi * 1.35
        rel = np.mod(ang - a0, 2 * np.pi)
        on = (np.abs(dist - radius) <= stroke / 2) & (rel <= span)
        shade = 1.0 - 0.22 * i
        rgb[on] = ORANGE * shade + BG * (1 - shade) if background else ORANGE
        alpha[on] = 1.0 if background else shade
    dot = dist <= 0.055
    rgb[dot] = ORANGE
    alpha[dot] = 1.0

    # Downsample (box filter) with premultiplied alpha.
    pre = rgb * alpha[..., None]
    pre = pre.reshape(size, ss, size, ss, 3).mean(axis=(1, 3))
    a = alpha.reshape(size, ss, size, ss).mean(axis=(1, 3))
    out = np.where(a[..., None] > 0, pre / np.maximum(a[..., None], 1e-9), 0)
    img = np.concatenate([out, a[..., None] * 255], axis=2)
    return np.clip(np.round(img), 0, 255).astype(np.uint8)


def png_bytes(img):
    h, w, _ = img.shape
    raw = b"".join(b"\x00" + img[row].tobytes() for row in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def write_ico(path, sizes):
    pngs = [png_bytes(render(s)) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, data in zip(sizes, pngs):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(header + entries + b"".join(pngs))


if __name__ == "__main__":
    ico = os.path.join(ROOT, "apps", "tacaudio_gui", "res", "tacaudio.ico")
    write_ico(ico, [16, 20, 24, 32, 40, 48, 64, 128, 256])
    logo = os.path.join(ROOT, "docs", "images", "logo.png")
    os.makedirs(os.path.dirname(logo), exist_ok=True)
    with open(logo, "wb") as f:
        f.write(png_bytes(render(512)))
    print("wrote", ico)
    print("wrote", logo)
