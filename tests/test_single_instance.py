# 单实例验证：第一个实例运行中 → 第二个实例应立即退出
import subprocess
import time
import ctypes

exe = r"E:\TRAE Project\SimplePic\SimplePic.exe"

p1 = subprocess.Popen([exe])
time.sleep(3)

t0 = time.time()
p2 = subprocess.Popen([exe])
rc = None
while time.time() - t0 < 10:
    if p2.poll() is not None:
        rc = p2.returncode
        break
    time.sleep(0.2)
dt = time.time() - t0

print("instance1 running :", p1.poll() is None)
print("instance2 exited  :", rc is not None, "code=", rc, "after %.1fs" % dt)

p1.terminate()
time.sleep(0.5)
print("cleanup done")
