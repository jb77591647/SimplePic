# -*- coding: utf-8 -*-
# 端到端验证：向主窗口客户区坐标发 WM_LBUTTONDOWN/UP，检查 ListView 是否切到图标视图
import ctypes, ctypes.wintypes as wt, subprocess, time, sys

u32 = ctypes.windll.user32
g32 = ctypes.windll.kernel32

# 本进程必须 DPI-aware，否则客户区坐标会被系统虚拟化缩放（×dpi/96），
# 发送的点击全部偏移（先前测试失真的根因）
try:
    u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # PER_MONITOR_AWARE_V2
except Exception:
    try: u32.SetProcessDPIAware()
    except Exception: pass

WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP   = 0x0202
MK_LBUTTON     = 0x0001

def find_main():
    return u32.FindWindowW(u32.CreateUnicodeBuffer if False else None, None) if False else u32.FindWindowW(None, "SimplePic 图片处理系统 v1.0")

def muldiv(x, dpi): return x * dpi // 96

def main():
    exe = r"E:\TRAE Project\SimplePic\SimplePic.exe"
    subprocess.Popen([exe])
    hwnd = None
    for _ in range(50):
        time.sleep(0.2)
        hwnd = find_main()
        if hwnd: break
    if not hwnd:
        print("FAIL: window not found"); return
    time.sleep(1.0)

    # 找 ListView 子控件
    lv = None
    def enumcb(h, lparam):
        nonlocal lv
        buf = ctypes.create_unicode_buffer(64)
        u32.GetClassNameW(h, buf, 64)
        if buf.value == "SysListView32": lv = h
        return True
    CBENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    u32.EnumChildWindows(hwnd, CBENUMPROC(0) and 0 or 0, 0) if False else None
    # EnumChildWindows 需要真实回调
    cb = CBENUMPROC(enumcb)
    u32.EnumChildWindows(hwnd, cb, 0)
    if not lv:
        print("FAIL: listview not found"); return

    style = u32.GetWindowLongW(lv, -16)  # GWL_STYLE
    print("initial style type:", style & 0x0003, "(0x1=REPORT 0x0=ICON)")

    dpi = u32.GetDpiForWindow(hwnd) if hasattr(u32, "GetDpiForWindow") else 96
    if not dpi: dpi = 96
    print("dpi:", dpi)
    S = lambda x: muldiv(x, dpi)

    rc = wt.RECT(); u32.GetClientRect(hwnd, ctypes.byref(rc))
    W, H = rc.right, rc.bottom
    print("client:", W, H)

    side_w, panel_w = S(210), S(290)
    header_h, stats_h, toolbar_h = S(56), S(68), S(84)
    red_right = W - panel_w - S(10)
    bw, gap = S(96), S(6)
    ty = header_h + stats_h + S(10)
    bh = S(32)

    # 依次点 缩略图小/中/大（i=1,2,3），确认样式变为 ICON(0x3)
    for i in (1, 2, 3):
        r = red_right - (3 - i) * (bw + gap)
        x = r - bw + bw // 2
        y = ty + bh // 2
        lp = (y << 16) | (x & 0xFFFF)
        u32.SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp)
        u32.SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp)  # 重复一次防边缘
        u32.SendMessageW(hwnd, WM_LBUTTONUP, 0, lp)
        time.sleep(0.6)
        st = u32.GetWindowLongW(lv, -16) & 0x0003
        print(f"click seg{i} at ({x},{y}) -> type {st} ({'ICON OK' if st == 0x0 else 'STILL ' + hex(st)})")

    # 点回列表
    r = red_right - 3 * (bw + gap)
    x, y = r - bw + bw // 2, ty + bh // 2
    lp = (y << 16) | (x & 0xFFFF)
    u32.SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp)
    u32.SendMessageW(hwnd, WM_LBUTTONUP, 0, lp)
    time.sleep(0.6)
    st = u32.GetWindowLongW(lv, -16) & 0x0003
    print("back to list:", "OK" if st == 0x1 else "FAIL " + hex(st))

    u32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
    time.sleep(0.8)
    subprocess.run(["taskkill", "/IM", "SimplePic.exe", "/F"], capture_output=True)

main()
