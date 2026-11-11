# capture SimplePic main window via PrintWindow -> docs/screenshot.png
# 裁剪到 DWM 扩展框架边界（真实可见窗口）：PrintWindow 会把 DWM 不可见边框/
# 阴影区渲染成黑色，直接落盘会在截图四周出现大黑边
import ctypes, ctypes.wintypes as wt, subprocess, time, sys
from PIL import Image

u32 = ctypes.windll.user32
gdi = ctypes.windll.gdi32
dwm = ctypes.windll.dwmapi
u32.SetProcessDPIAware()

subprocess.run(["taskkill", "/f", "/im", "SimplePic.exe"], capture_output=True)
time.sleep(0.5)
subprocess.Popen([r"E:\TRAE Project\SimplePic\SimplePic.exe"])
time.sleep(3)

hwnd = u32.FindWindowW("SimplePicMain", None)
if not hwnd:
    print("window not found"); sys.exit(1)
u32.ShowWindow(hwnd, 9)  # SW_RESTORE（被最小化时恢复）
time.sleep(0.8)

r = wt.RECT()
u32.GetWindowRect(hwnd, ctypes.byref(r))
w, h = r.right - r.left, r.bottom - r.top

# DWM 真实可见边界（去掉不可见缩放边框与阴影）
fb = wt.RECT()
if dwm.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(fb), ctypes.sizeof(fb)) != 0:
    fb = r  # 拿不到（老系统）退回整窗

hdc_wnd = u32.GetWindowDC(hwnd)
hdc_mem = gdi.CreateCompatibleDC(hdc_wnd)
hbmp = gdi.CreateCompatibleBitmap(hdc_wnd, w, h)
old = gdi.SelectObject(hdc_mem, hbmp)
ok = u32.PrintWindow(hwnd, hdc_mem, 2)  # PW_RENDERFULLCONTENT

class BMIH(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                ("biSizeImage", ctypes.c_uint32), ("biXPels", ctypes.c_int32),
                ("biYPels", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                ("biClrImportant", ctypes.c_uint32)]
bmi = BMIH(ctypes.sizeof(BMIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
buf = ctypes.create_string_buffer(w * h * 4)
got = gdi.GetDIBits(hdc_mem, hbmp, 0, h, buf, ctypes.byref(bmi), 0)

gdi.SelectObject(hdc_mem, old)
gdi.DeleteObject(hbmp)
gdi.DeleteDC(hdc_mem)
u32.ReleaseDC(hwnd, hdc_wnd)

if not ok or not got:
    print("PrintWindow failed", ok, got); sys.exit(1)
img = Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1).convert("RGB")
# 按 DWM 可见边界裁剪（偏移 = 框架边界 - 窗口矩形原点）
box = (fb.left - r.left, fb.top - r.top, fb.right - r.left, fb.bottom - r.top)
img = img.crop(box)
img.save(r"E:\TRAE Project\SimplePic\docs\screenshot.png")
print("captured", w, "x", h, "-> crop", img.size)
