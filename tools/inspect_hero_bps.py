"""
Probe the RPG Hero Squad pack and print everything the C++ side needs to know.

The pack ships three heroes with three separate skeletons and three different animation
sets, so "use the pack" is not one decision, it is three. What this answers, per hero:

  * what the BP's parent class is        - whether it could be a pawn at all
  * its mesh component transform         - the scale/offset that makes it stand correctly
  * which skeletal mesh and skeleton     - to prove the clips below belong to it
  * whether an animation blueprint is set - i.e. who drives the pose
  * capsule dimensions                   - to size our own capsule to the character

Every section is wrapped: one unavailable property must not cost us the rest of the report.

Run:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=tools/inspect_hero_bps.py \
      -unattended -nosplash -nullrhi -abslog=logs/inspect_hero.log
"""

import unreal

PACK = "/Game/RPGHeroSquad"
BP_DIR = PACK + "/CharacterBP"
MESH_DIR = PACK + "/Mesh/Character"

HEROES = ["TinyHero", "AnimalHero", "RPGHero"]

# Mesh assets keyed the way the pack names them: the AnimalHero's mesh is called "Dog".
MESH_BY_HERO = {
    "TinyHero": "SK_TinyHeroPBR",
    "AnimalHero": "SK_DogPBR",
    "RPGHero": "SK_RPGHeroPBR",
}

# Every clip we might want, per hero, with the role it would play. Names are the pack's own.
CLIPS = {
    "TinyHero": [
        ("idle", "Animation/TinyHero/Anim_Idle_Normal_TinyHero"),
        ("walk", "Animation/TinyHero/InPlace/Anim_MoveFWD_Normal_InPlace_TinyHero"),
        ("run", "Animation/TinyHero/InPlace/Anim_MoveFWD_Battle_InPlace_TinyHero"),
        ("sprint", "Animation/TinyHero/InPlace/Anim_SprintFWD_Battle_InPlace_TinyHero"),
        ("jump_start", "Animation/TinyHero/InPlace/Anim_JumpStart_Normal_InPlace_TinyHero"),
        ("jump_air", "Animation/TinyHero/InPlace/Anim_JumpAir_Normal_InPlace_TinyHero"),
        ("jump_end", "Animation/TinyHero/InPlace/Anim_JumpEnd_Normal_InPlace_TinyHero"),
        ("jump_full", "Animation/TinyHero/InPlace/Anim_JumpFull_Normal_InPlace_TinyHero"),
    ],
    "AnimalHero": [
        ("idle", "Animation/AnimalHero/Anim_Idle_Battle_AnimalHero"),
        ("walk", "Animation/AnimalHero/InPlace/Anim_WalkForwardBattle_IP_AnimalHero"),
        ("run", "Animation/AnimalHero/InPlace/Anim_RunForwardBattle_IP_AnimalHero"),
        ("sprint", "Animation/AnimalHero/InPlace/Anim_SprintForwardBattle_IP_AnimalHero"),
    ],
    "RPGHero": [
        ("idle", "Animation/RPGHero/Anim_Idle_RPGHero"),
        ("walk", "Animation/RPGHero/InPlace/Anim_Walk_IP_RPGHero"),
        ("run", "Animation/RPGHero/InPlace/Anim_Run_IP_RPGHero"),
        ("sprint", "Animation/RPGHero/InPlace/Anim_Sprint_IP_RPGHero"),
    ],
}

# What the chasers will actually need, in order, once the player is on TinyHero.
CHASER_PLAN = ["AnimalHero", "RPGHero", "AnimalHero", "RPGHero"]


def log(message):
    unreal.log("PursuitAI/heroes: " + str(message))


def short(path):
    """'/Game/A/B.C' -> '/Game/A/B'."""
    return path.split(".")[0] if path else "<none>"


def attempt(label, function, formatter=str):
    """Run one probe; on failure print the reason instead of aborting the report."""
    try:
        return function()
    except Exception as error:
        log("        %-14s <error: %s>" % (label, error))
        return None


def get(obj, prop, default=None):
    """Editor property read that never raises."""
    try:
        return obj.get_editor_property(prop)
    except Exception:
        return default


def describe_transform(component):
    location = get(component, "relative_location")
    rotation = get(component, "relative_rotation")
    scale = get(component, "relative_scale3d")
    if location is None or rotation is None or scale is None:
        return "<transform unavailable>"
    return ("loc=(%.1f, %.1f, %.1f) rot=(pitch %.1f, yaw %.1f, roll %.1f) "
            "scale=(%.4f, %.4f, %.4f)"
            % (location.x, location.y, location.z,
               rotation.pitch, rotation.yaw, rotation.roll,
               scale.x, scale.y, scale.z))


def components_of(blueprint):
    """Every component the blueprint owns, via the subobject data subsystem."""
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    library = unreal.SubobjectDataBlueprintFunctionLibrary
    found = []
    for handle in subsystem.k2_gather_subobject_data_for_blueprint(blueprint):
        obj = library.get_object(handle)
        if obj is None:
            continue
        found.append((str(library.get_variable_name(handle)), obj))
    return found


def describe_component(name, obj):
    cls = obj.get_class().get_name()
    log("      [%-24s] %s" % (name, cls))
    log("        transform      : %s" % describe_transform(obj))

    if isinstance(obj, unreal.SkeletalMeshComponent):
        mesh = attempt("mesh", lambda: get(obj, "skeletal_mesh"))
        log("        mesh           : %s" % (short(mesh.get_path_name()) if mesh else "<none>"))
        anim_class = attempt("anim class", lambda: get(obj, "anim_class"))
        log("        anim class     : %s" % (anim_class.get_name() if anim_class else "<none>"))
        log("        anim mode      : %s" % describe_anim_mode(obj))
        for index, material in enumerate(get(obj, "override_materials") or []):
            if material:
                log("        override_mat[%d]: %s" % (index, short(material.get_path_name())))

    if isinstance(obj, unreal.CapsuleComponent):
        log("        half height    : %s" % attempt("half height", lambda: get(obj, "capsule_half_height")))
        log("        radius         : %s" % attempt("radius", lambda: get(obj, "capsule_radius")))


def describe_anim_mode(component):
    """'AnimationSingleNode' vs 'AnimationBlueprint' - i.e. who owns the pose."""
    try:
        mode = component.get_editor_property("animation_mode")
        return str(mode)
    except Exception as error:
        return "<error: %s>" % error


def inspect_blueprint(hero):
    path = "%s/BP_%sPBR.BP_%sPBR" % (BP_DIR, hero, hero)
    log("=" * 78)
    log("BP   " + path)
    blueprint = unreal.load_asset(path)
    if blueprint is None:
        log("  MISSING")
        return

    generated = blueprint.generated_class()
    log("  generated class : %s" % generated.get_name())
    # UBlueprint::ParentClass is not exposed to get_editor_property in 5.7; the generated
    # class's super class is the same answer and is always reachable.
    log("  parent class    : %s" % attempt("parent", lambda: generated.get_super_class().get_name()))
    log("  parent path     : %s" % attempt("parent path", lambda: generated.get_super_class().get_path_name()))

    # Independent cross-check straight out of the asset registry.
    data = unreal.EditorAssetLibrary.find_asset_data(path)
    log("  registry parent : %s" % attempt("registry", lambda: data.get_tag_value("ParentClass")))

    try:
        for name, obj in components_of(blueprint):
            describe_component(name, obj)
    except Exception as error:
        log("  <component walk failed: %s>" % error)


def inspect_mesh(hero):
    name = MESH_BY_HERO[hero]
    path = "%s/%s.%s" % (MESH_DIR, name, name)
    log("=" * 78)
    log("MESH %s (%s)" % (name, hero))
    mesh = unreal.load_asset(path)
    if mesh is None:
        log("  MISSING " + path)
        return

    skeleton = attempt("skeleton", lambda: get(mesh, "skeleton"))
    log("  skeleton      : %s" % (short(skeleton.get_path_name()) if skeleton else "<none>"))

    # Bounds tell us the character's real size, which is what the capsule has to match.
    def bounds():
        box = mesh.get_bounds()
        return ("origin=(%.1f, %.1f, %.1f) extent=(%.1f, %.1f, %.1f) "
                "-> height %.1f cm, width %.1f cm"
                % (box.origin.x, box.origin.y, box.origin.z,
                   box.box_extent.x, box.box_extent.y, box.box_extent.z,
                   box.box_extent.z * 2.0, box.box_extent.y * 2.0))

    log("  bounds        : %s" % attempt("bounds", bounds))
    log("  import scale  : %s" % attempt("import scale", lambda: get(mesh, "imported_scale_multiplier")))

    for index, slot in enumerate(get(mesh, "materials") or []):
        material = get(slot, "material_interface")
        log("  material[%d]   : %s" % (index, short(material.get_path_name()) if material else "<none>"))


def inspect_clips(hero):
    log("=" * 78)
    log("CLIPS %s" % hero)
    for role, relative in CLIPS[hero]:
        name = relative.split("/")[-1]
        path = "%s/%s.%s" % (PACK, relative, name)
        clip = unreal.load_asset(path)
        if clip is None:
            log("  %-11s MISSING %s" % (role, path))
            continue
        skeleton = get(clip, "skeleton")
        length = attempt("length", clip.get_play_length)
        # A root-motion clip would fight a capsule we move ourselves; InPlace must not have it.
        root_motion = get(clip, "enable_root_motion")
        log("  %-11s %-50s len=%s skel=%-22s rootMotion=%s"
            % (role, name,
               ("%6.2fs" % length) if isinstance(length, float) else str(length),
               skeleton.get_name() if skeleton else "<none>", root_motion))


def main():
    log("RPG Hero Squad probe - %d hero(es)" % len(HEROES))
    for hero in HEROES:
        try:
            inspect_blueprint(hero)
        except Exception as error:
            log("BP %s <failed: %s>" % (hero, error))
    for hero in HEROES:
        try:
            inspect_mesh(hero)
        except Exception as error:
            log("MESH %s <failed: %s>" % (hero, error))
    for hero in HEROES:
        try:
            inspect_clips(hero)
        except Exception as error:
            log("CLIPS %s <failed: %s>" % (hero, error))

    log("=" * 78)
    log("CHASER PLAN - %d chaser(s): %s" % (len(CHASER_PLAN), ", ".join(CHASER_PLAN)))
    log("probe done")


main()
