#!/usr/bin/env python3
"""Render target dumps of WWHD_FRAME_TRACE (*.surf) to PNG.

usage: surf2png.py FILE_OR_DIR... [--scale S]

Each .surf has a 32-byte header: "WWSF", width, height, VkFormat, bytes per texel, GX2 format,
guest address, 0; then the texels. Float formats are written twice: clamped to 0..1 (*.png) and
divided by their maximum (*_norm.png); the value ranges go to stdout.
"""
import os, struct, sys
import numpy as np
from PIL import Image

VK = {37: ("u8", 4), 44: ("u8bgra", 4), 43: ("u8", 4), 50: ("u8bgra", 4), 9: ("u8", 1), 16: ("u8", 2),
      64: ("a2b10g10r10", 1), 97: ("f16", 4), 91: ("u16", 4), 109: ("f32", 4), 107: ("u32", 4),
      122: ("b10g11r11", 1), 76: ("f16", 1), 83: ("f16", 2), 100: ("f32", 1), 103: ("f32", 2), 70: ("u16", 1), 77: ("u16", 2)}

def half_floats(a):
    return a.view(np.float16).astype(np.float32)

def decode(path):
    raw = open(path, "rb").read()
    magic, w, h, vkf, bpt, gx2, addr, _ = struct.unpack("<8I", raw[:32])
    if magic != 0x46535757:
        raise ValueError("not a .surf file")
    data = np.frombuffer(raw[32:32 + w * h * bpt], np.uint8)
    kind, ch = VK.get(vkf, (None, 0))
    if kind is None:
        raise ValueError("VkFormat %d not supported" % vkf)
    if kind in ("u8", "u8bgra"):
        img = data.reshape(h, w, ch).astype(np.float32) / 255
        if kind == "u8bgra":
            img = img[..., [2, 1, 0, 3]]
    elif kind == "u16":
        img = data.view(np.uint16).reshape(h, w, ch).astype(np.float32) / 65535
    elif kind == "f16":
        img = half_floats(data.view(np.uint16)).reshape(h, w, ch)
    elif kind == "f32":
        img = data.view(np.float32).reshape(h, w, ch)
    elif kind == "u32":
        img = data.view(np.uint32).reshape(h, w, ch).astype(np.float32)
    elif kind == "a2b10g10r10":
        v = data.view(np.uint32).reshape(h, w)
        img = np.stack([(v >> s) & 1023 for s in (0, 10, 20)] + [(v >> 30) * 341], -1).astype(np.float32) / 1023
    elif kind == "b10g11r11":
        v = data.view(np.uint32).reshape(h, w)
        def uf(m, e, bits):
            m = m.astype(np.float32); e = e.astype(np.int32)
            return np.where(e == 0, m / (1 << bits) * 2.0 ** -14, (1 + m / (1 << bits)) * 2.0 ** (e - 15))
        img = np.stack([uf(v & 63, (v >> 6) & 31, 6), uf((v >> 11) & 63, (v >> 17) & 31, 6),
                        uf((v >> 22) & 31, (v >> 27) & 31, 5)], -1)
    return img, dict(w=w, h=h, vk=vkf, gx2=gx2, addr=addr)

def save(img, path):
    if img.shape[2] == 1:
        img = np.repeat(img, 3, 2)
    elif img.shape[2] == 2:
        img = np.concatenate([img, np.zeros_like(img[..., :1])], 2)
    rgb = np.nan_to_num(img[..., :3])
    Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8)).save(path)

def main():
    files = []
    for a in sys.argv[1:]:
        if os.path.isdir(a):
            files += sorted(os.path.join(a, f) for f in os.listdir(a) if f.endswith(".surf"))
        elif a.endswith(".surf"):
            files.append(a)
    for f in files:
        try:
            img, info = decode(f)
        except Exception as e:
            print("%s: %s" % (f, e)); continue
        rgb = img[..., :min(3, img.shape[2])]
        mx, mean = float(np.nanmax(rgb)), float(np.nanmean(rgb))
        alpha = " alpha %.3f..%.3f" % (float(img[..., 3].min()), float(img[..., 3].max())) if img.shape[2] == 4 else ""
        print("%s %ux%u vk%d gx2 %X: max %.4f mean %.4f%s" % (os.path.basename(f), info["w"], info["h"], info["vk"],
                                                             info["gx2"], mx, mean, alpha))
        save(img, f[:-5] + ".png")
        if mx > 1.0 or mx < 0.2:
            save(img / max(mx, 1e-6), f[:-5] + "_norm.png")

if __name__ == "__main__":
    main()
