import unreal

LEVEL = "/Game/Cartoon_City_Free/Maps/Demonstration_Train"

le = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
asys = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

if not le.load_level(LEVEL):
    raise RuntimeError("load failed")

for a in asys.get_all_level_actors():
    name = a.get_class().get_name()
    if name in ("PursuitCharEnv", "PursuitCharAgent"):
        loc = a.get_actor_location()
        rot = a.get_actor_rotation()
        unreal.log("[PursuitAI/inspect_city_level] %s (%s) loc=(%.0f, %.0f, %.0f) rot=(%.0f, %.0f, %.0f) label=%s" % (
            name, a.get_name(), loc.x, loc.y, loc.z, rot.roll, rot.pitch, rot.yaw, a.get_actor_label()))
        if name == "PursuitCharEnv":
            unreal.log("[PursuitAI/inspect_city_level]   bStartOnCityStage=%s bBuildArenaRig=%s chaser=%s evader=%s" % (
                a.get_editor_property("bStartOnCityStage"),
                a.get_editor_property("bBuildArenaRig"),
                a.get_editor_property("ChaserAgent"),
                a.get_editor_property("EvaderAgent")))
