# -*- coding: utf-8 -*-
"""SimplePic 端到端 UI 自动化冒烟：注入文件 -> 校验计数 -> 点清空 -> 校验归零"""
import ctypes, ctypes.wintypes as wt
import os, struct, subprocess, tempfile, time

u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32

# 1x1 PNG 测试文件
PNG = bytes.fromhex(
    '89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489'
    '0000000d4944415478da63fcffff3f030005fe02fea72d3f5c0000000049454e44ae426082')
tmp = os.path.join(tempfile.gettempdir(), 'sp_test.png')
open(tmp, 'wb').write(PNG)

p = subprocess.Popen([r'E:\TRAE Project\SimplePic\SimplePic.exe'])
time.sleep(2)

main_hwnd = None
@ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
def enum_main(h, l):
    global main_hwnd
    pid = wt.DWORD()
    u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    if pid.value == p.pid:
        main_hwnd = h
        return False
    return True
u32.EnumWindows(enum_main, 0)
main_hwnd = u32.FindWindowW('SimplePicMain', None)
assert main_hwnd, 'main window not found'

lv = u32.FindWindowExW(main_hwnd, None, 'SysListView32', None)
btn_clear = u32.FindWindowExW(main_hwnd, None, 'Button', '清空列表')
assert lv and btn_clear, f'lv={lv} clear={btn_clear}'

LVM_GETITEMCOUNT = 0x1004
cnt0 = u32.SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0)

# 注入 WM_DROPFILES
k32.GlobalAlloc.restype = ctypes.c_void_p
k32.GlobalAlloc.argtypes = [wt.UINT, ctypes.c_size_t]
k32.GlobalLock.restype = ctypes.c_void_p
k32.GlobalLock.argtypes = [ctypes.c_void_p]
k32.GlobalUnlock.argtypes = [ctypes.c_void_p]
path_data = (tmp + '\0\0').encode('utf-16-le')
hdr = struct.pack('<IiiII', 20, 0, 0, 0, 1)  # pFiles=20, pt, fNC, fWide
GMEM_MOVEABLE, GMEM_ZEROINIT = 0x0002, 0x0040
hglob = k32.GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, len(hdr) + len(path_data))
ptr = k32.GlobalLock(hglob)
ctypes.memmove(ptr, hdr + path_data, len(hdr) + len(path_data))
k32.GlobalUnlock(hglob)
WM_DROPFILES = 0x233
u32.SendMessageW(main_hwnd, WM_DROPFILES, ctypes.c_void_p(hglob), 0)  # DragFinish 会释放内存
time.sleep(1)
cnt1 = u32.SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0)

# 模拟点击清空列表
u32.SendMessageW(btn_clear, 0x00F5, 0, 0)  # BM_CLICK
time.sleep(1)
cnt2 = u32.SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0)

# 再注入一个文件，为管线验证做准备（此处只验证清空，不跑处理）
print(f'init={cnt0} after_drop={cnt1} after_clear={cnt2}')
print('CLEAR_OK' if cnt2 == 0 and cnt1 > cnt0 else 'CLEAR_FAIL')

subprocess.run(['taskkill', '/PID', str(p.pid), '/F'], capture_output=True)
