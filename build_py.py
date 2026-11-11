# -*- coding: utf-8 -*-
# SimplePic 构建脚本：直接调用 MSVC cl.exe（绕开损坏的 shell 环境）
import glob
import os
import subprocess
import sys

ROOT = r"E:\TRAE Project\SimplePic"
MSVC = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
SDK = r"C:\Program Files (x86)\Windows Kits\10"
SDKV = "10.0.26100.0"
CL = MSVC + r"\bin\Hostx64\x64\cl.exe"

env = os.environ.copy()
env["INCLUDE"] = ";".join([
    MSVC + r"\include",
    SDK + r"\Include\%s\shared" % SDKV,
    SDK + r"\Include\%s\ucrt" % SDKV,
    SDK + r"\Include\%s\um" % SDKV,
    SDK + r"\Include\%s\cppwinrt" % SDKV,  # C++/WinRT 头（Windows.Data.Pdf）
    SDK + r"\Include\%s\winrt" % SDKV,     # roapi.h（RoInitialize）所在目录
])
env["LIB"] = ";".join([
    MSVC + r"\lib\x64",
    SDK + r"\Lib\%s\um\x64" % SDKV,
    SDK + r"\Lib\%s\ucrt\x64" % SDKV,
])
env["PATH"] = MSVC + r"\bin\Hostx64\x64;" + env["PATH"]

os.chdir(ROOT)
os.makedirs("build", exist_ok=True)

BASE = [
    "/nologo", "/std:c++17", "/EHsc", "/W3", "/O2", "/MT", "/utf-8",
    "/DUNICODE", "/D_UNICODE", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
    "/D_CRT_SECURE_NO_WARNINGS",
    "/I" + ROOT + r"\third_party\stb",
    "/I" + ROOT + r"\third_party\webp\src",
    "/I" + ROOT + r"\src",
]

SRCS = [
    "src/main.cpp",
    "src/codec/codec.cpp",
    "src/codec/jpeg_meta.cpp",
    "src/ops/ops.cpp",
    "src/pipeline/pipeline.cpp",
    "src/ui/main_window.cpp",
]

# libwebp（WebP 编解码：有损/无损编码 + 全格式解码）
WEBP = ROOT + r"\third_party\webp"
WEBP_SRCS = []
for d in (r"\sharpyuv", r"\src\dec", r"\src\dsp", r"\src\enc", r"\src\utils"):
    WEBP_SRCS += sorted(glob.glob(WEBP + d + r"\*.c"))
WEBP_BASE = [
    "/nologo", "/O2", "/MT", "/W1",
    "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/D_CRT_SECURE_NO_WARNINGS",
    "/I" + WEBP,          # libwebp 源码内部用 "src/webp/xxx.h" 引用
    "/I" + WEBP + r"\src", # 对外头文件在 src/webp/
]

def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace", env=env, cwd=ROOT)
    out = (p.stdout or "").strip()
    err = (p.stderr or "").strip()
    return p.returncode, out, err

def show(tag, rc, out, err):
    if out:
        print("[%s stdout]\n%s" % (tag, out[-4000:]))
    if err:
        print("[%s stderr]\n%s" % (tag, err[-6000:]))
    print("[%s] exit=%s" % (tag, rc))
    sys.stdout.flush()

objs = []
webp_objs = []
failed = False

# 先编 libwebp（一次性批量编译，obj 落到 build\webp\）
os.makedirs(os.path.join(ROOT, "build", "webp"), exist_ok=True)
rc, out, err = run([CL] + WEBP_BASE + ["/c"] + WEBP_SRCS + ["/Fo" + os.path.join(ROOT, "build", "webp") + "\\"])
show("libwebp (%d files)" % len(WEBP_SRCS), rc, out, err)
if rc != 0:
    failed = True
else:
    webp_objs = sorted(glob.glob(os.path.join(ROOT, "build", "webp", "*.obj")))

for src in SRCS:
    if failed:
        break
    obj = "build/" + os.path.splitext(os.path.basename(src))[0] + ".obj"
    rc, out, err = run([CL] + BASE + ["/c", src, "/Fo" + obj])
    show(src, rc, out, err)
    if rc != 0:
        failed = True
        break
    objs.append(obj)

if not failed:
    # 应用图标资源：rc.exe 编译 app.rc（1 ICON app.ico）→ app.res 参与链接
    RC = SDK + r"\bin\%s\x64\rc.exe" % SDKV
    if os.path.exists(RC):
        rc, out, err = run([RC, "/nologo", "/fo" + os.path.join(ROOT, "build", "app.res"),
                            os.path.join(ROOT, "src", "app.rc")])
        show("rc", rc, out, err)
        if rc != 0:
            failed = True

if not failed:
    LIBS = ["user32.lib", "gdi32.lib", "comctl32.lib", "comdlg32.lib",
            "shell32.lib", "ole32.lib", "msimg32.lib", "shlwapi.lib", "windowscodecs.lib",
            "WindowsApp.lib"]  # WinRT：RoInitialize/RoGetActivationFactory（Windows.Data.Pdf）
    rc, out, err = run([CL, "/nologo"] + objs + webp_objs +
                       [os.path.join(ROOT, "build", "app.res")] +
                       ["/FeSimplePic.exe", "/link", "/SUBSYSTEM:WINDOWS"] + LIBS)
    show("link", rc, out, err)
    if rc == 0:
        # 嵌入 manifest（失败不致命）
        MT = SDK + r"\bin\%s\x64\mt.exe" % SDKV
        if os.path.exists(MT):
            rc2, out2, err2 = run([MT, "-nologo", "-manifest", "src/app.manifest",
                                   "-outputresource:SimplePic.exe;1"])
            show("mt", rc2, out2, err2)
        exe = os.path.join(ROOT, "SimplePic.exe")
        print("BUILD OK:", exe, os.path.getsize(exe), "bytes" if os.path.exists(exe) else "MISSING")
