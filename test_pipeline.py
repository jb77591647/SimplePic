import os, subprocess
MSVC = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
SDK = r'C:\Program Files (x86)\Windows Kits\10'; SDKV = '10.0.26100.0'
ROOT = r'E:\TRAE Project\SimplePic'
env = os.environ.copy()
env['INCLUDE'] = ';'.join([MSVC+r'\include', SDK+r'\Include\%s\shared'%SDKV, SDK+r'\Include\%s\ucrt'%SDKV, SDK+r'\Include\%s\um'%SDKV, ROOT+r'\third_party\stb'])
env['LIB'] = ';'.join([MSVC+r'\lib\x64', SDK+r'\Lib\%s\um\x64'%SDKV, SDK+r'\Lib\%s\ucrt\x64'%SDKV])
env['PATH'] = MSVC+r'\bin\Hostx64\x64;'+env['PATH']
CL = MSVC+r'\bin\Hostx64\x64\cl.exe'
r1 = subprocess.run([CL,'/nologo','/std:c++17','/EHsc','/utf-8','/c',ROOT+r'\tests\pipeline_test.cpp','/Fo'+ROOT+r'\build\pipeline_test.obj'], capture_output=True, text=True, errors='replace', env=env)
print('compile rc:', r1.returncode, (r1.stdout+r1.stderr)[-600:])
objdir = os.path.join(ROOT, 'build')
objs = [os.path.join(objdir, n) for n in ['pipeline_test.obj','pipeline.obj','ops.obj','codec.obj','jpeg_meta.obj']]
r2 = subprocess.run([CL,'/nologo','/Fe'+ROOT+r'\build\pipeline_test.exe','/SUBSYSTEM:CONSOLE'] + objs + ['user32.lib','gdi32.lib','ole32.lib','shlwapi.lib'], capture_output=True, text=True, errors='replace', env=env, cwd=ROOT)
print('link rc:', r2.returncode, (r2.stdout+r2.stderr)[-600:])
if r2.returncode == 0:
    r3 = subprocess.run([ROOT+r'\build\pipeline_test.exe'], capture_output=True, text=True, errors='replace', timeout=60)
    print('TEST rc:', r3.returncode, 'OUT:', r3.stdout.strip())
