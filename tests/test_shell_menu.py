# 端到端验证右键菜单 v3（* + AppliesTo）：写上一版遗留键 → 启动 exe → 检查迁移结果
import winreg
import subprocess
import time

HKCU = winreg.HKEY_CURRENT_USER
EXTS = [".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".tif", ".tiff",
        ".pdf", ".heic", ".heif", ".hif", ".avif"]


def has(path):
    try:
        winreg.OpenKey(HKCU, path).Close()
        return True
    except OSError:
        return False


def query(path, name=None):
    k = winreg.OpenKey(HKCU, path)
    v = winreg.QueryValueEx(k, name)[0]
    k.Close()
    return v


# 1) 模拟上一版状态：按扩展名注册（图片菜单不显示的那版）
for e in EXTS:
    k = winreg.CreateKey(HKCU, r"Software\Classes\%s\shell\SimplePic\command" % e)
    winreg.SetValueEx(k, None, 0, winreg.REG_SZ, '"C:\\fake\\v2.exe" "%1"')
    k.Close()
print("v2 per-ext keys written")

# 2) 启动 exe（启动时 migrate_shell_menu 自愈）
exe = r"E:\TRAE Project\SimplePic\SimplePic.exe"
p = subprocess.Popen([exe])
time.sleep(4)

# 3) 检查迁移结果
print("v2 per-ext keys removed :", all(not has(r"Software\Classes\%s\shell\SimplePic" % e) for e in EXTS))
print("new *\\shell\\SimplePic  :", has(r"Software\Classes\*\shell\SimplePic"))
print("dir key still there     :", has(r"Software\Classes\Directory\shell\SimplePic"))
print(".txt NOT registered     :", not has(r"Software\Classes\.txt\shell\SimplePic"))

try:
    base = r"Software\Classes\*\shell\SimplePic"
    print("label     :", query(base))
    print("icon      :", query(base, "Icon"))
    at = query(base, "AppliesTo")
    print("AppliesTo :", at[:80] + ("..." if len(at) > 80 else ""))
    print("AppliesTo ok:", all('"%s"' % e in at.replace('"*', '"') or '*%s"' % e in at for e in [".jpg", ".pdf", ".avif"]))
    print("command   :", query(base + r"\command"))
except OSError as e:
    print("read fail:", e)

p.terminate()
time.sleep(0.5)
print("app terminated")
