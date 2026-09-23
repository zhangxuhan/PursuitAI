# Finds a clear 10x10 m arena patch + ground height on the Demonstration city map, so the
# env box (PursuitAIEnv) can be staged in the city instead of the grey training rig.
#
#   powershell -File tools/run_pyscript.ps1 -Script <repo>\tools\inspect_city_stage.py \
#            -Log <repo>\logs\city_stage.txt
#
# Same output-to-file rule as inspect_city_scale.py: the pythonscript commandlet swallows
# unreal.log(), so anything worth reading is written as it goes.

import os
import traceback

import unreal

LEVEL_PATH = "/Game/Cartoon_City_Free/Maps/Demonstration"
OUT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "logs", "city_stage.txt"))

# The env arena is a 1000x1000 cm box (ArenaHalfSize=500). The patch must clear it with a
# margin so the balls never sit inside a wall, and the god-style camera above it needs its
# own vertical corridor - which any open sky above a street provides.
ARENA_HALF = 500.0
MARGIN = 150.0
BLOCK_HALF = ARENA_HALF + MARGIN

_lines = []


def log(message):
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

    # Ground-standing occluders: geometry whose base is near the road and whose top is
    # meaningfully above it. Rooftop clutter (bases above head height) cannot matter to
    # balls that live below 2 m, and the road itself has its top below 500.
    occluders = []
    road_boxes = []
    player_start = None

    for actor in actors:
        cls = actor.get_class().get_name()
        if cls == "PlayerStart":
            player_start = actor.get_actor_location()
            continue
        if "StaticMeshActor" not in cls and "SkeletalMeshActor" not in cls:
            continue
        try:
            origin, extent = actor.get_actor_bounds(False)
        except Exception:
            continue
        base = origin.z - extent.z
        top = origin.z + extent.z
        if top > 500.0 and base < 800.0:
            occluders.append((origin.x - extent.x, origin.x + extent.x,
                              origin.y - extent.y, origin.y + extent.y,
                              base, top, actor.get_actor_label()))
        if top < 600.0:
            road_boxes.append((origin.x - extent.x, origin.x + extent.x,
                               origin.y - extent.y, origin.y + extent.y,
                               top))

    log("geometry occluders: %d   road-level boxes: %d" % (len(occluders), len(road_boxes)))
    if player_start:
        log("PLAYERSTART (%.0f, %.0f, %.0f)" % (player_start.x, player_start.y, player_start.z))

    def ground_z_at(x, y):
        """Highest road-like surface covering this point (top < 600)."""
        best = None
        for x0, x1, y0, y1, top in road_boxes:
            if x0 <= x <= x1 and y0 <= y <= y1:
                best = top if best is None else max(best, top)
        return best

    def patch_clear(cx, cy):
        """True when no occluder's XY footprint enters the blocking square."""
        x0, x1 = cx - BLOCK_HALF, cx + BLOCK_HALF
        y0, y1 = cy - BLOCK_HALF, cy + BLOCK_HALF
        for ox0, ox1, oy0, oy1, _base, _top, _label in occluders:
            if ox0 < x1 and ox1 > x0 and oy0 < y1 and oy1 > y0:
                return False
        return True

    # Sweep a grid over the streets around the player start - the whole map is too big to
    # be worth scanning and the opening shot wants the neighbourhood the camera already
    # knows. Candidate centres every 250 cm.
    xs = range(-2000, 2751, 250)
    ys = range(-2000, 2001, 250)

    candidates = []
    for cx in xs:
        for cy in ys:
            if not patch_clear(cx, cy):
                continue
            gz = ground_z_at(cx, cy)
            if gz is None:
                continue
            d = 0.0
            if player_start:
                d = ((cx - player_start.x) ** 2 + (cy - player_start.y) ** 2) ** 0.5
            candidates.append((d, cx, cy, gz))

    candidates.sort()
    log("--- clear patches (distance from PlayerStart, centre, ground z) ---")
    for d, cx, cy, gz in candidates[:15]:
        log("  d=%-6.0f centre=(%+.0f, %+.0f) ground=%.0f" % (d, cx, cy, gz))

    if candidates:
        d, cx, cy, gz = candidates[0]
        log("")
        log("PICK  -PursuitArenaAt=%d,%d,%d" % (cx, cy, int(round(gz)) + 60))
        log("      (centre XY of the clearest patch, Z = ground + 60 so the slab bottom")
        log("       sits just above the road with ArenaHalfHeight=60)")

    log("done")


try:
    main()
except Exception:
    log("EXCEPTION:\n" + traceback.format_exc())
