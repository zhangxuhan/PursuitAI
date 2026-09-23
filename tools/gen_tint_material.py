# Creates /Game/Materials/M_PursuitTint, the one material the characters and the play
# level are both tinted through.
#
#   UnrealEditor-Cmd.exe <project>.uproject -run=pythonscript \
#       -script="<repo>/tools/gen_tint_material.py" -unattended -nosplash -nullrhi
#
# Why this asset has to exist at all: the engine's BasicShapeMaterial has no
# bUsedWithSkeletalMesh usage flag, so a dynamic instance of it applied to a character
# silently falls back to the default material. The log says so -
#
#   Material ... MID_BasicShapeMaterial_0 missing bUsedWithSkeletalMesh=True!
#   Default Material will be used in game.
#
# - and the symptom is a character that renders, but never in the colour you asked for.
# Setting the flag on the engine asset is not an option (it lives in Engine/Content), so
# the project owns its own. One vector parameter, "Color", matching the name the RL
# environment and PursuitCharacter already write to.
#
# Idempotent: an existing asset is left alone rather than rebuilt, so re-running after a
# manual tweak in the material editor does not throw the tweak away. Delete the asset if
# you want it regenerated.

import unreal

ASSET_NAME = "M_PursuitTint"
FOLDER = "/Game/Materials"
ASSET_PATH = FOLDER + "/" + ASSET_NAME
PARAMETER = "Color"

# Neutral grey rather than a colour: whoever instantiates this is expected to set the
# parameter, and a loud default hides the case where nobody did.
DEFAULT_COLOUR = unreal.LinearColor(0.55, 0.55, 0.58, 1.0)

# 0.6 is matte enough to read shape under a single directional light without going
# plasticky. The RL scene's spheres land in the same range.
ROUGHNESS = 0.6


def log(message):
    unreal.log("[PursuitAI/tintmat] " + str(message))


def main():
    library = unreal.EditorAssetLibrary

    if library.does_asset_exist(ASSET_PATH):
        existing = library.load_asset(ASSET_PATH)
        log("already exists: %s (parent-parameter %r, skeletal=%s) - leaving it alone"
            % (ASSET_PATH, PARAMETER,
               existing.get_editor_property("used_with_skeletal_mesh")))
        return

    if not library.does_directory_exist(FOLDER):
        library.make_directory(FOLDER)

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(ASSET_NAME, FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError("create_asset returned None for " + ASSET_PATH)

    # The whole point of the asset. Skeletal because the characters are skeletal meshes;
    # instanced-static as well so the same material can dress anything later.
    material.set_editor_property("used_with_skeletal_mesh", True)
    material.set_editor_property("used_with_instanced_static_meshes", True)
    # Everything in both levels is Movable, so the baked-lighting path is dead weight and
    # costs a shader permutation per platform.
    material.set_editor_property("used_with_static_lighting", False)

    editing = unreal.MaterialEditingLibrary

    colour = editing.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -450, -80)
    colour.set_editor_property("parameter_name", PARAMETER)
    colour.set_editor_property("default_value", DEFAULT_COLOUR)
    editing.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)

    roughness = editing.create_material_expression(
        material, unreal.MaterialExpressionConstant, -450, 120)
    roughness.set_editor_property("r", ROUGHNESS)
    editing.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    editing.layout_material_expressions(material)
    editing.recompile_material(material)

    library.save_loaded_asset(material)

    # Read the flags back. A usage flag that silently did not take produces exactly the
    # warning this asset exists to remove, and it would not show up until the game ran.
    log("created %s: skeletal=%s instanced=%s colour-param=%r roughness=%.2f"
        % (ASSET_PATH,
           material.get_editor_property("used_with_skeletal_mesh"),
           material.get_editor_property("used_with_instanced_static_meshes"),
           PARAMETER, ROUGHNESS))


main()
