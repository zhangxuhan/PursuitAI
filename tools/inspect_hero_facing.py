"""
Work out which way each hero's mesh faces, so the capsule's +X can be lined up with it.

Why this is worth a script: the mesh is authored in whatever axis the vendor modelled it
in, and the three heroes come from one pack but are not guaranteed to agree. Getting it
wrong is not subtle - the character runs sideways for the whole demo - and the only other
way to find out is to open a screenshot and guess.

The measurement, done on the reference pose at frame 0 (a rig's facing does not change
with the animation):

  * which axis the arms span        - a T-pose spreads the hands along the span axis,
                                      which tells us the *perpendicular* axis is forward
  * which way the toes point        - the ball/toe bone sits forward of the ankle, so the
                                      sign of its forward-axis offset IS the facing

No actor is spawned and no level is touched; this reads the animation's own bone tracks.

Run:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=tools/inspect_hero_facing.py \
      -unattended -nosplash -nullrhi -abslog=logs/inspect_facing.log
"""

import unreal

PACK = "/Game/RPGHeroSquad"

# One clip per hero, chosen because it exists and is the one the character will idle on.
IDLE_CLIP = {
    "TinyHero": "Animation/TinyHero/Anim_Idle_Normal_TinyHero",
    "AnimalHero": "Animation/AnimalHero/Anim_Idle_Battle_AnimalHero",
    "RPGHero": "Animation/RPGHero/Anim_Idle_RPGHero",
}

# Bone name fragments, by the question they answer.
SPAN_HINTS = ("hand", "wrist", "clavicle", "shoulder", "upperarm", "lowerarm", "arm")
FOOT_HINTS = ("ball", "toe", "foot", "ankle")
FACE_HINTS = ("head", "nose", "snout", "jaw", "muzzle", "ear", "tail")


def log(message):
    unreal.log("PursuitAI/facing: " + str(message))


def clip_path(hero):
    relative = IDLE_CLIP[hero]
    return "%s/%s.%s" % (PACK, relative, relative.split("/")[-1])


def describe(pose, library, bone):
    try:
        transform = library.get_bone_pose(pose, bone)
    except Exception as error:
        return None, "<pose failed: %s>" % error
    t = transform.translation
    return t, "x=%+8.2f  y=%+8.2f  z=%+8.2f" % (t.x, t.y, t.z)


def inspect(hero):
    log("=" * 78)
    log("HERO %s" % hero)
    clip = unreal.load_asset(clip_path(hero))
    if clip is None:
        log("  MISSING " + clip_path(hero))
        return

    library = unreal.AnimPoseExtensions
    try:
        # The evaluation options argument has no default in python, so it has to be built.
        # Defaults are what we want: no retargeting, no additive base, no root motion strip.
        pose = library.get_anim_pose_at_frame(clip, 0, unreal.AnimPoseEvaluationOptions())
    except Exception as error:
        log("  <get_anim_pose_at_frame failed: %s>" % error)
        return
    if not library.is_valid(pose):
        log("  pose is not valid")
        return

    bones = [str(b) for b in library.get_bone_names(pose)]
    log("  bones: %d" % len(bones))
    log("  all: %s" % ", ".join(bones))

    # Where every bone sits, so we can find which axis the rig is widest along.
    table = []
    for bone in bones:
        translation, text = describe(pose, library, bone)
        if translation is not None:
            table.append((bone, translation))

    if not table:
        log("  no readable bone poses")
        return

    def extreme(pick):
        bone, translation = max(table, key=pick)
        return bone, translation

    wide_bone, wide = extreme(lambda item: abs(item[1].x))
    deep_bone, deep = extreme(lambda item: abs(item[1].y))
    tall_bone, tall = extreme(lambda item: item[1].z)
    log("  widest on X (%.1f) : %s" % (wide.x, wide_bone))
    log("  deepest on Y (%.1f): %s" % (deep.y, deep_bone))
    log("  highest on Z (%.1f): %s" % (tall.z, tall_bone))

    log("  -- arm span (which axis the hands spread along) --")
    for bone, translation in table:
        lower = bone.lower()
        if any(hint in lower for hint in SPAN_HINTS) and ("hand" in lower or "wrist" in lower):
            log("     %-28s x=%+8.2f  y=%+8.2f  z=%+8.2f" % (bone, translation.x, translation.y, translation.z))

    log("  -- feet (the toe's offset from the ankle is the facing) --")
    for bone, translation in table:
        if any(hint in bone.lower() for hint in FOOT_HINTS):
            log("     %-28s x=%+8.2f  y=%+8.2f  z=%+8.2f" % (bone, translation.x, translation.y, translation.z))

    log("  -- face / extremities --")
    for bone, translation in table:
        if any(hint in bone.lower() for hint in FACE_HINTS):
            log("     %-28s x=%+8.2f  y=%+8.2f  z=%+8.2f" % (bone, translation.x, translation.y, translation.z))

    log("  -- sockets --")
    try:
        log("     %s" % ", ".join(str(s) for s in library.get_socket_names(pose)))
    except Exception as error:
        log("     <socket names failed: %s>" % error)


def main():
    log("hero facing probe")
    for hero in ("TinyHero", "AnimalHero", "RPGHero"):
        try:
            inspect(hero)
        except Exception as error:
            log("HERO %s <failed: %s>" % (hero, error))
    log("facing probe done")


main()
