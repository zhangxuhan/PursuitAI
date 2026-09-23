# Prints what L_PursuitPlay actually contains, so "the scene renders black" can be
# narrowed down to "the level has no light" versus "the light is there and something
# else is wrong" without opening the editor.
#
#   UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript \
#       -script="<repo>/tools/inspect_play_level.py" -unattended -nosplash -nullrhi

import unreal

LEVEL_PATH = "/Game/Maps/L_PursuitPlay"


def log(message):
    unreal.log("[PursuitAI/inspect] " + str(message))


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
    for actor in actors:
        label = actor.get_actor_label()
        cls = actor.get_class().get_name()
        loc = actor.get_actor_location()
        log("  %-20s %-22s loc=(%.0f, %.0f, %.0f)" % (label, cls, loc.x, loc.y, loc.z))

    # Lights deserve more than a name: intensity 0 or Static mobility would both explain
    # a level that loads fine and renders nothing.
    for actor in actors:
        cls = actor.get_class().get_name()
        if cls == "DirectionalLight":
            comp = actor.get_editor_property("directional_light_component")
            log("SUN  intensity=%.3f mobility=%s atmosphere_sun=%s"
                % (comp.get_intensity(), comp.get_editor_property("mobility"),
                   comp.get_editor_property("atmosphere_sun_light")))
        elif cls == "SkyLight":
            comp = actor.get_editor_property("light_component")
            log("SKY  intensity=%.3f mobility=%s real_time=%s"
                % (comp.get_intensity(), comp.get_editor_property("mobility"),
                   comp.get_editor_property("real_time_capture")))

    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    settings = world.get_world_settings()
    log("default_game_mode = %s" % settings.get_editor_property("default_game_mode"))


main()
