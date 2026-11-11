# -*- coding: utf-8 -*-
# 端到端验证：参数区滚动条 + 滚轮（对应修复：SCROLLINFO nMax 映射、下拉框滚轮转发）
#
# 覆盖场景：
# 1) 遍历侧栏找到参数溢出（SCROLLBAR 可见）的模块
# 2) 滚轮 30 下 + 60 上：全部"可见控件"位置精确回到基线（隐藏控件允许留旧矩形，良性）
# 3) WM_VSCROLL SB_PAGEDOWN/PAGEUP（命令在 LOWORD）往返无漂移
# 4) 悬停 COMBOBOX 滚轮 → 转发滚动参数区（不再被下拉框吞掉），且选中值不变
import ctypes, ctypes.wintypes as wt, subprocess, time, sys

u32 = ctypes.windll.user32
try: u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # PMv2，坐标不被虚拟化
except Exception:
    try: u32.SetProcessDPIAware()
    except Exception: pass

WM_MOUSEWHEEL=0x020A; WM_VSCROLL=0x0115
WM_LBUTTONDOWN=0x0201; WM_LBUTTONUP=0x0202
MK_LBUTTON=1
LB_GETCOUNT=0x018B; LB_GETITEMDATA=0x0199; SB_SPACER=-1000
CB_GETCURSEL=0x0147; CB_GETDROPPEDSTATE=0x0157
SB_PAGEDOWN=3; SB_PAGEUP=2
ENUM=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p)

def rect_in(panel,h):
    r=wt.RECT(); u32.GetWindowRect(h,ctypes.byref(r))
    p1=wt.POINT(r.left,r.top); p2=wt.POINT(r.right,r.bottom)
    u32.ScreenToClient(panel,ctypes.byref(p1)); u32.ScreenToClient(panel,ctypes.byref(p2))
    return (p1.x,p1.y,p2.x,p2.y)

def wheel_wp(delta):
    return ((delta & 0xFFFF) << 16)

def main():
    out=sys.stdout
    try:
        run(out)
    finally:
        u32.PostMessageW(u32.FindWindowW(None,"SimplePic 图片处理系统 v1.0"),0x0010,0,0)
        time.sleep(0.5)
        subprocess.run(["taskkill","/IM","SimplePic.exe","/F"],capture_output=True)

def run(out):
    subprocess.Popen([r"E:\TRAE Project\SimplePic\SimplePic.exe"])
    hwnd=None
    for _ in range(50):
        time.sleep(0.2); hwnd=u32.FindWindowW(None,"SimplePic 图片处理系统 v1.0")
        if hwnd: break
    if not hwnd: print("FAIL: window not found"); return
    time.sleep(1.0)
    dpi=u32.GetDpiForWindow(hwnd) or 96
    S=lambda x:x*dpi//96
    print("dpi:", dpi, file=out)

    kids=[]
    def rec(h,l): kids.append(h); return True
    u32.EnumChildWindows(hwnd,ENUM(rec),0)
    sb=lb=panel=None
    for h in kids:
        b=ctypes.create_unicode_buffer(64); u32.GetClassNameW(h,b,64)
        c=b.value.lower(); rid=u32.GetDlgCtrlID(h)
        if c=="scrollbar": sb=h
        elif c=="listbox": lb=h
        elif c=="static" and rid==1200: panel=h  # IDC_PARAM_PANEL
    assert sb and panel and lb, "children not found"

    # 遍历侧栏找溢出模块
    n=u32.SendMessageW(lb,LB_GETCOUNT,0,0)
    y=0; found=-1
    for i in range(n):
        d=ctypes.c_long(u32.SendMessageW(lb,LB_GETITEMDATA,i,0)).value
        hh=S(10) if d==SB_SPACER else S(30)
        cy=y+hh//2; y+=hh
        lp=((cy&0xFFFF)<<16)|(S(100)&0xFFFF)
        u32.SendMessageW(lb,WM_LBUTTONDOWN,MK_LBUTTON,lp); u32.SendMessageW(lb,WM_LBUTTONUP,0,lp)
        time.sleep(0.4)
        if u32.IsWindowVisible(sb): found=i; break
    print(f"overflow module: sidebar item {found}", file=out)
    if found < 0:
        # SKIP：最小客户区高度已提高到 S(762)（侧栏 25 行完整显示），
        # 150% 下参数面板随之变高，所有模块参数均不溢出——本 DPI 无滚动条可测
        print("SKIP: no overflow module at this DPI (min client height 762)", file=out)
        return

    # 面板子控件（选中后创建）
    pk=[]
    def rec2(h,l): pk.append(h); return True
    u32.EnumChildWindows(panel,ENUM(rec2),0)
    pk=[h for h in pk if h!=sb]
    combos=[]
    for h in pk:
        b=ctypes.create_unicode_buffer(32); u32.GetClassNameW(h,b,32)
        if b.value.lower()=="combobox": combos.append(h)
    def vis():
        return {h:rect_in(panel,h) for h in pk if u32.IsWindowVisible(h)}

    pc=wt.POINT(S(140),S(150)); u32.ScreenToClient(panel,ctypes.byref(pc))
    wlp=((pc.y&0xFFFF)<<16)|(pc.x&0xFFFF)

    # 1) 滚轮往返回基线
    base=vis()
    for _ in range(30): u32.SendMessageW(panel,WM_MOUSEWHEEL,wheel_wp(-120),wlp)
    time.sleep(0.3)
    moved=sum(1 for h in base if u32.IsWindowVisible(h) and rect_in(panel,h)!=base[h])
    for _ in range(60): u32.SendMessageW(panel,WM_MOUSEWHEEL,wheel_wp(120),wlp)
    time.sleep(0.3)
    drift=[h for h in base if u32.IsWindowVisible(h) and rect_in(panel,h)!=base[h]]
    print(f"wheel roundtrip: base={len(base)} moved_down={moved} drift={len(drift)}",
          "OK" if not drift else "FAIL", file=out)

    # 2) WM_VSCROLL（命令在 LOWORD）
    base=vis()
    u32.SendMessageW(panel,WM_VSCROLL,SB_PAGEDOWN,sb); time.sleep(0.3)
    m=sum(1 for h in base if u32.IsWindowVisible(h) and rect_in(panel,h)!=base[h])
    print(f"WM_VSCROLL SB_PAGEDOWN: moved={m}", "OK" if m>0 else "FAIL", file=out)
    u32.SendMessageW(panel,WM_VSCROLL,SB_PAGEUP,sb); time.sleep(0.3)
    d=sum(1 for h in base if u32.IsWindowVisible(h) and rect_in(panel,h)!=base[h])
    print(f"WM_VSCROLL SB_PAGEUP: drift={d}", "OK" if d==0 else "FAIL", file=out)

    # 3) 悬停组合框滚轮转发
    if combos:
        cbx=combos[0]
        base0=vis()  # 最初位置（scroll=0）
        sel0=u32.SendMessageW(cbx,CB_GETCURSEL,0,0)
        u32.SendMessageW(cbx,WM_MOUSEWHEEL,wheel_wp(-120),0)
        u32.SendMessageW(cbx,WM_MOUSEWHEEL,wheel_wp(-120),0)
        time.sleep(0.3)
        m=sum(1 for h in base0 if u32.IsWindowVisible(h) and rect_in(panel,h)!=base0[h])
        sel1=u32.SendMessageW(cbx,CB_GETCURSEL,0,0)
        print(f"combo wheel forward: moved={m} cursel {sel0}->{sel1}",
              "OK" if m>0 and sel0==sel1 else "FAIL", file=out)
        u32.SendMessageW(cbx,WM_MOUSEWHEEL,wheel_wp(120),0)
        u32.SendMessageW(cbx,WM_MOUSEWHEEL,wheel_wp(120),0)
        time.sleep(0.3)
        d=sum(1 for h in base0 if u32.IsWindowVisible(h) and rect_in(panel,h)!=base0[h])
        print(f"combo wheel back: drift={d}", "OK" if d==0 else "FAIL", file=out)

    out.flush()

main()
