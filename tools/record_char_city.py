# Record a v2 char-env chase on the Cartoon City plaza, with a trained policy.
#
#   .venv/Scripts/python.exe tools/record_char_city.py --seconds 30 \
#       --model checkpoints/city_v2c/policy_0k.onnx --label UNTRAINED_0K
#
# This is record_city.py's machinery aimed at the v2 char env: same window find,
# same level-loaded poll, same topmost-overlay dodge, same gdigrab capture - but the
# launch is the -PursuitCharEnvBox host on the Demonstration map with the watch
# camera, the chaser driven by an exported ONNX policy instead of the v1 autoplay
# game mode. The RPGHero soldier joins as the second (greedy) chaser; either of them
# closing to catch radius ends the episode and the next one starts, so one clip shows
# several attempts.
#
# The "level is live" signal is the env's own 'episode 1 started' log line: the window
# and even the game mode come up long before the watch camera exists, and a clip
# started on the window alone filmed the loading screen (record_city.py history).

import argparse
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
MAP = '/Game/Cartoon_City_Free/Maps/Demonstration'

sys.path.insert(0, HERE)
import record_city as rc  # noqa: E402  (owns window find, overlay dodge, capture)
import record_window as rw  # noqa: E402

# The game window title ends in the RHI name (PCD3D_SM5). Killing by the project name
# alone also matches ANY window with PursuitAI in the title - it took out a Visual
# Studio window titled "PursuitAI - PursuitCharEnv.h - ..." (2026-09-21). Match the
# RHI fragment instead: it exists only on the launched game window.
GAME_TITLE_MARK = 'PCD3D'


def main():
    parser = argparse.ArgumentParser(description='Record the v2 char-env city chase with a policy.')
    parser.add_argument('--seconds', type=float, default=30.0)
    parser.add_argument('--fps', type=int, default=30)
    parser.add_argument('--resx', type=int, default=1280)
    parser.add_argument('--resy', type=int, default=720)
    parser.add_argument('--model', required=True,
                        help='ONNX path for -PursuitCharModel= (project-relative or absolute)')
    parser.add_argument('--label', default=None,
                        help='burned-in pane label; underscores become spaces')
    parser.add_argument('--map', default=MAP)
    parser.add_argument('--output', default=None)
    parser.add_argument('--crf', type=int, default=20)
    parser.add_argument('--settle', type=float, default=3.0)
    parser.add_argument('--keep-open', action='store_true')
    parser.add_argument('--allow-black', action='store_true')
    parser.add_argument('--extra', action='append', default=[],
                        help='extra switch for the game; repeatable')
    args = parser.parse_args()

    output = args.output or os.path.join(
        PROJECT, 'logs', 'portfolio',
        'char_city_%s_%ds.mp4' % (os.path.splitext(os.path.basename(args.model))[0],
                                  int(round(args.seconds))))

    if not os.path.exists(EDITOR):
        print('ERROR: engine not found: %s' % EDITOR)
        return 1
    model_path = args.model if os.path.isabs(args.model) else os.path.join(PROJECT, args.model)
    if not os.path.exists(model_path):
        print('ERROR: model not found: %s' % model_path)
        return 1

    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        print('ERROR: ffmpeg not found on PATH.')
        return 1

    log_path = os.path.join(PROJECT, 'logs', 'record_char_%s.log'
                            % time.strftime('%Y%m%d_%H%M%S'))
    os.makedirs(os.path.dirname(log_path), exist_ok=True)

    run_seconds = 0 if args.keep_open else int(round(args.seconds + args.settle + 90.0))

    command = [EDITOR, UPROJECT, args.map,
               '-game', '-windowed',
               '-resx=%d' % args.resx, '-resy=%d' % args.resy,
               '-abslog=%s' % log_path,
               '-PursuitCharEnvBox', '-PursuitStage=city',
               '-PursuitCharInference',
               '-PursuitCharModel=%s' % args.model,
               '-PursuitCharQuitAfter=%d' % run_seconds]
    if args.label:
        command.append('-PursuitPaneLabel=%s' % args.label.replace(' ', '_'))
    command.extend(args.extra)

    print('launching the v2 char chase on %s' % args.map)
    print('  model: %s' % args.model)
    print('  log: %s' % log_path)
    process = subprocess.Popen(command, cwd=PROJECT)

    print('waiting for the game window...')
    window = rc.wait_for_window()
    if not window:
        print('ERROR: the window never appeared. Check %s' % log_path)
        if process.poll() is None:
            process.terminate()
        return 1
    hwnd = window[0]
    print('window is up')

    # The watch camera exists only from the first episode on; before that the screen
    # shows the raw pawn view over a still-streaming city.
    print("waiting for 'episode 1 started' (the watch camera comes up with it)...")
    # The game mode's own marker (PursuitPlay:) fires at spawn time, before the env's
    # BeginPlay wires the watch camera - too early. The env's first-episode line is the
    # real "scene is live" signal, so point the shared poller at it.
    rc.LEVEL_READY_MARKER = b'PursuitCharEnv: episode 1 started'
    ok, waited = rc.wait_for_level(log_path, timeout=240.0)
    if not ok:
        print('ERROR: the first episode did not start within the timeout.')
        for line in rc.log_tail(log_path).splitlines():
            print('       | %s' % line)
        if process.poll() is None:
            process.terminate()
        rc.kill_by_title(GAME_TITLE_MARK)
        return 1
    print('first episode running after %.1f s' % waited)

    moved, blockers = rc.dodge_topmost_overlays(hwnd)

    # gdigrab fails to init outright on a negative offset ("Error opening input file
    # desktop." with the rect at x=-14), so pull the window fully on-screen and take
    # the rect from where it actually landed.
    rc.USER32.SetWindowPos(hwnd, 0, 10, 60, 0, 0, rc.SWP_NOSIZE | rc.SWP_NOZORDER)
    time.sleep(0.6)
    fresh = rw.find_window(rc.WINDOW_TITLE, rc.WINDOW_EXCLUDE)
    if fresh:
        hwnd = fresh[0][0]
        window = fresh[0]
    if blockers:
        for box in blockers:
            print('  topmost overlay covers %s' % (box,))
        if moved:
            print('  moved the game window to %s' % (moved,))
    else:
        print('  no topmost overlay in the way')

    time.sleep(max(args.settle, 0.0))

    visible, mean, rect, hwnd, attempts = rc.ensure_visible_frame(hwnd, window)
    if visible:
        print('picture is up (mean %.1f, %d raise attempt(s))' % (mean, attempts))
    else:
        print('ERROR: the capture region stayed blank (mean %.1f). Log: %s' % (mean, log_path))
        print('       Pass --allow-black to record anyway.')
        if process.poll() is None and not args.keep_open:
            process.terminate()
        rc.kill_by_title(GAME_TITLE_MARK)
        return 1

    print('capturing client area %d x %d at (%d, %d)' % (rect[2], rect[3], rect[0], rect[1]))
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
        print('leaving the game running (--keep-open).')
        return 0

    print('closing the game window')
    rc.kill_by_title(GAME_TITLE_MARK)
    return 0


if __name__ == '__main__':
    sys.exit(main())
