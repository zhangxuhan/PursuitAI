# Kills the processes that own windows whose title contains a substring.
#
#   python tools/kill_window.py PursuitAI
#
# Why this exists rather than taskkill /IM: `UnrealEditor.exe -game` forks a second
# UnrealEditor.exe for the game, so killing by image name takes the editor down with it if one
# happens to be open. Matching on the window title leaves the editor alone, because the editor
# window is titled "<project> - Unreal Editor" and the game's window is titled "<project>".
#
# Exits 0 whether or not anything matched - a script that fails because the thing it was
# cleaning up had already exited is not useful.

import ctypes
import ctypes.wintypes as win
import sys

USER32 = ctypes.windll.user32
KERNEL32 = ctypes.windll.kernel32

PROCESS_TERMINATE = 0x0001


def windows_matching(substring, exclude):
    found = []

    def callback(hwnd, _lparam):
        if not USER32.IsWindowVisible(hwnd):
            return True
        length = USER32.GetWindowTextLengthW(hwnd)
        if length == 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        USER32.GetWindowTextW(hwnd, buf, length + 1)
        title = buf.value
        if substring in title and (not exclude or exclude not in title):
            pid = win.DWORD()
            USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            found.append((title, pid.value))
        return True

    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, win.HWND, win.LPARAM)
    USER32.EnumWindows(callback_type(callback), 0)
    return found


def main():
    if len(sys.argv) < 2:
        print('usage: kill_window.py <title substring> [exclude substring]')
        return 2

    substring = sys.argv[1]
    exclude = sys.argv[2] if len(sys.argv) > 2 else 'Unreal Editor'

    matches = windows_matching(substring, exclude)
    if not matches:
        print('no window matching %r' % substring)
        return 0

    for title, pid in matches:
        handle = KERNEL32.OpenProcess(PROCESS_TERMINATE, False, pid)
        if not handle:
            print('could not open pid %d (%r) - already gone?' % (pid, title))
            continue
        ok = KERNEL32.TerminateProcess(handle, 0)
        KERNEL32.CloseHandle(handle)
        print('%s pid %d (%r)' % ('killed' if ok else 'FAILED to kill', pid, title))

    return 0


if __name__ == '__main__':
    sys.exit(main())
