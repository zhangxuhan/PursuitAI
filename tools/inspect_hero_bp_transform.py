import unreal

# Reads the exact mesh transform the pack's own blueprints use, so the character copies the
# art team's placement instead of guessing an origin offset and a scale.
BPS = {
    "TinyHero":   "/Game/RPGHeroSquad/CharacterBP/BP_TinyHeroPBR.BP_TinyHeroPBR",
    "RPGHero":    "/Game/RPGHeroSquad/CharacterBP/BP_RPGHeroPBR.BP_RPGHeroPBR",
    "AnimalHero": "/Game/RPGHeroSquad/CharacterBP/BP_AnimalHeroPBR.BP_AnimalHeroPBR",
}

def log(msg):
    unreal.log("PursuitAI/bptr: " + msg)

for label, path in BPS.items():
    bp = unreal.load_object(None, path)
    if bp is None:
        log("%s: BP NOT FOUND at %s" % (label, path))
        continue
    gen = bp.generated_class() if hasattr(bp, "generated_class") else bp
    if gen is None:
        log("%s: BP has no generated_class" % label)
        continue
    cdo = unreal.get_default_object(gen)
    if cdo is None:
        log("%s: no CDO" % label)
        continue
    comp = cdo.get_component_by_class(unreal.SkeletalMeshComponent)
    if comp is None:
        log("%s: no skeletal mesh component on CDO" % label)
        continue
    mesh = comp.get_editor_property("skeletal_mesh")
    loc = comp.get_editor_property("relative_location")
    rot = comp.get_editor_property("relative_rotation")
    scl = comp.get_editor_property("relative_scale3d")
    log("%s: mesh=%s" % (label, mesh.get_path_name() if mesh else "<none>"))
    log("    rel loc = (%.2f, %.2f, %.2f)" % (loc.x, loc.y, loc.z))
    log("    rel rot = (P%.2f, Y%.2f, R%.2f)" % (rot.pitch, rot.yaw, rot.roll))
    log("    rel scl = (%.2f, %.2f, %.2f)" % (scl.x, scl.y, scl.z))
    if mesh:
        b = mesh.get_bounds()
        c = b.get_editor_property("origin")
        e = b.get_editor_property("box_extent")
        log("    bounds center=(%.2f,%.2f,%.2f) extent=(%.2f,%.2f,%.2f) -> height=%.2f"
            % (c.x, c.y, c.z, e.x, e.y, e.z, e.z * 2.0))
