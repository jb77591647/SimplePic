import subprocess, os, sys
sys.path.insert(0, r"E:\TRAE Project\SimplePic")
ROOT = r"E:\TRAE Project\SimplePic"
MSVC = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
SDK = r"C:\Program Files (x86)\Windows Kits\10"; SDKV = "10.0.26100.0"
CL = MSVC + r"\bin\Hostx64\x64\cl.exe"
env = os.environ.copy()
env["INCLUDE"] = ";".join([MSVC + r"\include", SDK + r"\Include\%s\shared" % SDKV, SDK + r"\Include\%s\ucrt" % SDKV, SDK + r"\Include\%s\um" % SDKV])
env["LIB"] = ";".join([MSVC + r"\lib\x64", SDK + r"\Lib\%s\um\x64" % SDKV, SDK + r"\Lib\%s\ucrt\x64" % SDKV])
env["PATH"] = MSVC + r"\bin\Hostx64\x64;" + env["PATH"]
os.chdir(ROOT)
r = subprocess.run([CL, "/nologo", "/std:c++17", "/EHsc", "/W3", "/MT", "/utf-8", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/D_CRT_SECURE_NO_WARNINGS", "/DUNICODE", "/D_UNICODE", r"/I"+ROOT+r"\third_party\stb", r"/I"+ROOT+r"\third_party\webp\src", r"/I"+ROOT+r"\src", "/c", r"tests\dpi_test.cpp", r"/Fobuild\dpi_test.obj"], capture_output=True, env=env)
r.stdout = r.stdout.decode("gbk", "replace"); r.stderr = r.stderr.decode("gbk", "replace")
print("compile rc=", r.returncode); print(r.stderr[-1500:] if r.returncode else "")
if r.returncode == 0:
    import glob as _g
    objs = [ROOT + "\\build\\" + n + ".obj" for n in ["dpi_test","codec","jpeg_meta","ops","pipeline"]]
    objs += sorted(_g.glob(ROOT + r"\build\webp\*.obj"))
    r2 = subprocess.run([CL, "/nologo"] + objs + ["/Fe" + ROOT + r"\build\dpi_test.exe", "/link", "/SUBSYSTEM:CONSOLE", "user32.lib", "gdi32.lib", "comctl32.lib", "comdlg32.lib", "shell32.lib", "ole32.lib", "shlwapi.lib", "advapi32.lib", "windowscodecs.lib"], capture_output=True, env=env)
    r2.stdout = r2.stdout.decode("gbk", "replace"); r2.stderr = r2.stderr.decode("gbk", "replace")
    print("link rc=", r2.returncode)
    print("STDOUT:", r2.stdout[-2500:])
    print("STDERR:", r2.stderr[-2500:])
