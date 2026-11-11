# -*- coding: utf-8 -*-
# 编译并运行 PDF 解码回归测试（复用主构建的 codec.obj / webp obj）
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
    SDK + r"\Include\%s\cppwinrt" % SDKV,
    SDK + r"\Include\%s\winrt" % SDKV,
])
env["LIB"] = ";".join([
    MSVC + r"\lib\x64",
    SDK + r"\Lib\%s\um\x64" % SDKV,
    SDK + r"\Lib\%s\ucrt\x64" % SDKV,
])
env["PATH"] = MSVC + r"\bin\Hostx64\x64;" + env["PATH"]

os.chdir(ROOT)

cmd = [CL, "/nologo", "/std:c++17", "/EHsc", "/W3", "/O2", "/MT", "/utf-8",
       "/DUNICODE", "/D_UNICODE", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
       "/D_CRT_SECURE_NO_WARNINGS",
       "/I" + ROOT + r"\third_party\stb",
       "/I" + ROOT + r"\third_party\webp\src",
       "/I" + ROOT + r"\src",
       r"tests\pdf_test.cpp",
       r"build\codec.obj", r"build\jpeg_meta.obj",
       "/Fetests\\out\\pdf_test.exe",
       "/link", "/SUBSYSTEM:CONSOLE"]
webp_objs = sorted(glob.glob(os.path.join(ROOT, "build", "webp", "*.obj")))
cmd = cmd[:cmd.index("/link")] + webp_objs + cmd[cmd.index("/link"):]
cmd += ["user32.lib", "gdi32.lib", "comctl32.lib", "comdlg32.lib",
        "shell32.lib", "ole32.lib", "shlwapi.lib", "windowscodecs.lib",
        "WindowsApp.lib"]

p = subprocess.run(cmd, capture_output=True, text=True, errors="replace", env=env, cwd=ROOT)
print(p.stdout[-3000:] if p.stdout else "")
print(p.stderr[-3000:] if p.stderr else "")
print("compile exit =", p.returncode)
if p.returncode == 0:
    os.makedirs(os.path.join(ROOT, "tests", "out"), exist_ok=True)
    r = subprocess.run([os.path.join(ROOT, "tests", "out", "pdf_test.exe")],
                       capture_output=True, text=True, errors="replace", cwd=ROOT)
    print(r.stdout)
    print("test exit =", r.returncode)
sys.exit(p.returncode)
