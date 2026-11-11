# -*- coding: utf-8 -*-
"""完整场景复现：拖入文件 → 逐像素拖拽窗体 → 枚举矩形 + PrintWindow 截图"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import time
import traceback

RPT = r"E:\TRAE Project\SimplePic\debug_report.txt"
_log = open(RPT, "w", encoding="utf-8")


def P(*a):
    print(*a, file=_log)
    _log.flush()


def make_test_pngs():
    from PIL import Image
    import os
    d = r"E:\TRAE Project\SimplePic\tests\out"
    os.makedirs(d, exist_ok=True)
    paths = []
    for i, sz in enumerate([(1600, 1200), (900, 700)]):
        p = os.path.join(d, f"dbg_{i}.png")
        Image.new("RGB", sz, (120 + i * 40, 90, 160)).save(p)
        paths.append(p)
    return paths


def send_dropfiles(hwnd, paths):
    u32 = ctypes.windll.user32
    k32 = ctypes.windll.kernel32
    # 构造 DROPFILES 全局内存
    wide = "".join(p + "\0" for p in paths) + "\0"
    head = ctypes.create_string_buffer(b"\x14\x00\x00\x00" + b"\x00" * 12 + b"\x01\x00")  # pFiles=20, fWide=1
    payload = wide.encode("utf-16-le")
    total = len(head.raw) + len(payload) + 2
    gmem = k32.GlobalAlloc(0x0042, total)  # GMEM_MOVEABLE|GMEM_ZEROINIT
    ptr = k32.GlobalLock(gmem)
    ctypes.memmove(ptr, head.raw, len(head.raw))
    ctypes.memmove(ptr + len(head.raw), payload, len(payload))
    k32.GlobalUnlock(gmem)
    u32.PostMessageW(hwnd, 0x0233, gmem, 0)  # WM_DROPFILES


def _run():
    u32 = ctypes.windll.user32
    k32 = ctypes.windll.kernel32
    EXE = r"E:\TRAE Project\SimplePic\SimplePic.exe"

    u32.SetProcessDPIAware()
    subprocess.run(["taskkill", "/f", "/im", "SimplePic.exe"], capture_output=True)
    time.sleep(0.5)
    subprocess.Popen([EXE], cwd=r"E:\TRAE Project\SimplePic")
    time.sleep(2.5)

    hwnd = u32.FindWindowW("SimplePicMain", None)
    if not hwnd:
        P("window not found")
        return

    # 先设一个起始尺寸
    u32.SetWindowPos(hwnd, 0, 100, 60, 1900, 1000, 0x0004)
    time.sleep(0.4)

    pngs = make_test_pngs()
    send_dropfiles(hwnd, pngs)
    time.sleep(1.5)

    def rect(h):
        r = wt.RECT()
        u32.GetWindowRect(h, ctypes.byref(r))
        return r.left, r.top, r.right, r.bottom

    panel = btn_sel = btn_all = hlist = None

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb(h, lp):
        nonlocal panel, btn_sel, btn_all, hlist
        gid = u32.GetDlgCtrlID(h)
        buf = ctypes.create_unicode_buffer(64)
        u32.GetClassNameW(h, buf, 64)
        if gid == 1200 and buf.value == "Static":
            panel = h
        elif gid == 105:
            btn_sel = h
        elif gid == 106:
            btn_all = h
        elif "SysListView32" in buf.value:
            hlist = h
        return True

    u32.EnumChildWindows(hwnd, cb, 0)

    def rel(h):
        ph = rect(panel)
        b = rect(h)
        return (b[0] - ph[0], b[1] - ph[1], b[2] - b[0], b[3] - b[1])

    P("after drop: btn_sel rel:", rel(btn_sel))

    # 选中第一行，触发预览渲染链路
    LVM_SETITEMSTATE = 0x1000 + 43
    lv = wt.LVITEMW()
    lv.stateMask = 0x0002  # LVIS_SELECTED
    lv.state = 0x0002
    u32.SendMessageW(hlist, LVM_SETITEMSTATE, 0, ctypes.byref(lv))
    time.sleep(1.0)
    P("after select: btn_sel rel:", rel(btn_sel))

    # 逐像素拖拽 5 轮
    seqs = [
        (1900, 1000, 1300, 800),
        (1300, 800, 1900, 1000),
        (1900, 1000, 1281, 770),
        (1281, 770, 2000, 1100),
        (2000, 1100, 1450, 880),
    ]
    for i, (w0, h0, w1, h1) in enumerate(seqs):
        steps = max(abs(w1 - w0), abs(h1 - h0)) // 12 + 1
        for s in range(1, steps + 1):
            w = w0 + (w1 - w0) * s // steps
            h = h0 + (h1 - h0) * s // steps
            u32.SetWindowPos(hwnd, 0, 100, 60, w, h, 0x0004)
            time.sleep(0.012)
        time.sleep(0.25)
        P(f"cycle{i} w={w1}: btn_sel rel={rel(btn_sel)} btn_all rel={rel(btn_all)}")

    # 最终状态：全子控件矩形 + 列宽
    kids = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb3(h, lp):
        buf = ctypes.create_unicode_buffer(64)
        u32.GetClassNameW(h, buf, 64)
        gid = u32.GetDlgCtrlID(h)
        rr = wt.RECT()
        u32.GetWindowRect(h, ctypes.byref(rr))
        pt = wt.POINT(rr.left, rr.top)
        u32.ScreenToClient(hwnd, ctypes.byref(pt))
        kids.append((buf.value, gid, pt.x, pt.y, rr.right - rr.left, rr.bottom - rr.top))
        return True

    u32.EnumChildWindows(hwnd, cb3, 0)
    P("--- final children ---")
    for k in kids:
        P(f"{k[0]:<24} id={k[1]:<5} x={k[2]:<6} y={k[3]:<6} w={k[4]:<6} h={k[5]}")

    widths = [u32.SendMessageW(hlist, 0x1000 + 29, i, 0) for i in range(7)]
    P("col widths:", widths, "sum:", sum(widths))
    r = wt.RECT()
    u32.GetClientRect(hlist, ctypes.byref(r))
    P("list client w:", r.right, "WS_HSCROLL:", bool(u32.GetWindowLongW(hlist, -16) & 0x00100000))

    # PrintWindow 截图
    class BMIH(ctypes.Structure):
        _pack_ = 1
        _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                    ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                    ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                    ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                    ("biYPelsPerMeter", ctypes.c_int32),
                    ("biClrUsed", ctypes.c_uint32), ("biClrImportant", ctypes.c_uint32)]

    wr = rect(hwnd)
    w, h = wr[2] - wr[0], wr[3] - wr[1]
    hdc = u32.GetWindowDC(hwnd)
    memdc = ctypes.windll.gdi32.CreateCompatibleDC(hdc)
    bmi = BMIH()
    bmi.biSize = ctypes.sizeof(BMIH)
    bmi.biWidth, bmi.biHeight = w, -h
    bmi.biPlanes, bmi.biBitCount = 1, 32
    pbits = ctypes.c_void_p()
    bmp = ctypes.windll.gdi32.CreateDIBSection(memdc, ctypes.byref(bmi), 0, ctypes.byref(pbits), None, 0)
    old = ctypes.windll.gdi32.SelectObject(memdc, bmp)
    u32.PrintWindow(hwnd, memdc, 2)
    ctypes.windll.gdi32.SelectObject(memdc, old)
    buf = (ctypes.c_char * (w * h * 4)).from_address(pbits.value)
    data = bytes(buf)
    ctypes.windll.gdi32.DeleteObject(bmp)
    u32.ReleaseDC(hwnd, hdc)
    from PIL import Image
    img = Image.frombuffer("RGBA", (w, h), data, "raw", "BGRA", 0, 1)
    img.convert("RGB").save(r"E:\TRAE Project\SimplePic\docs\debug_resize.png")
    P("shot saved")


try:
    _run()
except Exception:
    P("TRACEBACK:\n" + traceback.format_exc())
finally:
    _log.close()
