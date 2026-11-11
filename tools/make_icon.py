# -*- coding: utf-8 -*-
# 从源图生成 SimplePic 应用图标 src/app.ico（多尺寸 256~16，圆角透明）
# 用法：python tools/make_icon.py <源图路径>
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "app.ico")


def main(src):
    img = Image.open(src).convert("RGBA")
    # 1) 找非白色内容包围盒（图标圆角方块贴近画布，白底可裁掉）
    gray = img.convert("L")
    # 阈值：与白色差 > 10 视为内容
    mask = gray.point(lambda p: 255 if p < 242 else 0)
    bbox = mask.getbbox()
    if bbox:
        img = img.crop(bbox)
    # 2) 补成正方形（短边扩白透明）
    w, h = img.size
    side = max(w, h)
    sq = Image.new("RGBA", (side, side), (255, 255, 255, 0))
    sq.paste(img, ((side - w) // 2, (side - h) // 2))
    # 3) 圆角透明蒙版（iOS 风格 22% 圆角，与源图样式一致）
    s = sq.size[0]
    r = int(s * 0.22)
    m = Image.new("L", (s, s), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=r, fill=255)
    sq.putalpha(m)
    # 4) 高质量重采样到 256，PIL 自动缩出多尺寸
    sq = sq.resize((256, 256), Image.LANCZOS)
    sq.save(OUT, format="ICO", sizes=[(256, 256), (128, 128), (64, 64),
                                      (48, 48), (32, 32), (16, 16)])
    print("OK:", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main(sys.argv[1])
