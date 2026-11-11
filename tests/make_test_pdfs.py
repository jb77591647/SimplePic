# -*- coding: utf-8 -*-
# 生成两个测试 PDF（写入 tests/out/）：
#   vector_text.pdf —— 纯矢量文字页（旧实现只认 DCTDecode 必失败，验证新渲染路径）
#   dct_image.pdf   —— DCTDecode 内嵌 JPEG 位图页（旧路径兼容验证）
import os
import io
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
os.makedirs(OUT, exist_ok=True)


def build_pdf(objs, content):
    """objs: [(obj_num, body_bytes)]，body 不含 'N 0 obj' 头；返回完整 PDF 字节"""
    buf = io.BytesIO()
    buf.write(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    for num, body in objs:
        offsets[num] = buf.tell()
        buf.write(b"%d 0 obj\n" % num)
        buf.write(body)
        buf.write(b"\nendobj\n")
    xref = buf.tell()
    max_num = max(offsets) + 1
    buf.write(b"xref\n0 %d\n" % max_num)
    buf.write(b"0000000000 65535 f \n")
    for i in range(1, max_num):
        buf.write(b"%010d 00000 n \n" % offsets[i])
    buf.write(b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
              % (max_num, xref))
    return buf.getvalue()


def make_vector_text_pdf():
    content = b"BT /F1 48 Tf 72 660 Td (SimplePic PDF render test) Tj ET\n" \
              b"BT /F1 28 Tf 72 600 Td (Windows.Data.Pdf page render OK) Tj ET\n"
    objs = [
        (1, b"<< /Type /Catalog /Pages 2 0 R >>"),
        (2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        (3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>"),
        (4, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
        (5, b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream"),
    ]
    return build_pdf(objs, content)


def make_dct_image_pdf(jpg_path):
    with open(jpg_path, "rb") as f:
        jpg = f.read()
    # JPEG 尺寸
    w, h = None, None
    i = 2
    while i + 9 < len(jpg):
        if jpg[i] != 0xFF:
            break
        marker = jpg[i + 1]
        seglen = struct.unpack(">H", jpg[i + 2:i + 4])[0]
        if marker in (0xC0, 0xC2):
            h = struct.unpack(">H", jpg[i + 5:i + 7])[0]
            w = struct.unpack(">H", jpg[i + 7:i + 9])[0]
            break
        i += 2 + seglen
    assert w and h, "jpeg parse fail"
    content = b"q %d 0 0 %d 0 0 cm /Im0 Do Q\n" % (w, h)
    objs = [
        (1, b"<< /Type /Catalog /Pages 2 0 R >>"),
        (2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        (3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
            b"/Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>"
            % (w, h)),
        (4, b"<< /Type /XObject /Subtype /Image /Width %d /Height %d "
            b"/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode "
            b"/Length %d >>\nstream\n" % (w, h, len(jpg)) + jpg + b"\nendstream"),
        (5, b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream"),
    ]
    return build_pdf(objs, content), w, h


def make_test_jpg():
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (600, 400), (240, 120, 60))
    d = ImageDraw.Draw(img)
    for x in range(0, 600, 40):
        d.line([(x, 0), (x, 400)], fill=(255, 255, 255))
    d.rectangle([250, 150, 350, 250], fill=(30, 30, 120))
    p = os.path.join(OUT, "src.jpg")
    img.save(p, "JPEG", quality=90)
    return p


if __name__ == "__main__":
    with open(os.path.join(OUT, "vector_text.pdf"), "wb") as f:
        f.write(make_vector_text_pdf())
    jpg = make_test_jpg()
    pdf, w, h = make_dct_image_pdf(jpg)
    with open(os.path.join(OUT, "dct_image.pdf"), "wb") as f:
        f.write(pdf)
    print("OK: tests/out/vector_text.pdf, tests/out/dct_image.pdf (%dx%d)" % (w, h))
