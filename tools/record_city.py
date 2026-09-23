# Record the Cartoon City chase - the project's one and only recording scene.
#
#   ./venv/Scripts/python.exe tools/record_city.py --seconds 30
#
# The map is Cartoon_City_Free/Maps/Demonstration and it is not a parameter, deliberately.
# Everything the project shows - the chase, the obstacles, the jump towers, the animation
# states - lives on that map, and L_PursuitAITrain is a training rig that builds its own
# bare floor at the world origin. A clip recorded there is a clip of a grey box, which is
# what the first attempt at this produced and why this script names the map itself instead
# of taking it from a switch that could be got wrong again.
#
# What it does, in order, and why each step is here:
#
#   1. Launches the game on the Demonstration map with -PursuitAutoPlay, so the chase drives
#      itself and nobody has to hold WASD. -PursuitRunSeconds is set past the end of the
#      recording so the process quits on its own afterwards; an UnrealEditor.exe left in
#      memory makes the next UBT build fail in three seconds with "Unable to build while
#      Live Coding is active", which reads as a compile error and is nothing of the sort.
#
#   2. Waits for the game window, and then for the LEVEL. The window is up long before the
#      scene is: `UnrealEditor.exe -game` creates its window first and loads the map behind
#      it, so "the window exists" is not "there is anything to film". A 30 s clip recorded on
#      that signal came back 60 KB of pure black - the loading screen, which compresses to
#      nothing. Cartoon City is a 5.7 MB BuiltData level with its own streaming, so the gap
#      is long. The signal that actually matters is the game mode's own BeginPlay line in
#      the log, which is why this launch passes -abslog=<per-run path> and polls that.
#
#   3. Moves the window out from under any topmost overlay. This is the step that was
#      missing when a recorded clip came back with a desktop lyric widget burned into its
#      top-right corner: BOTH capture engines grab the screen, not the window, so a
#      WS_EX_TOPMOST window is in the video by definition and no amount of raising the game
#      helps. Full-screen overlays (the NVIDIA overlay, the shell's Program Manager) are
#      ignored - they cover everything, so treating them as obstacles would leave nowhere
#      to put the window and they do not paint anything anyway.
#
#   4. Records with tools/record_window.py's gdigrab path: ffmpeg grabs and encodes in one
#      pass at real wall-clock rate.
#
#   5. Closes the window, by title rather than by image name, because `UnrealEditor.exe
#      -game` forks a second UnrealEditor.exe and killing by image name would take an open
#      editor down with it.
#
# Pass --keep-open to leave the game running afterwards (to watch it yourself), and
# --extra to forward switches to the game (e.g. --extra=-PursuitNoHud).

import argparse
import ctypes
import ctypes.wintypes as win
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.abspath(os.path.join(HERE, '..'))

# Tools for the project's own paths, not the caller's. The one absolute constant is the
# engine, because there is nothing in this project that knows where a given UE install
# lives and the system PATH has no Epic entry at all on this machine.
UE_ROOT = os.environ.get('PURSUIT_UE_ROOT', r'E:\Project\UE5\UE_5.7')
EDITOR = os.path.join(UE_ROOT, 'Engine', 'Binaries', 'Win64', 'UnrealEditor.exe')

UPROJECT = os.path.join(PROJECT, 'PursuitAI.uproject')
MAP = '/Game/Cartoon_City_Free/Maps/Demonstration'

WINDOW_TITLE = 'PursuitAI'
WINDOW_EXCLUDE = 'Unreal Editor'

USER32 = ctypes.windll.user32
KERNEL32 = ctypes.windll.kernel32
PROCESS_TERMINATE = 0x0001

GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x0008

SWP_NOSIZE = 0x0001
SWP_NOZORDER = 0x0004
SWP_NOMOVE = 0x0002

sys.path.insert(0, HERE)
import record_window as rw  # noqa: E402  (same folder, and it owns the capture code)


def visible_windows():
    """[(hwnd, title, (l, t, r, b), is_topmost, is_fullscreen)] for every visible titled window."""
    screen_w = USER32.GetSystemMetrics(0)
    screen_h = USER32.GetSystemMetrics(1)
    out = []

    def callback(hwnd, _lparam):
        if not USER32.IsWindowVisible(hwnd):
            return True
        length = USER32.GetWindowTextLengthW(hwnd)
        if length == 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        USER32.GetWindowTextW(hwnd, buf, length + 1)
        rect = win.RECT()
        USER32.GetWindowRect(hwnd, ctypes.byref(rect))
        box = (rect.left, rect.top, rect.right, rect.bottom)
        ex = USER32.GetWindowLongW(hwnd, GWL_EXSTYLE)
        full = (box[0] <= 0 and box[1] <= 0 and box[2] >= screen_w and box[3] >= screen_h)
        out.append((hwnd, buf.value, box, bool(ex & WS_EX_TOPMOST), full))
        return True

    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, win.HWND, win.LPARAM)
    USER32.EnumWindows(callback_type(callback), 0)
    return out


def wait_for_window(timeout=180.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = rw.find_window(WINDOW_TITLE, WINDOW_EXCLUDE)
        if found:
            return found[0]
        time.sleep(2.0)
    return None


# The game mode's BeginPlay line. Waiting on it rather than on a timer is what separates
# "the window is open" from "the scene is on screen": the window appears while the level is
# still loading behind a black viewport, and Cartoon City takes long enough that a clip
# started on the window alone is entirely black.
LEVEL_READY_MARKER = b'PursuitPlay:'
# A level that failed to load says so and then sits there. Bailing early on these turns a
# minute of waiting into a message that names the file to read.
LEVEL_FAILED_MARKERS = (b'Fatal error', b'LogExit: Executing StaticShutdownAfterError',
                        b'Error: Failed to load')


def log_tail(path, lines=12):
    try:
        with open(path, 'rb') as handle:
            text = handle.read().decode('utf-8', errors='replace')
        return '\n'.join(text.splitlines()[-lines:])
    except OSError as exc:
        return '<could not read %s: %s>' % (path, exc)


def wait_for_level(log_path, timeout=180.0):
    """Block until the game mode has started in this run's log, by polling its own log file.

    Polling rather than sleeping a fixed amount: the load time is not constant, and a clip
    that starts too early is black while one that starts too late misses the scene. Returns
    (ok, elapsed_seconds).
    """
    start = time.time()
    deadline = start + timeout
    while time.time() < deadline:
        try:
            with open(log_path, 'rb') as handle:
                blob = handle.read()
        except OSError:
            blob = b''

        if LEVEL_READY_MARKER in blob:
            return True, time.time() - start
        for marker in LEVEL_FAILED_MARKERS:
            if marker in blob:
                return False, time.time() - start
        time.sleep(1.0)
    return False, time.time() - start


# A frame of this scene averages about 120-170 per channel. A blank capture measures 0.0,
# with a handful of bright pixels where the HUD text is - which is what makes this check
# worth having: a black frame is not silence, it is a clip that looks like a rendering bug.
BLANK_FRAME_MEAN = 6.0


def frame_mean(rect):
    """Average channel value of the pixels the recorder would capture, right now."""
    from PIL import ImageGrab, ImageStat
    x, y, w, h = rect
    image = ImageGrab.grab(bbox=(x, y, x + w, y + h))
    return sum(ImageStat.Stat(image).mean) / 3.0


def wait_for_visible_frame(rect, threshold=BLANK_FRAME_MEAN, timeout=60.0):
    """Block until the capture region has actual picture in it. Returns (ok, mean, waited).

    The reason this exists rather than a longer sleep: a clip was once recorded 30 seconds
    of pure black *while the game was demonstrably running* - the log for that run shows the
    game mode's BeginPlay and a camera position every second throughout - so no log signal
    and no fixed delay would have caught it. Asking the pixels is the only check that cannot
    be fooled by the scene being up, the window being foreground and the rect being right.
    """
    start = time.time()
    last = 0.0
    while time.time() - start < timeout:
        last = frame_mean(rect)
        if last > threshold:
            return True, last, time.time() - start
        time.sleep(2.0)
    return False, last, time.time() - start


def window_title(hwnd):
    length = USER32.GetWindowTextLengthW(hwnd)
    if length == 0:
        return '<untitled>'
    buf = ctypes.create_unicode_buffer(length + 1)
    USER32.GetWindowTextW(hwnd, buf, length + 1)
    return buf.value


def describe_window(hwnd):
    """One line about what the window looks like right now, for a failure that has no log trace."""
    rect = win.RECT()
    USER32.GetWindowRect(hwnd, ctypes.byref(rect))
    foreground = USER32.GetForegroundWindow()
    return ('iconic=%d visible=%d rect=(%d, %d, %d, %d) foreground=%r'
            % (1 if USER32.IsIconic(hwnd) else 0,
               1 if USER32.IsWindowVisible(hwnd) else 0,
               rect.left, rect.top, rect.right, rect.bottom,
               window_title(foreground)[:40]))


def ensure_visible_frame(hwnd, fallback_window, threshold=BLANK_FRAME_MEAN,
                         rounds=4, per_round=8.0):
    """Raise the window, look at the pixels, and if they are blank raise it again.

    A blank capture is intermittent - it happened on one run out of three with no difference
    in the log, the level-load time or the rect - and it is not worth guessing at the cause
    when the recovery is mechanical. What the loop buys is that the common case costs one
    raise and the pathological case ends with a diagnosis instead of a black clip.

    Returns (ok, mean, rect, hwnd, attempts).
    """
    last = 0.0
    rect = None
    for attempt in range(rounds):
        rw.raise_window(hwnd)
        # The compositor needs a frame or two, and minimise/restore can move the window, so
        # the rect is re-measured after every raise rather than reused.
        time.sleep(0.8)
        fresh = rw.find_window(WINDOW_TITLE, WINDOW_EXCLUDE)
        if fresh:
            hwnd = fresh[0][0]
        window = fresh[0] if fresh else fallback_window
        rect = rw.client_rect(window)

        ok, last, waited = wait_for_visible_frame(rect, threshold, timeout=per_round)
        if ok:
            return True, last, rect, hwnd, attempt + 1
        print('  raise %d: still blank after %.0f s (mean %.1f) - %s'
              % (attempt + 1, waited, last, describe_window(hwnd)))
    return False, last, rect, hwnd, rounds


def rects_overlap(a, b):
    return not (a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])


def dodge_topmost_overlays(hwnd):
    """Move the game window somewhere no topmost overlay covers it.

    Returns (moved_to_or_None, blockers). Both capture engines read the screen, so this is
    not cosmetic - a blocker that stays put is burned into every frame.
    """
    screen_w = USER32.GetSystemMetrics(0)
    screen_h = USER32.GetSystemMetrics(1)

    window_rect = win.RECT()
    USER32.GetWindowRect(hwnd, ctypes.byref(window_rect))
    w = window_rect.right - window_rect.left
    h = window_rect.bottom - window_rect.top

    blockers = [box for _h, title, box, top, full in visible_windows()
                if top and not full and box[2] > box[0] and box[3] > box[1]
                and title.strip() != '']

    if not blockers:
        return None, []

    # Candidate spots, in order of preference. (0, 140) first: it keeps the window fully on
    # screen with room for the overlay area that usually sits along the top edge, and it is
    # the position the hand-made fix used, so a known-good answer is tried first.
    candidates = [(0, 140), (0, 0), (screen_w - w, 0), (0, screen_h - h),
                  (screen_w - w, screen_h - h)]
    for x, y in candidates:
        x = max(0, min(x, max(screen_w - w, 0)))
        y = max(0, min(y, max(screen_h - h, 0)))
        box = (x, y, x + w, y + h)
        if not any(rects_overlap(box, b) for b in blockers):
            USER32.SetWindowPos(hwnd, 0, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER)
            return (x, y), blockers

    return None, blockers


def kill_by_title(substring):
    """Close the game window's process, matching on title so an open editor is spared."""
    matches = []

    def callback(hwnd, _lparam):
        if not USER32.IsWindowVisible(hwnd):
            return True
        length = USER32.GetWindowTextLengthW(hwnd)
        if length == 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        USER32.GetWindowTextW(hwnd, buf, length + 1)
        title = buf.value
        if substring in title and WINDOW_EXCLUDE not in title:
            pid = win.DWORD()
            USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            matches.append((title, pid.value))
        return True

    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, win.HWND, win.LPARAM)
    USER32.EnumWindows(callback_type(callback), 0)

    for title, pid in matches:
        handle = KERNEL32.OpenProcess(PROCESS_TERMINATE, False, pid)
        if not handle:
            print('  could not open pid %d (%r) - already gone?' % (pid, title))
            continue
        KERNEL32.TerminateProcess(handle, 0)
        KERNEL32.CloseHandle(handle)
        print('  closed pid %d (%r)' % (pid, title))
    return len(matches)


def main():
    parser = argparse.ArgumentParser(description='Record the Cartoon City chase.')
    parser.add_argument('--seconds', type=float, default=30.0)
    parser.add_argument('--fps', type=int, default=30)
    parser.add_argument('--resx', type=int, default=1280)
    parser.add_argument('--resy', type=int, default=720)
    parser.add_argument('--output', default=None,
                        help='default logs/portfolio/city_<seconds>s.mp4')
    parser.add_argument('--crf', type=int, default=20)
    parser.add_argument('--still', default=None,
                        help='grab one frame instead of a clip (same launch, same window)')
    parser.add_argument('--settle', type=float, default=3.0,
                        help='seconds between the window appearing and the first frame')
    parser.add_argument('--keep-open', action='store_true',
                        help='leave the game running when the recording is done')
    parser.add_argument('--no-hud', action='store_true')
    parser.add_argument('--brightness', type=float, default=BLANK_FRAME_MEAN,
                        help='a first frame darker than this is treated as blank (default %.0f)'
                             % BLANK_FRAME_MEAN)
    parser.add_argument('--allow-black', action='store_true',
                        help='record even if the first frame is blank')
    parser.add_argument('--extra', action='append', default=[],
                        help='extra switch for the game; repeatable')
    # god (the default) is the detached camera high above the chase; follow and first are the
    # pawn's own cameras. It is a parameter rather than a constant because "show the chase from
    # outside" and "show what the player saw" are both legitimate clips, but the default has to
    # be god: the pawn's own camera is what put the back of a car in front of the chase in the
    # first recording of this scene, and an occlusion that hides the movement defeats the point
    # of filming it.
    parser.add_argument('--camera', default='god', choices=('god', 'follow', 'first'),
                        help='camera for the capture (default god)')
    args = parser.parse_args()

    output = args.output or os.path.join(
        PROJECT, 'logs', 'portfolio',
        'city_%ds.mp4' % int(round(args.seconds)))

    if not os.path.exists(EDITOR):
        print('ERROR: engine not found: %s' % EDITOR)
        print('       Set PURSUIT_UE_ROOT if it lives somewhere else.')
        return 1
    if not os.path.exists(UPROJECT):
        print('ERROR: project not found: %s' % UPROJECT)
        return 1

    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg and not args.still:
        print('ERROR: ffmpeg not found on PATH.')
        return 1

    # Each run gets its own log. Two reasons, and the second is the load-bearing one:
    # a shared Saved/Logs/PursuitAI.log would already hold the previous run's BeginPlay line,
    # so "wait for the marker" would return instantly on stale text; and nothing has to be
    # deleted, which matters because this host refuses bulk deletes.
    log_path = os.path.join(PROJECT, 'logs', 'record_city_%s.log'
                            % time.strftime('%Y%m%d_%H%M%S'))
    os.makedirs(os.path.dirname(log_path), exist_ok=True)

    # The run clock outlives the recording so the process ends by itself rather than being
    # killed mid-write. It has to cover the settle, the blank-frame retries and the recording
    # itself, or a slow start eats the clip; with --keep-open it is left at 0, which the game
    # mode reads as "no self-quit at all".
    run_seconds = 0 if args.keep_open else int(round(args.seconds + args.settle + 60.0))

    command = [EDITOR, UPROJECT, MAP,
               '-game', '-windowed',
               '-resx=%d' % args.resx, '-resy=%d' % args.resy,
               '-abslog=%s' % log_path,
               '-PursuitAutoPlay',
               '-PursuitCamera=%s' % args.camera,
               '-PursuitRunSeconds=%d' % run_seconds]
    if args.no_hud:
        command.append('-PursuitNoHud')
    command.extend(args.extra)

    print('launching the Cartoon City chase on %s' % MAP)
    print('  %s' % ' '.join(command[1:]))
    print('  log: %s' % log_path)
    process = subprocess.Popen(command, cwd=PROJECT)

    print('waiting for the game window...')
    window = wait_for_window()
    if not window:
        print('ERROR: the window never appeared. Check %s' % log_path)
        if process.poll() is None:
            process.terminate()
        return 1

    hwnd = window[0]
    print('window is up')

    print('waiting for the level to load (the window is black until the game mode starts)...')
    ready, waited = wait_for_level(log_path)
    if not ready:
        print('ERROR: the level did not come up within the timeout.')
        print('       last lines of %s:' % log_path)
        for line in log_tail(log_path).splitlines():
            print('       | %s' % line)
        if process.poll() is None:
            process.terminate()
        kill_by_title(WINDOW_TITLE)
        return 1
    print('level is up after %.1f s' % waited)

    moved, blockers = dodge_topmost_overlays(hwnd)
    if blockers:
        for box in blockers:
            print('  topmost overlay covers %s' % (box,))
        if moved:
            print('  moved the game window to %s to stay clear of it' % (moved,))
        else:
            print('  WARNING: every candidate position is covered by a topmost overlay.')
            print('           The overlay will be in the video. Close it and re-run.')
    else:
        print('  no topmost overlay in the way')

    # A city run reloads a large level, and the first seconds are streaming, so the settle
    # window is longer than record_window's default.
    time.sleep(max(args.settle, 0.0))

    # Raise, look at the pixels, and keep raising until there is a picture. Every failure
    # this is here to catch - a stale rect after minimise/restore, another window holding the
    # foreground, a window dragged off screen - ends the same way on screen, so the check is
    # on the pixels and the recovery is just to try again.
    if args.allow_black:
        rw.raise_window(hwnd)
        time.sleep(0.8)
        fresh = rw.find_window(WINDOW_TITLE, WINDOW_EXCLUDE)
        hwnd = fresh[0][0] if fresh else hwnd
        rect = rw.client_rect(fresh[0] if fresh else window)
    else:
        visible, mean, rect, hwnd, attempts = ensure_visible_frame(
            hwnd, window, args.brightness)
        if visible:
            print('picture is up (mean %.1f, %d raise attempt(s))' % (mean, attempts))
        else:
            print('ERROR: the capture region stayed blank after %d raises (mean %.1f).' % (attempts, mean))
            print('       A blank region with the game running means it is not actually being')
            print('       displayed - check for a topmost overlay over the window, or a window')
            print('       dragged off screen. Log: %s' % log_path)
            print('       Pass --allow-black to record anyway.')
            if process.poll() is None and not args.keep_open:
                process.terminate()
            kill_by_title(WINDOW_TITLE)
            return 1

    if USER32.GetForegroundWindow() != hwnd:
        print('WARNING: the game is not in the foreground - the grab may capture')
        print('         whatever is in front of it.')
    print('capturing client area %d x %d at (%d, %d)' % (rect[2], rect[3], rect[0], rect[1]))

    if args.still:
        os.makedirs(os.path.dirname(os.path.abspath(args.still)), exist_ok=True)
        x, y, w, h = rect
        from PIL import ImageGrab
        image = ImageGrab.grab(bbox=(x, y, x + w, y + h))
        image.save(args.still)
        print('wrote %s (%s)' % (args.still, image.size))
    else:
        os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
        code = rw.capture_gdigrab(ffmpeg, rect, args.seconds, args.fps, output, args.crf)
        if code != 0:
            print('ERROR: gdigrab failed with exit code %d' % code)
            if process.poll() is None and not args.keep_open:
                process.terminate()
            return code
        print('wrote %s (%.1f MB, %d fps, %.1f s)'
              % (output, os.path.getsize(output) / 1048576.0, args.fps, args.seconds))

    if args.keep_open:
        print('leaving the game running (--keep-open). Close it with Alt+F4, or:')
        print('  python tools/kill_window.py %s' % WINDOW_TITLE)
        return 0

    print('closing the game window')
    kill_by_title(WINDOW_TITLE)
    return 0


if __name__ == '__main__':
    sys.exit(main())
