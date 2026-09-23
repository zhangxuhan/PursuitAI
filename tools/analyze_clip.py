"""Measure a recorded clip instead of looking at it.

Why this exists
---------------
"The god camera filmed a wall for four seconds" is the failure mode this project keeps
hitting, and every version of it was found by a person watching a video. That does not
scale, and on this machine it does not even work: reading a PNG back through the tooling
returns "Content filtered", so a frame-by-frame look at a 30 s recording is not available.

So the clip gets measured. The gate is the two failures that have actually happened:

  flat     - the fraction of the frame sitting in one colour bucket. A camera that has ended
             up inside a building is looking at one flat wall, and no amount of "it looked
             orange" beats 0.94 as evidence. Cartoon City from 29 m up measures around
             0.10-0.31, because the modal bucket is road or roof and everything else is a
             different surface; measured on the first clip that passed, max 0.312.
  luma     - mean brightness. The other known failure is a black frame: the window exists,
             the level has not loaded, and the log still says the game is running.

The `green` and `red` columns are DIAGNOSTICS and not a gate, which took a calibration run
to learn. The idea was "the chasers are tinted (0.14, 0.62, 0.22) and the player is tinted
(0.70, 0.09, 0.09), so a count proves the subject is on screen". Cartoon City is full of
green trees and red cars: green ran up to 297000 pixels on frames whose subject was a park,
and red never dropped below 900 even on frames where the chasers were nowhere. So the
counts are reported for a human to read alongside the timestamps, and only a frame with
BOTH at zero is called empty - that one is unambiguous, because it means no vegetation, no
car and no pawn of either colour was in shot.

Usage
-----
    .venv/Scripts/python.exe tools/analyze_clip.py logs/portfolio/god_chase_30s.mp4
    ... --fps 2 --report logs/clip_report_god.txt

Exit code is 0 when the clip passes, 1 when any sampled frame is black or is one flat
surface. That makes it usable as a gate in a recording loop.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image


# One frame in a bucket this wide is "one colour". 16 levels per channel is coarse enough
# that a gradient across a wall still registers as a single bucket, and fine enough that a
# street with buildings, road and sky does not.
BUCKET = 16

# A frame this flat is not a city. Measured pass on the reference clip: 0.312 worst case.
FLAT_DEGENERATE = 0.88

# Below this the level is not lit or has not loaded.
LUMA_BLACK = 12.0


def tint_counts(rgb):
    """(green-dominant, red-dominant) pixel counts. See the module docstring: diagnostics."""
    r = rgb[..., 0].astype(np.int16)
    g = rgb[..., 1].astype(np.int16)
    b = rgb[..., 2].astype(np.int16)
    green = np.count_nonzero((g >= 90) & (g > r * 1.25) & (g > b * 1.25))
    red = np.count_nonzero((r >= 90) & (r > g * 1.4) & (r > b * 1.4))
    return int(green), int(red)


def flat_fraction(rgb):
    """Fraction of the frame in its single most common coarse colour bucket."""
    buckets = (rgb // BUCKET).astype(np.int32)
    keys = (buckets[..., 0] << 16) | (buckets[..., 1] << 8) | buckets[..., 2]
    counts = np.bincount(keys.ravel())
    return float(counts.max()) / float(keys.size)


def luma(rgb):
    return float(rgb[..., 0].mean() * 0.2126
                 + rgb[..., 1].mean() * 0.7152
                 + rgb[..., 2].mean() * 0.0722)


def extract(video, fps, out_dir):
    """Pull frames out with ffmpeg. Returns them sorted by name (== by time)."""
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        raise SystemExit('ERROR: ffmpeg not on PATH.')

    os.makedirs(out_dir, exist_ok=True)
    pattern = os.path.join(out_dir, 'f_%05d.png')

    proc = subprocess.run(
        [ffmpeg, '-y', '-loglevel', 'error', '-i', video,
         '-vf', 'fps=%d' % fps, pattern],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit('ERROR: ffmpeg failed (%d):\n%s' % (proc.returncode, proc.stderr))

    frames = sorted(f for f in os.listdir(out_dir) if f.endswith('.png'))
    if not frames:
        raise SystemExit('ERROR: ffmpeg produced no frames - is the file a video?')
    return [os.path.join(out_dir, f) for f in frames]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('video')
    parser.add_argument('--fps', type=int, default=2,
                        help='frames per second to sample (default 2)')
    parser.add_argument('--report', default=None,
                        help='write the report here as well as to stdout')
    parser.add_argument('--keep', action='store_true',
                        help='keep the extracted frames (they are deleted by default)')
    args = parser.parse_args()

    if not os.path.isfile(args.video):
        raise SystemExit('ERROR: no such file: %s' % args.video)

    name = os.path.splitext(os.path.basename(args.video))[0]
    out_dir = os.path.join(os.path.dirname(os.path.abspath(args.video)),
                           '_frames_%s' % name)
    work_dir = out_dir if args.keep else tempfile.mkdtemp(prefix='clipframes_')

    try:
        frames = extract(args.video, args.fps, work_dir)
        size = Image.open(frames[0]).size

        lines = []
        lines.append('clip      %s' % args.video)
        lines.append('sampled   %d frames at %d fps, %dx%d'
                     % (len(frames), args.fps, size[0], size[1]))
        lines.append('')
        lines.append('  t(s)    luma    flat    green     red   verdict')
        lines.append('  ------------------------------------------------------------')

        bad = []
        empties = []
        flats = []
        lumas = []
        greens = []
        reds = []

        for index, path in enumerate(frames):
            rgb = np.asarray(Image.open(path).convert('RGB'))
            t = float(index) / float(args.fps)
            f = flat_fraction(rgb)
            lu = luma(rgb)
            gc, rc = tint_counts(rgb)

            flats.append(f)
            lumas.append(lu)
            greens.append(gc)
            reds.append(rc)

            notes = []
            if lu < LUMA_BLACK:
                notes.append('BLACK - level not loaded?')
            if f >= FLAT_DEGENERATE:
                notes.append('DEGENERATE - one surface fills the frame')
            if gc == 0 and rc == 0:
                notes.append('EMPTY - no vegetation, no car, no pawn of either tint')
                empties.append(t)
            if notes:
                bad.append((t, notes))

            lines.append('  %5.1f  %6.1f  %6.3f  %6d  %6d   %s'
                         % (t, lu, f, gc, rc, '; '.join(notes) if notes else 'ok'))

        lines.append('')
        lines.append('flat   max %.3f  mean %.3f   (degenerate at %.2f)'
                     % (max(flats), sum(flats) / len(flats), FLAT_DEGENERATE))
        lines.append('luma   min %.1f  mean %.1f   (black below %.0f)'
                     % (min(lumas), sum(lumas) / len(lumas), LUMA_BLACK))
        lines.append('green  min %d  max %d   (diagnostic: trees dominate this map)'
                     % (min(greens), max(greens)))
        lines.append('red    min %d  max %d   (diagnostic: cars dominate this map)'
                     % (min(reds), max(reds)))
        lines.append('')

        if bad:
            lines.append('FAIL - %d of %d sampled frames:' % (len(bad), len(frames)))
            for t, notes in bad:
                lines.append('  t=%.1fs  %s' % (t, '; '.join(notes)))
        else:
            lines.append('PASS - every sampled frame is lit and is not a single flat surface.')

        if empties:
            lines.append('')
            lines.append('note: %d frame(s) had nothing but terrain in shot, at t=%s.'
                         % (len(empties), ', '.join('%.1fs' % t for t in empties)))
            lines.append('      Expected when the chase crosses open ground with the chasers '
                         'out of the lens;')
            lines.append('      only a run of them from the same moment is worth looking at.')

        report = '\n'.join(lines)
        print(report)

        if args.report:
            with open(args.report, 'w', encoding='utf-8') as handle:
                handle.write(report + '\n')
            print('\nwrote %s' % args.report)

        hard_bad = [t for t, notes in bad
                    if any('BLACK' in n or 'DEGENERATE' in n for n in notes)]
        return 1 if hard_bad else 0

    finally:
        if not args.keep:
            shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
