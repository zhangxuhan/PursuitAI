# Measures the Cartoon City "Demonstration" map so the god camera's height can be chosen
# from numbers instead of guessed.
#
#   powershell -File tools/run_pyscript.ps1 -Script <repo>\tools\inspect_city_scale.py \
#            -Log <repo>\logs\inspect_city_scale.log
#
# Why this exists rather than a "just put it 3000 up" default: the whole point of the god
# camera is that nothing occludes the chase, and in a city the thing that occludes is a
# building between the lens and the street. "High enough" is therefore a measured property
# of this level - the top of its tallest geometry - not a taste decision, and this script
# prints exactly that plus the level's footprint so the framing can be sized at the same
# time.
#
# Output goes to a FILE, not to unreal.log: the pythonscript commandlet swallows
# unreal.log() output even with -stdout, so a script that logs and exits leaves nothing
# behind to read. Writing as it goes is deliberate too - if a later query kills the
# script (the physics wrappers in this UE build can), everything measured so far survives.

import os
import traceback

import unreal

LEVEL_PATH = "/Game/Cartoon_City_Free/Maps/Demonstration"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", "city_scale.txt")
OUT = os.path.normpath(OUT)

_lines = []


def log(message):
    """Append a line and flush it immediately. See the file note above."""
    _lines.append(str(message))
    try:
        with open(OUT, "w", encoding="utf-8") as handle:
            handle.write("\n".join(_lines) + "\n")
    except OSError:
        pass


def main():
    log("level: " + LEVEL_PATH)

    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level_editor.load_level(LEVEL_PATH):
        log("ERROR: could not load the level")
        return

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    log("actor count: %d" % len(actors))

    # Bounds are taken over the actors that are actually geometry. Lights, PlayerStarts and
    # the like have degenerate or absurd bounds and would poison the extent.
    boxes = []
    for actor in actors:
        cls = actor.get_class().get_name()
        if "StaticMeshActor" not in cls and "SkeletalMeshActor" not in cls:
            continue
        try:
            origin, extent = actor.get_actor_bounds(False)
        except Exception as exc:
            log("  bounds unavailable for %s (%s)" % (actor.get_actor_label(), exc))
            continue
        boxes.append((actor.get_actor_label(), cls,
                      origin.x, origin.y, origin.z, extent.x, extent.y, extent.z))

    log("geometry actors: %d" % len(boxes))
    if not boxes:
        return

    min_x = min(b[2] - b[5] for b in boxes)
    max_x = max(b[2] + b[5] for b in boxes)
    min_y = min(b[3] - b[6] for b in boxes)
    max_y = max(b[3] + b[6] for b in boxes)
    min_z = min(b[4] - b[7] for b in boxes)
    max_z = max(b[4] + b[7] for b in boxes)

    log("WORLD  x=[%.0f, %.0f] (%.0f wide)" % (min_x, max_x, max_x - min_x))
    log("WORLD  y=[%.0f, %.0f] (%.0f deep)" % (min_y, max_y, max_y - min_y))
    log("WORLD  z=[%.0f, %.0f] (tallest top %.0f)" % (min_z, max_z, max_z))

    # The tallest 15, because "whichever building is between the camera and the street" is a
    # question about the tall ones only.
    log("--- tallest geometry (top z, descending) ---")
    boxes.sort(key=lambda b: b[4] + b[7], reverse=True)
    for b in boxes[:15]:
        log("  top=%-6.0f base=%-6.0f  xy=(%.0f, %.0f) half=(%.0f, %.0f)  %s"
            % (b[4] + b[7], b[4] - b[7], b[2], b[3], b[5], b[6], b[0]))

    # What a downward-looking camera would see: the horizontal footprint of everything whose
    # top is above a few candidate heights. A height at which the count collapses is a height
    # at which only a handful of towers remain as occluders.
    log("--- occluder count above a candidate camera height ---")
    for height in (1000, 1500, 2000, 2500, 3000, 4000, 5000):
        count = sum(1 for b in boxes if (b[4] + b[7]) > height)
        log("  above %.0f: %d actor(s)" % (height, count))

    for actor in actors:
        if actor.get_class().get_name() == "PlayerStart":
            loc = actor.get_actor_location()
            log("PLAYERSTART (%.0f, %.0f, %.0f)" % (loc.x, loc.y, loc.z))

    log("done")


try:
    main()
except Exception:
    log("EXCEPTION:\n" + traceback.format_exc())
