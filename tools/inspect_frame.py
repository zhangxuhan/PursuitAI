# Copyright Epic Games, Inc. All Rights Reserved.
#
# Answer "what is actually on screen?" from a screenshot, numerically.
#
#   ./venv/Scripts/python.exe tools/inspect_frame.py logs/hero.png
#   ./venv/Scripts/python.exe tools/inspect_frame.py logs/hero.png --profile 648,340,410
#
# Why this exists:
#   A screenshot was the only evidence for three real bugs in the watch scene (a stray
#   pawn sphere, doubled shadows, an obscuring floor), and in each case the eye read it
#   wrong first. This turns the picture into numbers:
#
#   * a blob pass listing everything darker than the lit floor, with bounding box,
#     centroid and mean colour - the fastest way to spot an object nobody asked for;
#   * an optional luminance profile along a line through an object, which is what
#     separates a *shadow* from a *mesh*: a mesh has a highlight brighter than its
#     surroundings, a shadow is never brighter than the floor it falls on.
#
# Reading the output:
#   bbox/centre are in screenshot pixels. A centre that lands on the exact middle of the
#   client area keeps landing there across frames -> the object sits on the world point
#   the camera is aimed at, which is a strong hint about where it comes from.

import argparse
import sys
from collections import deque

from PIL import Image

MIN_BLOB_PIXELS = 60
FLOOR_LUMA = 120  # anything below this counts as "not lit floor and not void"


def luma(r, g, b):
    return (r * 3 + g * 6 + b) // 10


def blob_pass(image, margin_top=0, margin_bottom=40, margin_side=6):
    width, height = image.size
    pixels = image.load()
    x0, x1 = margin_side, width - margin_side
    y0, y1 = margin_top, height - margin_bottom
    w, h = x1 - x0, y1 - y0

    mask = [[False] * w for _ in range(h)]
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = pixels[x, y]
            if luma(r, g, b) < FLOOR_LUMA:
                mask[y - y0][x - x0] = True

    seen = [[False] * w for _ in range(h)]
    blobs = []
    for j in range(h):
        for i in range(w):
            if not mask[j][i] or seen[j][i]:
                continue
            queue = deque([(i, j)])
            seen[j][i] = True
            points = []
            while queue:
                a, bq = queue.popleft()
                points.append((a, bq))
                for da, db in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    na, nb = a + da, bq + db
                    if 0 <= na < w and 0 <= nb < h and mask[nb][na] and not seen[nb][na]:
                        seen[nb][na] = True
                        queue.append((na, nb))
            if len(points) < MIN_BLOB_PIXELS:
                continue

            xs = [p[0] + x0 for p in points]
            ys = [p[1] + y0 for p in points]
            colours = [pixels[p[0] + x0, p[1] + y0] for p in points]
            mr = sum(c[0] for c in colours) // len(colours)
            mg = sum(c[1] for c in colours) // len(colours)
            mb = sum(c[2] for c in colours) // len(colours)
            if mg - max(mr, mb) > 10:
                kind = 'green'
            elif mr - max(mg, mb) > 20:
                kind = 'red'
            else:
                kind = 'dark'
            blobs.append((len(points), kind, (min(xs), min(ys), max(xs), max(ys)),
                          (sum(xs) // len(xs), sum(ys) // len(ys)), (mr, mg, mb)))

    blobs.sort(reverse=True)
    return blobs


def profile(image, x, y_from, y_to):
    pixels = image.load()
    print('luminance profile at x=%d, y=%d..%d' % (x, y_from, y_to))
    for y in range(y_from, y_to + 1, 5):
        r, g, b = pixels[x, y]
        print('  y=%3d rgb=(%3d,%3d,%3d) luma=%3d' % (y, r, g, b, luma(r, g, b)))
    print('  -> if every sample is at or below the floor luma, this is a shadow.')
    print('     A value above the floor luma means a lit surface (a mesh).')


def main():
    parser = argparse.ArgumentParser(description='Report the objects in a screenshot.')
    parser.add_argument('image')
    parser.add_argument('--profile', default=None,
                        help='x,y_start,y_end - print a luminance profile down that column')
    parser.add_argument('--margin-top', type=int, default=0,
                        help='skip this many rows at the top (engine warning text lives there)')
    args = parser.parse_args()

    image = Image.open(args.image).convert('RGB')
    print('image: %s  size=%s' % (args.image, image.size))

    blobs = blob_pass(image, margin_top=args.margin_top)
    print('objects darker than the lit floor: %d' % len(blobs))
    for count, kind, bbox, centre, colour in blobs[:12]:
        print('  px=%6d %-5s bbox=%-24s centre=%-12s meanRGB=%s'
              % (count, kind, bbox, centre, colour))

    if args.profile:
        x, y_from, y_to = (int(v) for v in args.profile.split(','))
        profile(image, x, y_from, y_to)
    return 0


if __name__ == '__main__':
    sys.exit(main())
