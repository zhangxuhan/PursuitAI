# Generates the playable level for PursuitAI, L_PursuitPlay, headlessly.
#
# Run:
#   UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript \
#       -script="<repo>/tools/gen_play_level.py" -unattended -nosplash -nullrhi
#
# Idempotent: everything it generated last time is deleted and rebuilt, so re-running is how
# you change the arena rather than something to avoid. Only actors of the classes listed in
# GENERATED_CLASSES are removed, so anything placed by hand survives.
#
# The layout is deliberately sparse and legible: a walled square, a few blocks, a handful of
# pillars. It exists to make the chase interesting - straight lines that get cut off, corners
# to round, no dead ends to hide in.
#
# All lights are Movable on purpose. A Static light needs built lighting data, and this level
# is generated and then played immediately, so there is nothing to build - and a map that
# renders black because its lighting was never built is a very confusing way to find that out.
#
# The light recipe is copied from APursuitAIEnv rather than invented here. This engine's exposure
# is effectively fixed (with no PostProcessVolume in the scene, SceneView pins Min=Max=1), so the
# light intensity *is* the brightness - there is no auto-exposure to renormalise anything. The
# project's calibration is a key at 0.4 lux plus a shadowless fill at 0.1, which lands the floor
# around (157,158,160); reusing it means a screenshot of this map and a screenshot of the training
# map are directly comparable. Do not raise the numbers without re-measuring.
#
# Honest note on how this file got here: the first version used 3.2 lux plus a SkyLight plus a
# SkyAtmosphere, and every screenshot was black. That was NOT the lighting - it was the PlayerStart
# pointing straight up (see the note in the layout loop), which put the follow camera inside the
# player's capsule looking at empty sky. Both changes landed together, so which of them changed the
# brightness was never measured. 3.2 lux is 8x the calibration above and would probably have
# clipped to white once the camera was fixed; "probably" is the reason this comment says so
# instead of claiming a number.
#
# SkyLight and SkyAtmosphere are still deliberately absent. A sky lit by a 0.4 lux sun is not
# worth its cost, and each one is another variable in a scene that was being debugged.

import unreal

LEVEL_PATH = "/Game/Maps/L_PursuitPlay"
GAME_MODE_CLASS = "/Script/PursuitAI.PursuitPlayGameMode"

CUBE = "/Engine/BasicShapes/Cube.Cube"
CYLINDER = "/Engine/BasicShapes/Cylinder.Cylinder"

# The project's own material, not BasicShapeMaterial. It carries the same "Color" vector, but
# unlike the engine asset it has bUsedWithSkeletalMesh set - which is what lets the characters
# wear a tinted instance instead of falling back to the default material at runtime.
TINT_MATERIAL = "/Game/Materials/M_PursuitTint.M_PursuitTint"

# Colours reused from the training scene's language: neutral ground, everything the player has
# to notice is either grey-blue or red.
FLOOR_COLOUR = unreal.LinearColor(0.16, 0.17, 0.19, 1.0)
OBSTACLE_COLOUR = unreal.LinearColor(0.30, 0.33, 0.38, 1.0)

# Key and fill, in lux. The key is the only shadow caster - two shadow-casting directional
# lights put two shadows under every object and they read as extra objects. The rotations are
# the training scene's, so the two maps are lit from the same direction and a screenshot of
# one is comparable to a screenshot of the other.
KEY_INTENSITY = 0.4
KEY_ROTATION = (-55.0, 25.0)
FILL_INTENSITY = 0.1
FILL_ROTATION = (35.0, -140.0)

# The generator's territory. Everything it creates is labelled with this prefix, nothing else in
# the level is, and the prefix is the contract: rename a generated actor to something without it
# and the generator stops touching that actor. Rename one of yours to start with it and you are
# volunteering it for deletion.
GENERATED_PREFIX = "PP_"

# Classes this file is allowed to delete. SkyLight / SkyAtmosphere / PostProcessVolume are still
# listed even though nothing here spawns them any more: an earlier revision did, and they have to
# be cleared before the new lighting can be judged on its own. A prefix match alone would catch
# them, but requiring the class as well means a hand-placed actor of some other type that happens
# to be named PP_* is still kept.
GENERATED_CLASSES = [
    "StaticMeshActor",
    "DirectionalLight",
    "SkyLight",
    "SkyAtmosphere",
    "PostProcessVolume",
    "PlayerStart",
]

# (label, mesh, location, scale, yaw)
# Cube and cylinder are both 100 cm, so scale is metres x 10.
LAYOUT = [
    # floor: 40 m across, top face on z = 0
    ("PP_Floor", CUBE, (0.0, 0.0, -50.0), (40.0, 40.0, 1.0), 0.0),

    # boundary: a 36 m square, waist to chest high. Without it the floor has an edge and the
    # player finds it immediately.
    ("PP_Wall_N", CUBE, (0.0, 1800.0, 150.0), (36.0, 1.6, 3.0), 0.0),
    ("PP_Wall_S", CUBE, (0.0, -1800.0, 150.0), (36.0, 1.6, 3.0), 0.0),
    ("PP_Wall_E", CUBE, (1800.0, 0.0, 150.0), (36.0, 1.6, 3.0), 90.0),
    ("PP_Wall_W", CUBE, (-1800.0, 0.0, 150.0), (36.0, 1.6, 3.0), 90.0),

    # four long blocks around the middle: the main thing to break a straight line
    ("PP_Block_N", CUBE, (0.0, 500.0, 125.0), (6.0, 1.5, 2.5), 0.0),
    ("PP_Block_S", CUBE, (0.0, -500.0, 125.0), (6.0, 1.5, 2.5), 0.0),
    ("PP_Block_E", CUBE, (500.0, 0.0, 125.0), (1.5, 6.0, 2.5), 0.0),
    ("PP_Block_W", CUBE, (-500.0, 0.0, 125.0), (1.5, 6.0, 2.5), 0.0),

    # pillars: rounded obstacles are where the chasers' feelers visibly earn their keep
    ("PP_Pillar_C", CYLINDER, (0.0, 0.0, 150.0), (1.5, 1.5, 3.0), 0.0),
    ("PP_Pillar_NE", CYLINDER, (1000.0, 1000.0, 150.0), (1.2, 1.2, 3.0), 0.0),
    ("PP_Pillar_NW", CYLINDER, (-1000.0, 1000.0, 150.0), (1.2, 1.2, 3.0), 0.0),
    ("PP_Pillar_SE", CYLINDER, (1000.0, -1000.0, 150.0), (1.2, 1.2, 3.0), 0.0),
    ("PP_Pillar_SW", CYLINDER, (-1000.0, -1000.0, 150.0), (1.2, 1.2, 3.0), 0.0),

    # low crates: waist high, so they can be jumped onto and over. Deliberately on the
    # diagonals, not on the axes - the player spawns on the south axis and the follow camera
    # sits 420 cm behind him, which is exactly where a crate used to be. The spring arm would
    # have hauled itself in to avoid clipping it, so the player would have started every round
    # with the camera pressed against the back of his head.
    ("PP_Crate_NE", CUBE, (1350.0, 1350.0, 75.0), (3.0, 3.0, 1.5), 0.0),
    ("PP_Crate_NW", CUBE, (-1350.0, 1350.0, 75.0), (3.0, 3.0, 1.5), 0.0),
    ("PP_Crate_SE", CUBE, (1350.0, -1350.0, 75.0), (3.0, 3.0, 1.5), 0.0),
    ("PP_Crate_SW", CUBE, (-1350.0, -1350.0, 75.0), (3.0, 3.0, 1.5), 0.0),
]

# On the south axis, facing +Y at the middle. Far enough back that the camera behind him has
# clear air all the way to the south wall, and there is still a block to round on the way in.
PLAYER_START = (0.0, -1200.0, 100.0)
PLAYER_START_YAW = 90.0


def log(message):
    unreal.log("[PursuitAI/gen_play] " + str(message))


def fail(message):
    unreal.log_error("[PursuitAI/gen_play] " + str(message))
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
        fail("could not load class " + path + " (is the module compiled and loaded?)")
    return cls


def main(level_path=LEVEL_PATH):
    """Generate, or regenerate, the arena into level_path.

    level_path defaults to the play map. tools/verify_handmade_survives.py passes a throwaway
    level instead, which is the only way to test the delete rule while an editor instance has
    the real map open: the editor holds a lock on a .umap it has loaded, so a headless run that
    tries to save that same map fails with a sharing violation rather than doing anything useful.
    """
    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    library = unreal.EditorAssetLibrary

    if level_editor is None or actors is None:
        fail("editor subsystems unavailable - is this running as an editor commandlet?")

    if not library.does_directory_exist("/Game/Maps"):
        library.make_directory("/Game/Maps")

    if library.does_asset_exist(level_path):
        log("level exists, opening " + level_path)
        if not level_editor.load_level(level_path):
            fail("failed to load " + level_path)
    else:
        log("creating " + level_path)
        if not level_editor.new_level(level_path):
            fail("failed to create " + level_path)

    # --- clear whatever a previous run generated ----------------------------
    # Label AND class, not class alone. See the note on GENERATED_PREFIX: the actors you place
    # by hand in the editor are StaticMeshActors, so matching on class alone would delete a
    # hand-built level without saying anything.
    def is_generated(actor):
        try:
            label = actor.get_actor_label()
        except Exception:
            return False
        if not label.startswith(GENERATED_PREFIX):
            return False
        return actor.get_class().get_name() in GENERATED_CLASSES

    removed = 0
    kept = 0
    for actor in list(actors.get_all_level_actors()):
        if is_generated(actor):
            actors.destroy_actor(actor)
            removed += 1
        elif actor.get_class().get_name() not in ("WorldSettings", "Brush", "LevelBounds"):
            kept += 1
    log("cleared %d generated actor(s); kept %d actor(s) that are not mine" % (removed, kept))

    # --- geometry -----------------------------------------------------------
    cube = library.load_asset(CUBE)
    cylinder = library.load_asset(CYLINDER)
    meshes = {CUBE: cube, CYLINDER: cylinder}
    if cube is None or cylinder is None:
        fail("engine basic shapes are missing; /Engine/BasicShapes should always be there")

    static_mesh_actor_class = load_class("/Script/Engine.StaticMeshActor")

    # --- two shared material instances --------------------------------------
    # Built as assets rather than as one dynamic instance per actor. A dynamic instance cannot
    # be made from python (UMaterialInstanceDynamic::Create is not exposed), and two shared
    # assets are less to carry than eighteen unique ones anyway.
    #
    # Parent and colour are written on every run, not only on creation. That is what makes a
    # re-run after the parent material changes actually take effect - the alternative is that
    # the instance keeps pointing at whatever it was first built from and the level looks
    # unchanged for reasons that are invisible from the outside.
    tint_material = library.load_asset(TINT_MATERIAL)
    if tint_material is None:
        raise RuntimeError(
            "%s is missing - run tools/gen_tint_material.py first" % TINT_MATERIAL)

    def make_material_instance(name, colour):
        path = "/Game/Materials/" + name
        if library.does_asset_exist(path):
            instance = library.load_asset(path)
        else:
            library.make_directory("/Game/Materials")
            factory = unreal.MaterialInstanceConstantFactoryNew()
            tools = unreal.AssetToolsHelpers.get_asset_tools()
            instance = tools.create_asset(name, "/Game/Materials", unreal.MaterialInstanceConstant, factory)
            if instance is None:
                raise RuntimeError("create_asset returned None for " + name)

        unreal.MaterialEditingLibrary.set_material_instance_parent(instance, tint_material)
        unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(instance, "Color", colour)
        library.save_loaded_asset(instance)
        return instance

    floor_material = try_step("MI_PlayFloor", lambda: make_material_instance("MI_PlayFloor", FLOOR_COLOUR))
    obstacle_material = try_step("MI_PlayObstacle", lambda: make_material_instance("MI_PlayObstacle", OBSTACLE_COLOUR))

    if floor_material is None or obstacle_material is None:
        log("tinting unavailable - the level will render in the default grey, which is fine but flat")

    tinted = 0

    for label, mesh_path, location, scale, yaw in LAYOUT:
        spawned = actors.spawn_actor_from_class(
            static_mesh_actor_class,
            unreal.Vector(location[0], location[1], location[2]),
            # Keyword, not positional. unreal.Rotator's positional order is (roll, pitch, yaw),
            # so Rotator(0.0, 90.0, 0.0) - which reads like "yaw 90" to anyone arriving from
            # C++'s FRotator(Pitch, Yaw, Roll) - is actually a 90 degree PITCH, and it stands
            # the actor on its nose. That mistake cost a debug cycle: the east and west walls
            # were tipped over, and the PlayerStart pointed straight up, which put the follow
            # camera inside the player's own capsule looking at an empty sky. Every screenshot
            # was black. The parameter name is the fix; it cannot be got wrong the same way.
            unreal.Rotator(yaw=yaw),
        )
        if spawned is None:
            fail("failed to spawn " + label)

        spawned.set_actor_label(label)
        spawned.set_actor_scale3d(unreal.Vector(scale[0], scale[1], scale[2]))

        component = spawned.get_editor_property("static_mesh_component")
        component.set_editor_property("static_mesh", meshes[mesh_path])
        # Movable so a Movable light lights it without a lighting build.
        component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)

        # Tinting is best effort: a flat colour reads far better than default grey, but a
        # failure here must not stop the level from being generated.
        instance = floor_material if label == "PP_Floor" else obstacle_material
        if instance is not None:
            def tint(c=component, m=instance):
                c.set_material(0, m)
                return m.get_name()

            if try_step("tint " + label, tint) is not None:
                tinted += 1

    log("generated %d obstacle/floor actor(s), %d tinted" % (len(LAYOUT), tinted))

    # --- lights -------------------------------------------------------------
    # Two directional lights, no sky, no post-process volume, auto-exposure left alone. See
    # the note at the top of this file for why that combination and not a brighter sun.
    sun_class = load_class("/Script/Engine.DirectionalLight")

    def spawn_sun(label, rotation, intensity, cast_shadows, priority):
        sun = actors.spawn_actor_from_class(
            sun_class,
            unreal.Vector(0.0, 0.0, 1200.0),
            # Named for the same reason as the layout loop above.
            unreal.Rotator(pitch=rotation[0], yaw=rotation[1]))
        if sun is None:
            raise RuntimeError("failed to spawn " + label)
        sun.set_actor_label(label)

        # DirectionalLightComponent, not LightComponent: forward_shading_priority only exists
        # on the directional flavour. Setting it names which of the two lights wins forward
        # shading, instead of leaving the engine to warn about the ambiguity.
        component = sun.get_editor_property("directional_light_component")
        component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
        component.set_intensity(intensity)
        component.set_editor_property("cast_shadows", cast_shadows)
        component.set_editor_property("forward_shading_priority", priority)
        return sun

    spawn_sun("PP_SunKey", KEY_ROTATION, KEY_INTENSITY, True, 1)
    spawn_sun("PP_SunFill", FILL_ROTATION, FILL_INTENSITY, False, 0)
    log("lighting: key %.2f lux (shadows) at %s, fill %.2f lux at %s, exposure left alone"
        % (KEY_INTENSITY, KEY_ROTATION, FILL_INTENSITY, FILL_ROTATION))

    # --- player start -------------------------------------------------------
    start_class = load_class("/Script/Engine.PlayerStart")
    start = actors.spawn_actor_from_class(
        start_class,
        unreal.Vector(PLAYER_START[0], PLAYER_START[1], PLAYER_START[2]),
        unreal.Rotator(yaw=PLAYER_START_YAW))
    start.set_actor_label("PP_PlayerStart")

    # Read the rotation back rather than trusting it. This is the value the PlayerController
    # adopts as its ControlRotation, and because the follow camera uses the control rotation, a
    # wrong pitch here is not a slightly odd spawn - it is a black screen, since the camera
    # ends up under the player looking at the sky. One line makes that visible in the log.
    actual = start.get_actor_rotation()
    log("player start at %s yaw=%.0f (read back: pitch=%.0f yaw=%.0f roll=%.0f)"
        % (PLAYER_START, PLAYER_START_YAW, actual.pitch, actual.yaw, actual.roll))

    # --- game mode ----------------------------------------------------------
    # The whole reason the play map can be a normal level: the game mode is per-level, so the
    # training map keeps the engine default and never sees a character or a chaser.
    game_mode_class = load_class(GAME_MODE_CLASS)

    def set_via_level_actor():
        for actor in actors.get_all_level_actors():
            if actor.get_class().get_name() == "WorldSettings":
                actor.set_editor_property("default_game_mode", game_mode_class)
                return "world settings actor, now %s" % actor.get_editor_property("default_game_mode")
        raise RuntimeError("WorldSettings not in the level actor list")

    def set_via_world():
        # The WorldSettings actor is not in get_all_level_actors, so this is the route that
        # actually runs. Reading the value straight back matters: if this silently did nothing,
        # the level would still save and open, and the only symptom would be a map where nothing
        # spawns and the keyboard does nothing.
        world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
        settings = world.get_world_settings()
        settings.set_editor_property("default_game_mode", game_mode_class)
        return "editor world, now %s" % settings.get_editor_property("default_game_mode")

    if try_step("set default game mode", set_via_level_actor) is None:
        if try_step("set default game mode (fallback)", set_via_world) is None:
            log("could not set the game mode from python - set it on the level's World Settings by hand")

    # --- persist ------------------------------------------------------------
    if not level_editor.save_current_level():
        fail("save_current_level failed for " + level_path)

    log("OK - %s saved with %d obstacle(s)" % (level_path, len(LAYOUT)))


# Guarded so this file can also be exec'd as a module by tools/verify_handmade_survives.py,
# which calls main() itself after planting an actor for it to *not* delete.
if __name__ == "__main__":
    main()
