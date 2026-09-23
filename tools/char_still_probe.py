# One-off visual probe v2: same launch, but find the game window by title, bring
# it to the foreground, and grab its client rect - other windows on the desktop
# must not occlude the verdict.

import ctypes
import os
import subprocess
import sys
import time

PROJ = r"E:\Project\UE5+ai"
UE = r"E:\Project\UE5\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"
OUT = os.path.join(PROJ, "logs", "watch_probe2.png")
LOG = os.path.join(PROJ, "logs", "watch_probe2.log")

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()

ENUM_PROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
found = []

def on_window(hwnd, _lparam):
    if user32.IsWindowVisible(hwnd):
        cls = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, cls, 256)
        if cls.value == "UnrealWindow":
            buf = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, buf, 256)
            found.append((hwnd, buf.value))
    return True

cmd = [
    UE,
    os.path.join(PROJ, "PursuitAI.uproject"),
    "/Game/Maps/L_PursuitCharTrain",
    "-game", "-windowed", "-resx=1280", "-resy=720",
    "-WinX=0", "-WinY=0",
    "-PursuitCharGreedy", "-PursuitCharSeed=1", "-PursuitCharQuitAfter=120",
    "-abslog=%s" % LOG,
]
proc = subprocess.Popen(cmd, cwd=PROJ)
print("launched pid", proc.pid)

time.sleep(45.0)  # editor cold start

user32.EnumWindows(ENUM_PROC(on_window), None)
print("windows:", found)
if not found:
    print("VERDICT: NO WINDOW")
    proc.kill()
    sys.exit(1)

hwnd = int(found[0][0])
# Force the window on top of everything: SetForegroundWindow alone loses to the
# Windows foreground lock when the caller is a background process, and ImageGrab
# grabs whatever pixels are actually on screen.
user32.ShowWindow(hwnd, 9)  # SW_RESTORE
user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0053)  # HWND_TOPMOST, NOSIZE|NOMOVE|SHOWWINDOW
time.sleep(1.5)
rect = ctypes.wintypes_RECT = type("R", (ctypes.Structure,), {"_fields_": [
    ("left", ctypes.c_long), ("top", ctypes.c_long),
    ("right", ctypes.c_long), ("bottom", ctypes.c_long)]})()
user32.GetWindowRect(hwnd, ctypes.byref(rect))
w, h = rect.right - rect.left, rect.bottom - rect.top
print("rect", w, h)

from PIL import ImageGrab  # noqa: E402
OUT2 = os.path.join(PROJ, "logs", "watch_probe3.png")


def grab(tag):
    r = type("R", (ctypes.Structure,), {"_fields_": [
        ("left", ctypes.c_long), ("top", ctypes.c_long),
        ("right", ctypes.c_long), ("bottom", ctypes.c_long)]})()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    # Screen grab of the window rect; the window is HWND_TOPMOST, so nothing occludes it.
    img = ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom))
    path = OUT if tag == 1 else OUT2
    img.save(path)
    print("grabbed %dx%d" % (img.width, img.height))
    return img


img1 = grab(1)
time.sleep(4.0)
img2 = grab(2)
# Motion metric: mean absolute luma difference on a downscale (cheap but decisive -
# a frozen scene diffs ~0, a chase diffs by tens).
small1 = img1.convert("L").resize((128, 72))
small2 = img2.convert("L").resize((128, 72))
diffs = [abs(a - b) for a, b in zip(small1.getdata(), small2.getdata())]
mad = sum(diffs) / len(diffs)
moved_px = sum(1 for d in diffs if d > 8)
print("motion: mean-abs-diff %.1f, pixels-changed %.1f%%" % (mad, 100.0 * moved_px / len(diffs)))
print("VERDICT:", "MOVING" if mad > 2.0 else "FROZEN")

try:
    proc.wait(timeout=60)
except subprocess.TimeoutExpired:
    proc.kill()
print("editor exited")
