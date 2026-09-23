# Record a before/after comparison as ONE clip, by filming two game windows side by side.
#
#   ./venv/Scripts/python.exe tools/portfolio_pair.py --seconds 30
#
# Why one capture of two windows instead of two captures joined afterwards
# -----------------------------------------------------------------------
# The obvious way to make a side-by-side is to record each pane separately and run
# `ffmpeg hstack` over the pair. It does not work here, and the reason is not cosmetic:
# the two panes have to be *the same experiment* - same seed, same spawn, same step rate,
# same step budget - and two independent recordings have two independent frame clocks.
# Nothing then says that frame 300 of the left file and frame 300 of the right file are
# the same moment, and "the trained one got there first" is exactly the claim the clip is
# making. Recording the union of both windows in a single grab settles it: one frame of
# the file is one instant on one clock, and there is nothing left to align.
#
# The second thing the pair needs is a start gate, for the same reason. Two processes
# launched a few seconds apart would begin their episodes a few seconds apart even with
# identical settings, so both are given the same `-PursuitStartAt=<unix epoch>` and sit
# frozen until it arrives. The clip therefore opens with both panes motionless and in the
# same pose, which is free evidence that they started together.
#
# What it does, in order
# ----------------------
#   1. Picks a spot on screen for the pair that no topmost overlay covers. Both capture
#      engines read the SCREEN, so an always-on-top window inside the frame is in the
#      video by definition. This host has one (a desktop lyric widget) sitting exactly
#      where the right pane would go.
#
#   2. Launches the left pane, waits for its window, then for its level - by polling that
#      run's own -abslog for the environment's BeginPlay line. "The window exists" is not
#      "there is a picture": UnrealEditor.exe -game builds its window first and streams
#      the map behind it, and a clip started on the window alone came back 60 KB of black.
#
#   3. Launches the right pane the same way. Both windows carry the same title, so the
#      right one is identified as "the window that is not the left one" rather than by
#      name - that is the reason the two are started one at a time.
#
#   4. Places both, raises them, and asks the pixels whether there is actually a picture
#      in each pane. No log signal and no fixed sleep can answer that; see record_city.
#
#   5. Sleeps until a couple of seconds before the shared start epoch, then grabs the
#      union rectangle in one gdigrab pass.
#
#   6. Closes both by title. Killing by image name would take an open editor down with
#      them, because -game forks a second UnrealEditor.exe.

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

UE_ROOT = os.environ.get('PURSUIT_UE_ROOT', r'E:\Project\UE5\UE_5.7')
EDITOR = os.path.join(UE_ROOT, 'Engine', 'Binaries', 'Win64', 'UnrealEditor.exe')
UPROJECT = os.path.join(PROJECT, 'PursuitAI.uproject')

WINDOW_TITLE = 'PursuitAI'
WINDOW_EXCLUDE = 'Unreal Editor'

USER32 = ctypes.windll.user32
GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x0008
SWP_NOZORDER = 0x0004

sys.path.insert(0, HERE)
import record_window as rw       # noqa: E402  (owns the capture code)
import record_city as rc         # noqa: E402  (owns window enumeration and the level wait)

# The Demonstration city - deliberately the same stage as record_city.py. The comparison
# was first proved on the training rig because a bare arena loads in seconds while the
# city's 5.7 MB of streaming data does not, but the stage is not a pipeline detail: the
# A/B clip is portfolio footage, and portfolio footage is shot in the city or it is not
# portfolio footage. -PursuitEnvBox hosts the env on this map (the city's own game mode
# would otherwise run the playable chase over it), -PursuitStage=city drops the env's
# built-in floor and lights and points its camera straight down, and -PursuitArenaAt
# parks the slab in the street measured clear by tools/inspect_city_stage.py (centre
# (-250, 0), ground z=10, slab bottom just above the road with a 60 cm half-height).
# --map still wins if a run ever needs the rig back.
DEFAULT_MAP = '/Game/Cartoon_City_Free/Maps/Demonstration'
STAGE_FLAGS = ['-PursuitEnvBox',
               '-PursuitStage=city',
               '-PursuitArenaAt=-250,0,70',
               '-PursuitArenaHeight=60']

# The v2 character environment. Different level, different ready line, different shared
# switches - but the same dual-window mechanics, so --env=char reroutes those three
# things and keeps everything else (placement, gate, single-grab capture) unchanged.
CHAR_MAP = '/Game/Maps/L_PursuitCharTrain'
CHAR_STAGE_FLAGS = []   # the C++ arena rig (floor + ring) is built by the env itself

# The environment's own BeginPlay line, added for exactly this: a marker that means "the
# actor is live and stepping", as opposed to "a window exists".
READY_MARKER = b'PursuitAIEnv: drive='
READY_MARKER_CHAR = b'PursuitCharEnv: arena rig built'


def screen_size():
    return USER32.GetSystemMetrics(0), USER32.GetSystemMetrics(1)


def topmost_boxes():
    """[(l,t,r,b)] of every always-on-top window that actually paints and is not full screen.

    Full-screen topmost windows (the NVIDIA overlay, the shell's Program Manager) are
    skipped on purpose: they cover everything, so treating them as obstacles would leave
    nowhere to put the panes, and they draw nothing into a grab.
    """
    screen_w, screen_h = screen_size()
    boxes = []
    for _hwnd, title, box, top, full in rc.visible_windows():
        if top and not full and box[2] > box[0] and box[3] > box[1] and title.strip():
            boxes.append(box)
    return boxes


# -resx/-resy size the CLIENT area, but SetWindowPos and -WinX/-WinY place the OUTER
# rectangle. The frame is 8 px a side plus a 31 px title bar, so two "960 wide" panes
# actually need 976 px each and 1920 px of screen is 32 px short of holding both. Miss
# this and the right pane overlaps the left by the width of one border, which looks like
# a rendering glitch and is really arithmetic.
BORDER_PX = 8
TITLEBAR_PX = 31


def pick_anchor(pane_w, pane_h, blockers):
    """(x, y) for the top-left of the pair, or None if every candidate is covered."""
    screen_w, screen_h = screen_size()
    pair_w = (pane_w + 2 * BORDER_PX) * 2
    pane_h = pane_h + 2 * BORDER_PX + TITLEBAR_PX
    if pair_w > screen_w or pane_h > screen_h:
        return None

    candidates = [
        (0, 0),
        (0, screen_h - pane_h),
        (screen_w - pair_w, 0),
        (screen_w - pair_w, screen_h - pane_h),
        (0, (screen_h - pane_h) // 2),
    ]
    for x, y in candidates:
        x = max(0, min(x, screen_w - pair_w))
        y = max(0, min(y, screen_h - pane_h))
        box = (x, y, x + pair_w, y + pane_h)
        if not any(rc.rects_overlap(box, b) for b in blockers):
            return (x, y)
    return None


def wait_new_window(known, timeout=180.0):
    """Wait for a game window that is not one of `known`. Returns its (hwnd, l, t, r, b)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for window in rw.find_window(WINDOW_TITLE, WINDOW_EXCLUDE):
            if window[0] not in known:
                return window
        time.sleep(1.0)
    return None


def wait_ready(log_path, marker=READY_MARKER, timeout=180.0):
    start = time.time()
    deadline = start + timeout
    while time.time() < deadline:
        try:
            with open(log_path, 'rb') as handle:
                blob = handle.read()
        except OSError:
            blob = b''
        if marker in blob:
            return True, time.time() - start
        for marker in rc.LEVEL_FAILED_MARKERS:
            if marker in blob:
                return False, time.time() - start
        time.sleep(1.0)
    return False, time.time() - start


def place(hwnd, x, y):
    USER32.SetWindowPos(hwnd, 0, x, y, 0, 0, 0x0001 | SWP_NOZORDER)  # SWP_NOSIZE


def pane_command(slot, args, anchor, start_at, log_path, pane_x):
    """The command line for one pane. Everything the two share is set from shared values."""
    command = [EDITOR, UPROJECT, args.map,
               '-game', '-windowed',
               '-resx=%d' % args.resx, '-resy=%d' % args.resy,
               '-WinX=%d' % pane_x, '-WinY=%d' % anchor[1],
               '-abslog=%s' % log_path]

    if args.env == 'char':
        # v2 shared switches. Real-time CharacterMovement (no fixed step rate), so both
        # panes get the same fps cap or their dt-driven clocks drift apart; the quit
        # timer is each pane's own self-exit safety net beside the title kill.
        shared = ['-PursuitCharSeed=%d' % args.seed,
                  '-PursuitStartAt=%d' % start_at,
                  '-PursuitCharMaxFPS=60',
                  '-PursuitCharQuitAfter=%d' % int(args.seconds + args.preroll + 30)]
        stage = CHAR_STAGE_FLAGS
    else:
        # The controlled part. Both panes get the same four numbers, so the only
        # thing that can differ between them is the policy named by --left/--right.
        shared = ['-PursuitStatic',
                  '-PursuitSeed=%d' % args.seed,
                  '-PursuitRate=%s' % args.rate,
                  '-PursuitMaxSteps=%d' % args.max_steps,
                  '-PursuitStartAt=%d' % start_at]
        stage = STAGE_FLAGS

    command += shared + stage + [
        # Underscores, not spaces: FParse::Value stops at whitespace whatever its
        # separator setting is, and the environment turns them back into spaces.
        '-PursuitPaneLabel=%s' % slot['label'].replace(' ', '_')]
    command.extend(slot['flags'])
    return command


def main():
    parser = argparse.ArgumentParser(
        description='Record a two-pane before/after comparison in one clip.')
    parser.add_argument('--seconds', type=float, default=30.0)
    parser.add_argument('--fps', type=int, default=30)
    # 944 and not 960: -resx sizes the client area, the window frame costs another 16 px,
    # and two panes have to fit side by side on a 1920 px screen. See BORDER_PX.
    parser.add_argument('--resx', type=int, default=944)
    parser.add_argument('--resy', type=int, default=720)
    parser.add_argument('--map', default=None,
                        help='defaults to the city (v1) or the char rig (char)')
    parser.add_argument('--seed', type=int, default=20260921)
    parser.add_argument('--rate', default='6')
    parser.add_argument('--max-steps', type=int, default=300)
    parser.add_argument('--lead', type=float, default=120.0,
                        help='seconds between launching the panes and the shared start epoch')
    parser.add_argument('--preroll', type=float, default=2.0,
                        help='seconds of the frozen pose to film before the epoch arrives')
    parser.add_argument('--crf', type=int, default=20)
    parser.add_argument('--output', default=None)
    parser.add_argument('--brightness', type=float, default=rc.BLANK_FRAME_MEAN)
    parser.add_argument('--allow-black', action='store_true',
                        help='film even if a pane never shows a picture')
    parser.add_argument('--keep-open', action='store_true')
    # v1 = the ball env on the city stage; char = the v2 character env on its C++ rig.
    parser.add_argument('--env', choices=('v1', 'char'), default='v1',
                        help='which environment the two panes run')
    # The two panes. Left is the baseline and right is what it is being compared against;
    # both are lists of raw switches so the script never has to know what a "policy" is.
    parser.add_argument('--left', default='-PursuitRandom',
                        help='switches for the left pane (comma-separated)')
    parser.add_argument('--right', default='-PursuitDemo',
                        help='switches for the right pane (comma-separated)')
    parser.add_argument('--left-label', default='RANDOM BASELINE')
    parser.add_argument('--right-label', default='SCRIPTED GREEDY')
    args = parser.parse_args()

    if args.map is None:
        args.map = CHAR_MAP if args.env == 'char' else DEFAULT_MAP
    ready_marker = READY_MARKER_CHAR if args.env == 'char' else READY_MARKER

    output = args.output or os.path.join(
        PROJECT, 'logs', 'portfolio', 'pair_%ds.mp4' % int(round(args.seconds)))

    if not os.path.exists(EDITOR):
        print('ERROR: engine not found: %s' % EDITOR)
        return 1
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        print('ERROR: ffmpeg not found on PATH.')
        return 1

    screen_w, screen_h = screen_size()
    print('screen %d x %d' % (screen_w, screen_h))

    blockers = topmost_boxes()
    anchor = pick_anchor(args.resx, args.resy, blockers)
    if anchor is None:
        print('ERROR: no position fits two %dx%d panes that is clear of these overlays:'
              % (args.resx, args.resy))
        for box in blockers:
            print('       %s' % (box,))
        if not blockers:
            print('       (the panes do not fit on this screen at all - lower --resx/--resy)')
        return 1
    for box in blockers:
        print('  topmost overlay at %s' % (box,))
    print('  panes go at %s and %s, clear of all of them'
          % (anchor, (anchor[0] + args.resx, anchor[1])))

    stamp = time.strftime('%Y%m%d_%H%M%S')
    logs = [os.path.join(PROJECT, 'logs', 'pair_%s_%s.log' % (side, stamp))
            for side in ('left', 'right')]
    os.makedirs(os.path.dirname(logs[0]), exist_ok=True)

    slots = [
        {'name': 'LEFT', 'flags': args.left.split(','), 'label': args.left_label,
         'x': anchor[0], 'log': logs[0]},
        {'name': 'RIGHT', 'flags': args.right.split(','), 'label': args.right_label,
         'x': anchor[0] + args.resx + 2 * BORDER_PX, 'log': logs[1]},
    ]

    # The shared start epoch. Computed before anything is launched because it has to be
    # passed to both processes on their command lines - there is no channel to tell a
    # running process when to begin, so both are told at birth.
    start_at = int(time.time()) + int(round(args.lead))
    print('shared start epoch %d (%.0f s from now)' % (start_at, args.lead))

    known = set()
    handles = []
    for slot in slots:
        command = pane_command(slot, args, anchor, start_at, slot['log'], slot['x'])
        print('launching %s: %s' % (slot['name'], ' '.join(command[3:])))
        process = subprocess.Popen(command, cwd=PROJECT)

        print('  waiting for its window...')
        window = wait_new_window(known)
        if window is None:
            print('ERROR: the %s window never appeared. Log: %s' % (slot['name'], slot['log']))
            process.terminate()
            rc.kill_by_title(WINDOW_TITLE)
            return 1
        known.add(window[0])
        handles.append((slot, process, window))
        print('  window is up (hwnd %d)' % window[0])

        print('  waiting for the level...')
        ready, waited = wait_ready(slot['log'], ready_marker)
        if not ready:
            print('ERROR: the %s pane did not come up. Last lines of %s:'
                  % (slot['name'], slot['log']))
            for line in rc.log_tail(slot['log']).splitlines():
                print('       | %s' % line)
            rc.kill_by_title(WINDOW_TITLE)
            return 1
        print('  level is up after %.1f s' % waited)

    # Placed after both are up, because a window that is still being created ignores
    # SetWindowPos, and because neither may cover the other while it loads.
    rects = []
    for slot, _process, window in handles:
        hwnd = window[0]
        place(hwnd, slot['x'], anchor[1])
        rw.raise_window(hwnd)
        time.sleep(0.8)
        found = [w for w in rw.find_window(WINDOW_TITLE, WINDOW_EXCLUDE) if w[0] == hwnd]
        current = found[0] if found else window
        if current[1] != slot['x']:
            place(hwnd, slot['x'], anchor[1])
            time.sleep(0.5)
            found = [w for w in rw.find_window(WINDOW_TITLE, WINDOW_EXCLUDE) if w[0] == hwnd]
            current = found[0] if found else current
        rects.append(rw.client_rect(current))
        print('  %s pane client area %d x %d at (%d, %d)'
              % (slot['name'], rects[-1][2], rects[-1][3], rects[-1][0], rects[-1][1]))

    # Both panes have to have a picture before any of this is worth filming, and the check
    # is on the pixels with a raise-and-retry loop rather than a single measurement.
    #
    # The loop is not superstition. A blank grab on this host is intermittent - it happened
    # on one run in three with no difference in the log, the load time or the rectangle,
    # and the first version of this script failed with a pane that measured 0.0 for 47
    # straight seconds while the log showed a fully built scene. What differs between the
    # runs is invisible, and the recovery is mechanical, so the recovery is what runs.
    for index, ((slot, _process, window), rect) in enumerate(zip(handles, rects)):
        if args.allow_black:
            print('  %s pane: --allow-black, not checking the pixels' % slot['name'])
            continue
        ok, mean, rect, hwnd, attempts = rc.ensure_visible_frame(
            window[0], window, args.brightness)
        if not ok:
            print('ERROR: the %s pane stayed blank after %d raises (mean %.1f). Log: %s'
                  % (slot['name'], attempts, mean, slot['log']))
            rc.kill_by_title(WINDOW_TITLE)
            return 1
        rects[index] = rect
        handles[index] = (slot, _process, (hwnd,) + tuple(window[1:]))
        print('  %s pane has a picture (mean %.1f, %d raise attempt(s))'
              % (slot['name'], mean, attempts))

    def measure_union():
        x0 = min(r[0] for r in rects)
        y0 = min(r[1] for r in rects)
        x1 = max(r[0] + r[2] for r in rects)
        y1 = max(r[1] + r[3] for r in rects)
        return (x0, y0, (x1 - x0) - ((x1 - x0) % 2), (y1 - y0) - ((y1 - y0) % 2))

    remaining = start_at - time.time() - args.preroll
    if remaining > 0:
        print('both panes are frozen on WAITING FOR GO; filming in %.0f s' % remaining)
        time.sleep(remaining)
    else:
        print('WARNING: the start epoch has already passed (%.0f s ago) - the panes will'
              % -remaining)
        print('         not be in phase. Raise --lead.')

    # Re-raised after the wait and measured again: minimise/restore can move a window, and
    # a union rectangle computed before the wait is a rectangle that may no longer be over
    # the panes by the time the grab starts.
    for index, (slot, _process, window) in enumerate(handles):
        rw.raise_window(window[0])
        time.sleep(0.5)
        found = [w for w in rw.find_window(WINDOW_TITLE, WINDOW_EXCLUDE) if w[0] == window[0]]
        if found:
            rects[index] = rw.client_rect(found[0])
    union = measure_union()
    print('capturing the union %d x %d at (%d, %d)' % (union[2], union[3], union[0], union[1]))

    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    code = rw.capture_gdigrab(ffmpeg, union, args.seconds, args.fps, output, args.crf)
    if code != 0:
        print('ERROR: gdigrab failed with exit code %d' % code)
        rc.kill_by_title(WINDOW_TITLE)
        return code
    print('wrote %s (%.1f MB)' % (output, os.path.getsize(output) / 1048576.0))

    if args.keep_open:
        print('leaving both panes running (--keep-open).')
        return 0

    print('closing both panes')
    rc.kill_by_title(WINDOW_TITLE)
    return 0


if __name__ == '__main__':
    sys.exit(main())
