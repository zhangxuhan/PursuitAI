# Copyright Epic Games, Inc. All Rights Reserved.
#
# Record an Unreal game window to an mp4, without OBS and without a capture SDK.
#
#   ./venv/Scripts/python.exe tools/record_window.py --seconds 20 --fps 30 \
#       --output logs/portfolio/city.mp4
#
# Two capture engines, and the default is the second one:
#
#   gdigrab  ffmpeg grabs the screen itself and encodes in the same process. Frames carry
#            wall-clock timestamps, so the clip's duration IS the recording's duration. This
#            is the default because it is the only engine that makes that guarantee.
#
#   png      Pillow's ImageGrab writes a PNG per frame, then ffmpeg turns the sequence into
#            a clip. Kept because it needs no ffmpeg input device and because a frame
#            sequence is easier to inspect afterwards - but see the retiming note below.
#
# Why gdigrab is the default, in one measurement: a 1280x720 PNG grab+save costs about
# 100 ms, so asking for 20 fps actually delivers ~10.1 fps. The old code then encoded the
# 600 frames it got at the 20 fps it was asked for, and the "30 second" clip was 59 s of
# action played back in 30 - a silent 2x speed-up, which is exactly what makes a video look
# like a physics bug rather than a recording bug. Encoding at the measured rate fixes that
# (and that fix is still in the png path below), but a real 30 fps capture is better than a
# correctly-retimed 10 fps one, so gdigrab goes first and png stays as the fallback.
#
# Caveats worth knowing before you record:
#   * Both engines capture what is DISPLAYED. Anything covering the window ends up in the
#     video, and a topmost overlay (a desktop lyric widget, an FPS overlay) is covering it
#     by definition. The script raises the window first and checks the foreground, but it
#     cannot beat a WS_EX_TOPMOST window - tools/record_city.py moves the game out from
#     under those before it starts, which is the fix that actually works.
#   * Only the client area is captured; the OS title bar and borders are cropped off using
#     the window's own rect, which is where the 8/31 pixel insets come from.
#   * The mouse cursor is not drawn by default.
#   * 1280x720 unless you pass --fps/--resx... no: the client area measured from the window
#     decides it, and it is rounded down to an even width and height because yuv420p
#     requires even dimensions. gdigrab fails outright on an odd one.
#
# One frame instead of a clip - the fastest way to check what a scene looks like:
#   ./venv/Scripts/python.exe tools/record_window.py --still logs/shot.png
#   ./venv/Scripts/python.exe tools/record_window.py --still logs/shot.png --title PursuitAI
#
# The self-playing chase on the Cartoon City map, which is the intended use:
#   ./venv/Scripts/python.exe tools/record_city.py --seconds 30
#
# Requires: Pillow in the venv (for --still and the png engine), and ffmpeg on PATH
# (or pass --ffmpeg).

import argparse
import ctypes
import ctypes.wintypes as win
import os
import shutil
import subprocess
import sys
import time

from PIL import ImageGrab

USER32 = ctypes.windll.user32

# Window frame insets: Windows draws an 8 px border and a 31 px title bar around the
# client area at the default DPI, and only the client area is the game.
BORDER_PX = 8
TITLEBAR_PX = 31

SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002


def find_window(title_substring, exclude='Unreal Editor'):
    """Return [(hwnd, left, top, right, bottom)] for visible windows whose title contains the string."""
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
        if title_substring in title and (not exclude or exclude not in title):
            rect = win.RECT()
            USER32.GetWindowRect(hwnd, ctypes.byref(rect))
            found.append((hwnd, rect.left, rect.top, rect.right, rect.bottom))
        return True

    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, win.HWND, win.LPARAM)
    USER32.EnumWindows(callback_type(callback), 0)
    return found


def client_rect(window):
    """The window's client area in screen coordinates, rounded to even dimensions.

    Even, because every encoder that produces yuv420p - which is what browsers and phones
    play - needs even width and height, and gdigrab refuses an odd video_size outright
    rather than rounding for you.
    """
    _hwnd, left, top, right, bottom = window
    x = left + BORDER_PX
    y = top + TITLEBAR_PX
    w = (right - BORDER_PX) - x
    h = (bottom - BORDER_PX) - y
    return (x, y, w - (w % 2), h - (h % 2))


def raise_window(hwnd):
    """Bring the window to the front so a screen grab actually sees it.

    SetForegroundWindow on its own is not enough, and it fails in the worst possible way: Windows
    refuses to let a background process steal the foreground, the call returns without doing
    anything, and ImageGrab then politely captures whatever window was in front instead - so the
    "screenshot of the game" comes out as a picture of the browser next to it. Minimising and
    restoring first makes the shell treat the window as one the user just interacted with, and
    the raise takes.

    HWND_TOPMOST is deliberately not used: the window should come forward for the grab, not stay
    pinned above everything for the rest of the session.
    """
    SW_MINIMIZE = 6
    SW_RESTORE = 9

    USER32.ShowWindow(hwnd, SW_MINIMIZE)
    USER32.ShowWindow(hwnd, SW_RESTORE)
    USER32.SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE)  # 0 == HWND_TOP
    USER32.SetForegroundWindow(hwnd)


def grab_frames(rect, seconds, fps, out_dir):
    """PNG engine: grab `seconds * fps` frames with Pillow.

    Returns (frame_count, effective_fps). The effective rate is measured and handed back
    rather than assumed, because the encoder has to be told what actually happened - see
    the module header. Asking for more fps than the machine can deliver does not produce a
    slow-motion clip, it produces a fast one.
    """
    os.makedirs(out_dir, exist_ok=True)

    # Only clear frames this script could have written. A blanket '*.png' delete is what
    # tripped the host's bulk-delete guard once (SAFE_DELETE_BULK_CONFIRM_REQUIRED, count
    # 801 / threshold 50) and made a *successful* recording exit 1, because the mp4 is
    # written before the frames are cleaned up.
    for name in os.listdir(out_dir):
        if name.startswith('f') and name.endswith('.png'):
            os.remove(os.path.join(out_dir, name))

    x, y, _w, _h = rect
    bbox = (x, y, x + _w, y + _h)

    total = int(round(seconds * fps))
    start = time.time()
    for i in range(total):
        # Pace against an absolute clock rather than sleeping a fixed interval, so a
        # slow grab does not make the clip drift longer than requested.
        target = start + i / fps
        ImageGrab.grab(bbox=bbox).save(os.path.join(out_dir, 'f%05d.png' % i))
        slack = target + 1.0 / fps - time.time()
        if slack > 0:
            time.sleep(slack)

    elapsed = max(time.time() - start, 1e-6)
    effective = total / elapsed
    print('grabbed %d frames in %.1fs (%.1f fps effective, asked for %d)'
          % (total, elapsed, effective, fps))
    return total, effective


def capture_gdigrab(ffmpeg, rect, seconds, fps, output, quality):
    """gdigrab engine: ffmpeg grabs and encodes in one pass.

    No frame sequence, no retiming, and the duration of the file is the duration of the
    recording even when the machine cannot hold the requested rate - gdigrab timestamps
    every frame from the wall clock, so a dropped frame shows up as a gap rather than as
    the whole clip running fast.
    """
    x, y, w, h = rect
    command = [
        ffmpeg, '-y', '-loglevel', 'error',
        '-f', 'gdigrab',
        '-framerate', str(fps),
        '-offset_x', str(x), '-offset_y', str(y),
        '-video_size', '%dx%d' % (w, h),
        '-i', 'desktop',
        '-t', '%.3f' % seconds,
        '-c:v', 'libx264',
        '-preset', 'veryfast',
        '-pix_fmt', 'yuv420p',   # what browsers and phones actually play
        '-crf', str(quality),
        '-movflags', '+faststart',
        output,
    ]
    print('gdigrab: %dx%d at (%d, %d), %d fps, %.1f s'
          % (w, h, x, y, fps, seconds))
    result = subprocess.run(command)
    return result.returncode


def encode_png_sequence(ffmpeg, frames_dir, fps_for_playback, output, quality):
    command = [
        ffmpeg, '-y', '-loglevel', 'error',
        '-framerate', '%.4f' % fps_for_playback,
        '-i', os.path.join(frames_dir, 'f%05d.png'),
        '-c:v', 'libx264',
        '-preset', 'veryfast',
        '-pix_fmt', 'yuv420p',   # what browsers and phones actually play
        '-crf', str(quality),
        '-movflags', '+faststart',
        output,
    ]
    return subprocess.run(command).returncode


def main():
    parser = argparse.ArgumentParser(description='Record an Unreal game window to mp4.')
    parser.add_argument('--title', default='PursuitAI',
                        help='substring of the window title (default: PursuitAI)')
    parser.add_argument('--seconds', type=float, default=15.0)
    parser.add_argument('--fps', type=int, default=30)
    parser.add_argument('--output', default='logs/demo.mp4')
    parser.add_argument('--engine', choices=['auto', 'gdigrab', 'png'], default='auto',
                        help='auto (default) prefers gdigrab: real-time, no frame files')
    parser.add_argument('--crf', type=int, default=20, help='x264 quality; lower is better')
    parser.add_argument('--still', default=None,
                        help='save one PNG here and exit, instead of recording a clip')
    parser.add_argument('--settle', type=float, default=1.0,
                        help='seconds to wait after raising the window before grabbing')
    parser.add_argument('--no-focus', action='store_true',
                        help='do not raise the window (if it is already in front)')
    parser.add_argument('--frames-dir', default='logs/frames',
                        help='PNG sequence lives here; safe to delete afterwards')
    parser.add_argument('--keep-frames', action='store_true')
    parser.add_argument('--ffmpeg', default=None,
                        help='path to ffmpeg if it is not on PATH')
    args = parser.parse_args()

    windows = find_window(args.title)
    if not windows:
        print('ERROR: no visible window whose title contains %r.' % args.title)
        print('       Start the game first - e.g. tools\\play.bat on the Cartoon City map,')
        print('       or tools\\record_city.py which launches it for you - and keep it')
        print('       uncovered: this records the screen, not the window off-screen.')
        return 1

    hwnd = windows[0][0]
    rect = client_rect(windows[0])
    print('window rect %s -> capturing client area %d x %d at (%d, %d)'
          % (windows[0][1:], rect[2], rect[3], rect[0], rect[1]))

    if not args.no_focus:
        raise_window(hwnd)
        # The compositor needs a frame or two after the raise, and the scene itself may be
        # mid-reset, so this is not just politeness - grabbing immediately catches either
        # the old front window or an empty scene.
        time.sleep(max(args.settle, 0.0))

        # Minimise/restore can move the window, and the rect above was measured before it.
        again = find_window(args.title)
        if again:
            hwnd = again[0][0]
            rect = client_rect(again[0])
            print('after raising, capturing client area %d x %d at (%d, %d)'
                  % (rect[2], rect[3], rect[0], rect[1]))

        # Say so rather than silently producing a picture of the wrong window. This is what
        # happens when something else grabs the foreground back between the raise and the grab.
        if USER32.GetForegroundWindow() != hwnd:
            print('WARNING: the window is not in the foreground; the grab may capture whatever')
            print('         is in front of it instead. Move anything covering it out of the way.')

    if args.still:
        os.makedirs(os.path.dirname(os.path.abspath(args.still)), exist_ok=True)
        x, y, w, h = rect
        image = ImageGrab.grab(bbox=(x, y, x + w, y + h))
        image.save(args.still)
        print('wrote %s (%s)' % (args.still, image.size))
        return 0

    ffmpeg = args.ffmpeg or shutil.which('ffmpeg')
    if not ffmpeg:
        print('ERROR: ffmpeg not found. Pass --ffmpeg <path>, or install it.')
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

    engine = args.engine
    if engine == 'auto':
        engine = 'gdigrab'

    if engine == 'gdigrab':
        code = capture_gdigrab(ffmpeg, rect, args.seconds, args.fps, args.output, args.crf)
        if code != 0:
            print('ERROR: gdigrab failed with exit code %d. Retry with --engine png.' % code)
            return code
        total = int(round(args.seconds * args.fps))
    else:
        total, effective = grab_frames(rect, args.seconds, args.fps, args.frames_dir)
        # Encode at what actually happened, not at what was asked for. This is the whole
        # difference between a real-time clip and one that plays at 2x.
        code = encode_png_sequence(ffmpeg, args.frames_dir, effective, args.output, args.crf)
        if code != 0:
            print('ERROR: ffmpeg failed with exit code %d' % code)
            return code
        if not args.keep_frames:
            for name in os.listdir(args.frames_dir):
                if name.startswith('f') and name.endswith('.png'):
                    os.remove(os.path.join(args.frames_dir, name))

    size = os.path.getsize(args.output)
    print('wrote %s (%.1f MB, ~%d frames at %d fps, %s engine)'
          % (args.output, size / 1048576.0, total, args.fps, engine))
    return 0


if __name__ == '__main__':
    sys.exit(main())
