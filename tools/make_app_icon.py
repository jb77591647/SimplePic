# -*- coding: utf-8 -*-
# 程序图标生成：由 src/logo.png（品牌 LOGO 原图，RGBA 带圆角 alpha）生成 src/app.ico
# 尺寸覆盖 256/128/64/48/32/24/16；256 直接用原图 LANCZOS 缩放。
# 用法：python tools/make_app_icon.py
import os
import struct

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "logo.png")
OUT = os.path.join(ROOT, "src", "app.ico")
SIZES = [256, 128, 64, 48, 32, 24, 16]


def png_bytes(img):
    """编码单张 PNG（PIL 输出的 ICO 内嵌 PNG 仅对 256 层合规，这里统一手写 ICO 容器）。"""
    import io
    b = io.BytesIO()
    img.save(b, "PNG")
    return b.getvalue()


def build_ico():
    base = Image.open(SRC).convert("RGBA")
    pngs = []
    for s in SIZES:
        img = base.resize((s, s), Image.LANCZOS)
        pngs.append((s, png_bytes(img)))
    # ICO 容器：6 字节头 + 每图 16 字节目录 + 各 PNG 数据
    header = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    entries = b""
    body = b""
    for s, data in pngs:
        w = 0 if s >= 256 else s  # 256 写 0
        entries += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32,
                               len(data), offset)
        body += data
        offset += len(data)
    with open(OUT, "wb") as f:
        f.write(header + entries + body)
    print(f"ICO written: {OUT} ({os.path.getsize(OUT)} bytes, {len(pngs)} sizes)")


if __name__ == "__main__":
    build_ico()
