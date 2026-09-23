# Dumps what the Cartoon City "Demonstration" map actually contains, so "where can the
# chasers round a building" and "what is already an obstacle" are answered from the level
# rather than guessed. Prints actors, their bounds, and the collision-relevant facts.
#
#   UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript \
#       -script="<repo>/tools/inspect_demo_city.py" -unattended -nosplash -nullrhi

import unreal

LEVEL_PATH = "/Game/Cartoon_City_Free/Maps/Demonstration"


def log(message):
    unreal.log("[PursuitAI/city] " + str(message))


def main():
    library = unreal.EditorAssetLibrary
    if not library.does_asset_exist(LEVEL_PATH):
        log("ERROR: %s does not exist" % LEVEL_PATH)
        return

    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level_editor.load_level(LEVEL_PATH):
        log("ERROR: could not load " + LEVEL_PATH)
        return

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    log("level has %d actor(s)" % len(actors))

    # Group by class so the shape of the level is readable at a glance rather than as a
    # 400-line list.
    by_class = {}
    for actor in actors:
        cls = actor.get_class().get_name()
        by_class.setdefault(cls, []).append(actor)

    for cls in sorted(by_class.keys()):
        log("CLASS %-34s count=%d" % (cls, len(by_class[cls])))

    log("--- actor detail ---")
    for actor in actors:
        label = actor.get_actor_label()
        cls = actor.get_class().get_name()
        loc = actor.get_actor_location()
        try:
            origin, extent = actor.get_actor_bounds(False)
            bounds = "loc=(%.0f, %.0f, %.0f) extent=(%.0f, %.0f, %.0f)" % (
                origin.x, origin.y, origin.z, extent.x, extent.y, extent.z)
        except Exception as exc:
            bounds = "bounds unavailable (%s)" % exc
        log("  %-28s %-30s %s" % (label, cls, bounds))

    # Every static mesh actor is a candidate wall, and whether it blocks the chasers is the
    # only thing that matters here, so the collision profile is printed rather than inferred.
    log("--- static mesh collision ---")
    for actor in actors:
        cls = actor.get_class().get_name()
        if "StaticMeshActor" not in cls:
            continue
        comp = actor.get_editor_property("static_mesh_component")
        mesh = comp.get_editor_property("static_mesh")
        collision = comp.get_editor_property("body_instance")
        log("  %-28s mesh=%s collision_enabled=%s" % (
            actor.get_actor_label(),
            mesh.get_name() if mesh else "<none>",
            collision.get_editor_property("collision_enabled") if collision else "<none>"))

    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    settings = world.get_world_settings()
    log("default_game_mode = %s" % settings.get_editor_property("default_game_mode"))

    # The player start is the spawn the chasers ring around, so it is worth stating.
    for actor in actors:
        if actor.get_class().get_name() == "PlayerStart":
            loc = actor.get_actor_location()
            rot = actor.get_actor_rotation()
            log("PLAYERSTART loc=(%.0f, %.0f, %.0f) rot=(pitch=%.1f, yaw=%.1f, roll=%.1f)"
                % (loc.x, loc.y, loc.z, rot.pitch, rot.yaw, rot.roll))


main()
