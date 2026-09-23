# Places a test course of obstacles into the Cartoon City "Demonstration" map, so the
# chasers' steering and jumping can be observed against things that are *deliberately*
# short enough to jump and deliberately too tall to jump.
#
# Run headless (editor must be closed - it holds a lock on the .umap):
#   UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript \
#       -script="<repo>/tools/gen_city_obstacles.py" -unattended -nosplash -nullrhi
#
# Idempotent and *non-destructive by construction*: every actor this file creates is named
# with the DEMO_PREFIX below, and only actors carrying that prefix are ever deleted. The
# city's own 285 static meshes can never be touched, whatever happens here. That is the
# single most important property of this script - the map is a third-party asset and a
# regeneration bug must not be able to damage it.
#
# The course is laid out as a corridor of test cases around the PlayerStart at
# (-230, 0, 162). It is arranged so that reading the log answers the two questions that
# matter: does a chaser go *around* a tall wall (steering), and does it go *over* a low
# crate (jumping)?
#
# The heights are chosen against the chaser's own numbers, which is the whole point:
#   MaxStepHeight   = 45 cm    -> anything below this is walked over, no jump at all
#   MaxJumpHeight   = 120 cm   -> anything above this must be steered around
# so the interesting band is 45..120 cm and the crates sit in the middle of it.
#
# MaxJumpHeight is 120 and not 150 any more, and the reason is that it is bounded by the jump
# ARC rather than by taste: the feet rise at most
#
#     JumpZVelocity^2 / (2 * DefaultGravityZ * GravityScale) = 640^2 / (2 * 980 * 1.6) = 131 cm
#
# so a gate set above that accepts obstacles the chaser will commit to and then fail to clear,
# turning a correct walk-around into a stall. The old 150 made the whole 121..150 range a trap,
# and the platform pads below were originally laid out inside it. See the MaxJumpHeight comment
# in PursuitCharacter.h for the derivation.
#
# WHERE THE GROUND IS, and why this file measures it instead of assuming it.
#
# The first version of this script anchored every obstacle to PLAYER_START_Z (162), which is
# the PlayerStart's own Z - i.e. the capsule centre of the pawn standing there. The plaza
# those obstacles were laid out on is at Z = -10. So the whole course was floating 172 cm in
# the air, well above the head of anything walking under it, and a 120 s verification run
# found no obstacle taller than 46 cm anywhere along the chasers' paths - because the nearest
# thing to the plaza floor was a cloud of test geometry nobody could reach.
#
# The failure was silent in the worst way: the script logged every actor it spawned, with
# heights that read correctly ("height=90" for a 90 cm crate), and the map saved fine. The
# only symptom was in a *different* log, a run later, and it read as the AI being unable to
# find any obstacle rather than as the obstacles being in orbit.
#
# So: trace for the floor at each obstacle's own XY, and place the box on it. A few lines,
# and it removes a whole class of "the fixture is not where I think it is" bugs - the same
# lesson the jump-test crate in PursuitPlayGameMode.cpp learned from the other side.

import unreal

MAP_PATH = "/Game/Cartoon_City_Free/Maps/Demonstration"

CUBE = "/Engine/BasicShapes/Cube.Cube"

TINT_MATERIAL = "/Game/Materials/M_PursuitTint.M_PursuitTint"

# Same prefix contract as tools/gen_play_level.py: it is both the ownership marker and the
# delete guard. Nothing else in this level starts with it.
DEMO_PREFIX = "PX_"

JUMPABLE_COLOUR = unreal.LinearColor(0.16, 0.62, 0.28, 1.0)
WALL_COLOUR = unreal.LinearColor(0.62, 0.30, 0.16, 1.0)

# (label, xy, scale, yaw, kind) - the Z is MEASURED from the level, never assumed.
# Cube is 100 cm across, so scale is metres x 10, and an actor's location is its CENTRE.
# A crate 300 cm wide, 90 cm tall, sitting on ground at z = G:
#   location.z = G + 45   (centre is half the height above the floor)
#
# Everything is placed on the flat plaza around the spawn, clear of the city's own meshes:
# the level's props cluster on the roads, and these sit in the open ground between them.
#
# The plaza is at Z = -10, which is 172 cm BELOW the PlayerStart's own Z of 162 - see the
# note at the top. The ground plane here is still flat across all seven footprints (the
# script logs each measured floor Z so a slope would be visible rather than assumed away),
# which is why one shared measurement is not used and each actor gets its own trace.

LAYOUT = [
    # --- a low crate directly ahead: the "can you jump it" case ------------------
    # 90 cm tall, which is inside MaxJumpHeight (150) and above MaxStepHeight (45), so a
    # chaser has to jump and cannot simply walk up it.
    ("PX_Crate_Low_A", (-230.0, 420.0), (3.0, 3.0, 0.9), 0.0, "jumpable"),

    # A second, taller-but-still-jumpable one at the edge of MaxJumpHeight. If the height
    # test is working, this is the tallest thing a chaser will attempt.
    ("PX_Crate_Low_B", (-230.0, 700.0), (3.0, 3.0, 1.1), 0.0, "jumpable"),

    # --- a wall that must be steered around --------------------------------------
    # 320 cm, far above MaxJumpHeight. A chaser that jumps at this is buggy; a chaser that
    # goes round it is correct. Placed beside the crates so both behaviours are visible in
    # one run without walking anywhere.
    ("PX_Wall_High", (330.0, 420.0), (0.8, 6.0, 3.2), 0.0, "wall"),

    # A second wall at right angles, so there is a corner to round rather than a single face.
    ("PX_Wall_High2", (330.0, 900.0), (6.0, 0.8, 3.2), 0.0, "wall"),

    # --- a kerb, which is the "should NOT jump" case -----------------------------
    # 30 cm, below MaxStepHeight, so the movement component walks over it for free. A
    # player would not notice it and a chaser should not hop it; if the log shows jumps
    # here, the lower bound of the height test is wrong.
    #
    # Moved to (620, 420). Its first spot, (-620, 420), turned out to be a car park: the
    # tallest thing under it was SM_Car_06 at z=163, so the kerb sat on a car roof 153 cm
    # above the road. The standable filter now rejects that outright and the obstacle is
    # relocated rather than silently misplaced - a kerb nobody can reach tests nothing.
    ("PX_Kerb_Low", (620.0, 420.0), (3.0, 2.0, 0.3), 0.0, "kerb"),

    # --- a jumpable gate the chaser has to clear to stay on the player's line -----
    # Two low crates with a gap-free span, far enough from the walls that going round is a
    # long way: this is where jumping should measurably beat steering.
    #
    # The first of the pair moved off (0, 1080) for the same reason as the kerb - SM_Car_16
    # sat there at z=148. The gate is meant to span the chasers' line to the player, and a
    # span with a car in the middle of it is not a span.
    ("PX_Gate_A", (620.0, 1080.0), (2.4, 2.4, 0.9), 0.0, "jumpable"),
    ("PX_Gate_B", (0.0, 1080.0), (2.4, 2.4, 0.9), 0.0, "jumpable"),

    # --- jumping platforms: a ladder, and why it only spans 75 cm ---------------
    # A ladder of flat pads the chaser has to hop onto, plus one deliberate control.
    #
    # BOTH ENDS OF THE BAND ARE PHYSICAL, NOT CHOSEN:
    #   * MaxStepHeight (45 cm) is what the capsule walks over for free. At or below it the
    #     movement component solves the obstacle before the AI ever sees one, so 46 cm is the
    #     first height at which a jump is even possible.
    #   * MaxJumpHeight (120 cm) is the hard refusal in IsObstacleJumpable, and it sits under
    #     the arc's 131 cm apex - the derivation is in the file header.
    # So 44 cm is invisible, 46 cm is a jump and 121 cm is a wall. Height is a switch here,
    # not a difficulty dial: making a jumping obstacle taller does not make it harder, it makes
    # it a different obstacle. That is why the tall platforms below are built as a STACK.
    #
    # Pads are 200x200 cm and spaced 260 cm apart (60 cm of clear air between edges): close
    # enough that a jump reaches the next pad, far enough that the row is not one long ramp.
    #
    # The pad size is also a landing requirement rather than a round number. A chaser that
    # takes off 190 cm short of a face is still driving forward while airborne, and it only
    # comes back down to a 100 cm deck's height ~160 cm past that face - so a pad shallower
    # than about 200 cm is one it flies over instead of onto.
    #
    # Note the chaser only meets these when the chase routes through them. Nothing here makes
    # the *player* climb, so a pad nobody's path crosses sits unused rather than untested.
    ("PX_Step_A", (620.0, 1360.0), (2.0, 2.0, 0.60), 0.0, "jumpable"),
    ("PX_Step_B", (620.0, 1620.0), (2.0, 2.0, 1.10), 0.0, "jumpable"),
    ("PX_Step_C", (620.0, 1880.0), (2.0, 2.0, 1.20), 0.0, "jumpable"),

    # The control: 300 cm with no route to it. It should never be jumped, and a jump logged
    # against it is a bug in the height gate rather than a success. It is also the argument for
    # the towers below in a single actor - same height, opposite outcome, and the only
    # difference is whether there is a way up.
    ("PX_Tower_Tall", (620.0, 2140.0), (2.0, 2.0, 3.00), 0.0, "wall"),

    # --- climbable towers: three decks each, topping out at 300 cm --------------
    #
    # This is how a HIGH platform gets built, and the reason it is not one tall block.
    #
    # Every deck is exactly 100 cm above the one before it, so every step is inside the 45-120
    # band and costs the chaser exactly one jump. The top is 300 cm - more than twice the
    # tallest thing a single arc can clear - and it is reached by climbing rather than by
    # jumping higher. Measured against PX_Tower_Tall this is the whole lesson in one place:
    # a 300 cm block is a wall, and a 300 cm block with two 100 cm steps in front of it is a
    # summit. The geometry is identical; only the route differs.
    #
    # The decks are 300 cm deep, not 200. A chaser jumping from the ground leaves at 190 cm
    # out and touches back down at a deck's height ~160 cm past the face it cleared, and the
    # decks behind the first are entered the same way from the deck below - so 200 cm would
    # put a low deck's landing point over its own far edge, which is how a jump becomes a fall.
    #
    # DECKS SHARE ONE MEASURED FLOOR - hence the sixth field. Measuring each deck separately
    # would let a 15 cm dip under the middle deck silently turn a 100 cm step into a 115 cm one
    # and break the climb, and it would break it in the worst possible way: the tower would
    # still stand, still look right, and simply never be climbed. The floor is measured once,
    # at the bottom deck's own footprint, i.e. on the ground the chaser actually arrives from,
    # and the two decks above reuse that reading.
    #
    # Two towers, on opposite sides of the run, because a chase that never crosses one may
    # still cross the other - a fixture nothing walks into tests nothing.
    ("PX_Deck_1", (900.0, 1360.0), (3.0, 3.0, 1.0), 0.0, "jumpable"),
    ("PX_Deck_2", (900.0, 1740.0), (3.0, 3.0, 2.0), 0.0, "jumpable", "PX_Deck_1"),
    ("PX_Deck_3", (900.0, 2120.0), (3.0, 3.0, 3.0), 0.0, "jumpable", "PX_Deck_1"),

    ("PX_TowerW_1", (-620.0, 1360.0), (3.0, 3.0, 1.0), 0.0, "jumpable"),
    ("PX_TowerW_2", (-620.0, 1740.0), (3.0, 3.0, 2.0), 0.0, "jumpable", "PX_TowerW_1"),
    ("PX_TowerW_3", (-620.0, 2120.0), (3.0, 3.0, 3.0), 0.0, "jumpable", "PX_TowerW_1"),

    # --- a third tower, and why it is not beside the other two ---------------
    # Placement is not decoration here, it is the difference between a fixture and scenery.
    # Both towers above sit at y = 1360-2120, and a 180 s verification run put essentially all
    # of the chase traffic in y = -400..600: the north towers were reached, but only
    # opportunistically (11 jumps near the west one in one run, 3 in the next). A tower the
    # chase walks past once a minute tests one climb a minute.
    #
    # This one goes in the busy band instead - x = 1000, y = 200..960, alongside the obstacles
    # that took 60 and 32 jumps in the same run. Same 100 / 200 / 300 stack, same shared floor.
    #
    # x = 1000 rather than 900 so it clears PX_Kerb_Low's footprint (470..770 in x) without
    # touching it; 900 would have overlapped by 20 cm.
    ("PX_TowerS_1", (1000.0, 200.0), (3.0, 3.0, 1.0), 0.0, "jumpable"),
    ("PX_TowerS_2", (1000.0, 580.0), (3.0, 3.0, 2.0), 0.0, "jumpable", "PX_TowerS_1"),
    ("PX_TowerS_3", (1000.0, 960.0), (3.0, 3.0, 3.0), 0.0, "jumpable", "PX_TowerS_1"),
]


def layout_entries():
    """LAYOUT rows normalised to six fields; the trailing floor_from is optional.

    Only a stacked tower needs a shared floor - see the tower note in LAYOUT - so making the
    field mandatory would put a None on eleven rows to serve six. A row that does carry it
    names an EARLIER row and reuses that row's measured floor, which is what keeps a tower's
    steps exactly the height they were designed to be whatever the ground does between decks.
    """
    for row in LAYOUT:
        yield tuple(row) if len(row) == 6 else tuple(row) + (None,)


def log(message):
    unreal.log("[PursuitAI/city_obstacles] " + str(message))


def fail(message):
    unreal.log_error("[PursuitAI/city_obstacles] " + str(message))
    raise RuntimeError(message)


def try_step(label, fn, default=None):
    try:
        value = fn()
        log("OK   %s -> %s" % (label, value))
        return value
    except Exception as exc:
        log("FAIL %s -> %s: %s" % (label, type(exc).__name__, exc))
        return default


def load_class(path):
    cls = unreal.load_class(None, path)
    if cls is None:
        fail("could not load class " + path)
    return cls


def main():
    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    library = unreal.EditorAssetLibrary

    if level_editor is None or actors is None:
        fail("editor subsystems unavailable - is this an editor commandlet?")

    if not library.does_asset_exist(MAP_PATH):
        fail("map does not exist: " + MAP_PATH)
    if not level_editor.load_level(MAP_PATH):
        fail("failed to load " + MAP_PATH)

    # --- clear only our own previous run -------------------------------------
    # Prefix AND class. The class check is belt and braces on top of the prefix: the city's
    # meshes are StaticMeshActors too, so a prefix match alone is the only thing protecting
    # them, and saying so in code makes the guarantee explicit rather than incidental.
    removed = 0
    kept = 0
    for actor in list(actors.get_all_level_actors()):
        try:
            label = actor.get_actor_label()
        except Exception:
            continue
        if label.startswith(DEMO_PREFIX) and actor.get_class().get_name() == "StaticMeshActor":
            actors.destroy_actor(actor)
            removed += 1
        else:
            kept += 1
    log("cleared %d of my own actor(s); kept %d actor(s) belonging to the city" % (removed, kept))

    cube = library.load_asset(CUBE)
    if cube is None:
        fail("engine cube mesh is missing")

    tint_material = library.load_asset(TINT_MATERIAL)
    if tint_material is None:
        log("WARNING: %s missing; obstacles will render default grey. Run tools/gen_tint_material.py"
            % TINT_MATERIAL)

    # One material instance per kind, so "the green ones are jumpable" is a fact a viewer can
    # use rather than something they have to be told.
    def make_instance(name, colour):
        path = "/Game/Materials/" + name
        if library.does_asset_exist(path):
            instance = library.load_asset(path)
        else:
            library.make_directory("/Game/Materials")
            factory = unreal.MaterialInstanceConstantFactoryNew()
            tools = unreal.AssetToolsHelpers.get_asset_tools()
            instance = tools.create_asset(name, "/Game/Materials",
                                          unreal.MaterialInstanceConstant, factory)
            if instance is None:
                raise RuntimeError("create_asset returned None for " + name)
        unreal.MaterialEditingLibrary.set_material_instance_parent(instance, tint_material)
        unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
            instance, "Color", colour)
        library.save_loaded_asset(instance)
        return instance

    jumpable_material = try_step("MI_CityJumpable",
                                 lambda: make_instance("MI_CityJumpable", JUMPABLE_COLOUR))
    wall_material = try_step("MI_CityWall", lambda: make_instance("MI_CityWall", WALL_COLOUR))

    static_mesh_actor_class = load_class("/Script/Engine.StaticMeshActor")

    # ---------------------------------------------------------------------------------
    # Finding the floor WITHOUT a collision trace.
    #
    # Physics queries do not work from this commandlet. Measured on UE 5.7.4, with the
    # Cartoon City map loaded and 292 actors present: every candidate binding returned a bare
    # None, including a cast straight down through the middle of a known-solid StaticMeshActor
    # whose component honestly reports the BlockAll profile.
    #
    #   unreal.SystemLibrary.line_trace_single        -> None
    #   unreal.SystemLibrary.line_trace_single_new    -> None
    #   unreal.SystemLibrary.line_trace_single_by_profile("BlockAll") -> None
    #   world.line_trace_single / sweep_single        -> attribute does not exist
    #
    # A bare None is not a miss - a miss comes back as a (bool, HitResult) pair - so the
    # wrapper was declining the call, not reporting empty space. (`SystemLibrary` also has no
    # line_trace_single_by_channel at all on this build, despite that spelling appearing in
    # docs; the channel-taking call is plain line_trace_single, whose live TypeError gave its
    # real signature.) Two dozen variations over three probes all came back empty, so this is
    # a property of the environment rather than of the argument list.
    #
    # What DOES work is get_actor_bounds, so the floor is taken from the world geometry
    # instead: the highest surface belonging to an actor that is genuinely on the ground, at
    # the obstacle's own XY. Bounds are exact for the axis-aligned boxes this city is built
    # out of and were already verified by tools/inspect_demo_city.py.
    # ---------------------------------------------------------------------------------

    # Snapshot the level's geometry once: (label, min_z, max_z, min_x, max_x, min_y, max_y).
    # A city of 292 actors is cheap to walk, and sampling it per obstacle would be 7x the work
    # for the same answer.
    surfaces = []
    for actor in actors.get_all_level_actors():
        try:
            if actor.get_class().get_name() != "StaticMeshActor":
                continue
            label = actor.get_actor_label()
            if label.startswith(DEMO_PREFIX):
                # Our own previous course, which a re-run destroys moments from now. Letting it
                # count as ground would make the floor of a re-run depend on the rig we are
                # about to delete, so it would drift upward every time this script ran.
                continue
            origin, extent = actor.get_actor_bounds(False)
            surfaces.append((
                label,
                origin.z - extent.z, origin.z + extent.z,
                origin.x - extent.x, origin.x + extent.x,
                origin.y - extent.y, origin.y + extent.y,
            ))
        except Exception:
            continue

    log("scanned %d city surface(s) for floor heights" % len(surfaces))

    # The plaza floor, as a sanity anchor: the median top of everything in the level. Most of
    # a city's actor count is street furniture sitting on the ground, so the median is the
    # pavement, and a measurement far from it is worth reporting.
    tops = sorted(surface[2] for surface in surfaces)
    median_top = tops[len(tops) // 2] if tops else 0.0
    log("median top of all city geometry: z=%.0f" % median_top)

    # The bands a "you can stand here" surface can be in. Everything is measured from the
    # level's own median, so this file needs no hand-tuned constant per city:
    #   * a surface more than 250 cm above the median is a wall, a roof, a tree - you cannot
    #     put an obstacle on it and expect anyone on the pavement to meet it;
    #   * a surface more than 150 cm BELOW the median is a basement or a drain.
    #
    # This exists because the first bounds-based run picked cars: PX_Kerb_Low landed at
    # floor z=163 "on SM_Car_06" and PX_Gate_A at z=148 "on SM_Car_16", i.e. on the roofs of
    # two parked vehicles, because a car's bounding box is genuinely the highest thing whose
    # footprint covers those points. The boxes were correctly placed and useless: no chaser
    # walks on a car roof, so the gate would never have been in anyone's path.
    # (The band alone did NOT catch them - a car roof is only 63 cm above this city's median,
    # which is why the name filter below exists as well. Two independent guards, because
    # "highest thing under me" is the wrong question when the thing might be a vehicle.)
    STANDABLE_ABOVE = 250.0
    STANDABLE_BELOW = 150.0
    low_band = median_top - STANDABLE_BELOW
    high_band = median_top + STANDABLE_ABOVE

    # Things nobody can stand on, whatever their bounds say. Matched against the actor label,
    # which in this pack is descriptive (SM_Car_16, SM_Van_Wheel_Rear_Right) rather than
    # generated - so this is a name filter that is legible in the level, not a heuristic on
    # numbers. A city without cars simply never matches anything here.
    NOT_STANDABLE_HINTS = ("Car", "Van", "Truck", "Bus", "Bike", "Motor")

    def is_standable(label, max_z):
        if any(hint.lower() in label.lower() for hint in NOT_STANDABLE_HINTS):
            return False
        return low_band <= max_z <= high_band

    def ground_z_at(x, y):
        """Top Z of the highest STANDABLE surface whose footprint contains (x, y), or None.

        "Highest" rather than "nearest": an obstacle must sit on top of whatever is already
        there, so a kerb under the footprint wins over the paving under that. But only
        surfaces that pass is_standable count, which is what keeps a crate off a car roof.

        Returns (z, label) or (None, None). The caller skips the obstacle rather than placing
        it at a guess - see there.
        """
        best = None
        best_label = None
        for label, min_z, max_z, min_x, max_x, min_y, max_y in surfaces:
            if min_x <= x <= max_x and min_y <= y <= max_y:
                if not is_standable(label, max_z):
                    continue
                if best is None or max_z > best:
                    best = max_z
                    best_label = label
        if best is None:
            return None, None
        return best, best_label

    # Anything the filter rejected at one of our own footprints is named in the log: a re-run
    # whose obstacles move because a car parked on the spot is a fact about the level, not a
    # bug, and the log should say so rather than leaving the change unexplained.
    # A floor_from that names a row which does not exist would fall back to measuring its own
    # floor and quietly change a tower's step heights, so it is checked before anything spawns
    # rather than discovered in a run log one map later. Fatal, because it is a typo in this
    # file, the map has not been touched yet, and the fix is one word.
    seen_labels = set()
    for label, xy, scale, yaw, kind, floor_from in layout_entries():
        if floor_from is not None and floor_from not in seen_labels:
            fail("%s names floor_from='%s', which is not an earlier row of LAYOUT"
                 % (label, floor_from))
        seen_labels.add(label)

    for label, xy, scale, yaw, kind, floor_from in layout_entries():
        rejected = [
            (surface[0], surface[2]) for surface in surfaces
            if surface[3] <= xy[0] <= surface[4]
            and surface[5] <= xy[1] <= surface[6]
            and not is_standable(surface[0], surface[2])
        ]
        if rejected:
            log("  note: %s at (%.0f, %.0f) - rejected %s"
                % (label, xy[0], xy[1],
                   ", ".join("%s@z=%.0f" % (n, z) for n, z in rejected[:4])))

    spawned = 0
    # Floors measured so far this run, by label, so a stacked tower's upper decks can reuse the
    # reading taken at its base instead of taking their own.
    measured = {}
    for label, xy, scale, yaw, kind, floor_from in layout_entries():
        x, y = xy[0], xy[1]

        # The box's own height is scale[2] metres x 100 cm, because the engine Cube is 1 m.
        height = scale[2] * 100.0

        if floor_from is not None and floor_from in measured:
            ground, ground_label = measured[floor_from]
            # The deck's own floor is still measured, purely so the log can say whether the two
            # agree. Sharing the base's reading is deliberate - see the tower note - but "shares
            # it on purpose" and "would have found the same thing" are different claims, and the
            # gap between them is a deck embedded in a building: correct-looking in the log,
            # invisible in the level, and never climbed.
            own, own_label = ground_z_at(x, y)
            if own is None:
                log("  %-16s floor z=%.0f (SHARED with '%s'); own XY has NO standable surface - "
                    "check this deck is not inside geometry" % (label, ground, floor_from))
            elif abs(own - ground) > 1.0:
                log("  %-16s floor z=%.0f (shared with '%s'); own XY reads z=%.0f (on '%s') - "
                    "delta %.0f" % (label, ground, floor_from, own, own_label, own - ground))
            else:
                log("  %-16s floor z=%.0f (shared with '%s'; own XY agrees)"
                    % (label, ground, floor_from))
        else:
            ground, ground_label = ground_z_at(x, y)
        if ground is None:
            # Loud, and skipped rather than guessed at. A floating obstacle is worse than a
            # missing one: it is invisible to a chaser standing on the plaza, so a run over
            # it produces a false "the AI cannot find anything to jump" result - which is
            # exactly the failure this whole rewrite exists to fix.
            log("FAIL %s - nothing under (%.0f, %.0f); NOT spawning a floating box"
                % (label, x, y))
            continue

        # Remembered before the spawn, so a failure below cannot leave a later deck sharing a
        # floor from a row that never produced an actor.
        measured[label] = (ground, ground_label)

        # Position the CENTRE half the box's height above the measured floor.
        location = (x, y, ground + height * 0.5)
        log("  %-16s floor z=%.0f (on '%s') -> centre z=%.0f (height %.0f)"
            % (label, ground, ground_label, location[2], height))

        actor = actors.spawn_actor_from_class(
            static_mesh_actor_class,
            unreal.Vector(location[0], location[1], location[2]),
            # Keyword: unreal.Rotator's positional order is (roll, pitch, yaw), which is not
            # C++'s FRotator(Pitch, Yaw, Roll). Rotator(0, 90, 0) is a 90-degree PITCH.
            unreal.Rotator(yaw=yaw),
        )
        if actor is None:
            fail("failed to spawn " + label)
        actor.set_actor_label(label)
        actor.set_actor_scale3d(unreal.Vector(scale[0], scale[1], scale[2]))

        component = actor.get_editor_property("static_mesh_component")
        component.set_editor_property("static_mesh", cube)
        # Movable so the city's Movable sun lights it with no lighting build, exactly as the
        # play level does. Static geometry here would need a bake this commandlet cannot do.
        component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)

        material = jumpable_material if kind in ("jumpable", "kerb") else wall_material
        if material is not None:
            def tint(c=component, m=material):
                c.set_material(0, m)
                return m.get_name()
            try_step("tint " + label, tint)

        # Collision profile: BlockAll is what makes the obstacle real to both the chaser's
        # feelers (ECC_WorldStatic) and the capsule. Explicit rather than inherited, because
        # the whole point of this actor is that it blocks.
        try_step("collision " + label, lambda c=component: (
            c.set_collision_profile_name("BlockAll"),
            c.get_collision_profile_name())[1])

        # Read the physics-relevant geometry *back*, rather than trusting the numbers above.
        # Scale, mesh bounds and location are three separate things that can each be wrong,
        # and the height the jump test actually sees is derived from all of them.
        #
        # The "above floor" figure is the one that matters and the one the earlier version of
        # this script could not print: it is what the chaser's jump decision compares against
        # MaxStepHeight and MaxJumpHeight, and a box 90 cm tall floating 172 cm up reports
        # "height=90" while presenting nothing at all to anyone on the ground.
        try:
            origin, extent = actor.get_actor_bounds(False)
            log("  %-16s %-9s centre z=%.0f top z=%.0f height=%.0f above floor=%.0f extent=(%.0f, %.0f, %.0f)"
                % (label, kind, origin.z, origin.z + extent.z, extent.z * 2.0,
                   origin.z + extent.z - ground, extent.x, extent.y, extent.z))
        except Exception as exc:
            log("  %-16s %-9s bounds unavailable: %s" % (label, kind, exc))

        spawned += 1

    log("spawned %d of %d obstacle(s)" % (spawned, len(LAYOUT)))

    if not level_editor.save_current_level():
        fail("save_current_level failed for " + MAP_PATH)

    log("OK - %s saved with %d obstacle(s) from this generator" % (MAP_PATH, spawned))


main()
